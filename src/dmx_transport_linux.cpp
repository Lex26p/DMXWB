#include "dmxwb/dmx_transport.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

namespace dmxwb {
namespace {

[[nodiscard]] bool configure_line(int fd, unsigned int baud) noexcept {
    termios2 settings{};
    if (::ioctl(fd, TCGETS2, &settings) != 0) {
        return false;
    }

    settings.c_iflag = 0;
    settings.c_oflag = 0;
    settings.c_lflag = 0;
    settings.c_cflag &= ~(CBAUD | CSIZE | PARENB | PARODD | CRTSCTS);
    settings.c_cflag |= BOTHER | CS8 | CSTOPB | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    settings.c_ispeed = baud;
    settings.c_ospeed = baud;

    return ::ioctl(fd, TCSETS2, &settings) == 0;
}

[[nodiscard]] bool write_all(int fd, const std::uint8_t* data, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const auto remaining = size - offset;
        const auto written = ::write(fd, data + offset, remaining);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written == 0) {
            errno = EIO;
        }
        return false;
    }
    return true;
}

}  // namespace

DmxTransport::DmxTransport(std::string port) : port_(std::move(port)) {}

DmxTransport::~DmxTransport() {
    close();
}

bool DmxTransport::open() {
    close();
    last_error_.clear();

    fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        return fail_with_errno("open");
    }

    if (!configure_data_mode()) {
        close();
        return false;
    }

    if (::ioctl(fd_, TCFLSH, TCIOFLUSH) != 0) {
        const auto failed = fail_with_errno("tcflush");
        close();
        return failed;
    }

    return true;
}

void DmxTransport::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

bool DmxTransport::is_open() const noexcept {
    return fd_ >= 0;
}

std::string_view DmxTransport::port() const noexcept {
    return port_;
}

std::string_view DmxTransport::last_error() const noexcept {
    return last_error_;
}

bool DmxTransport::send_frame(const DmxFrameView& frame) {
    if (!is_open()) {
        set_error("serial port is not open");
        return false;
    }
    if (frame.channels.size() > kDmxMaxChannels) {
        set_error("DMX frame exceeds 512 channels");
        return false;
    }

    // Wiren Board userspace BREAK proof method:
    // 38400 8N2 + 0x00 + wait until physically transmitted, then back to 250000 8N2.
    if (!configure_break_mode()) {
        return false;
    }
    if (!write_byte(0x00)) {
        return false;
    }
    if (!drain_output()) {
        return false;
    }
    if (!configure_data_mode()) {
        return false;
    }
    if (!write_frame_bytes(frame)) {
        return false;
    }
    if (!drain_output()) {
        return false;
    }

    return true;
}

bool DmxTransport::configure_data_mode() {
    if (!configure_line(fd_, 250000U)) {
        return fail_with_errno("configure 250000 8N2");
    }
    return true;
}

bool DmxTransport::configure_break_mode() {
    if (!configure_line(fd_, 38400U)) {
        return fail_with_errno("configure 38400 8N2 BREAK mode");
    }
    return true;
}

bool DmxTransport::write_byte(std::uint8_t value) {
    if (!write_all(fd_, &value, 1)) {
        return fail_with_errno("write BREAK byte");
    }
    return true;
}

bool DmxTransport::write_frame_bytes(const DmxFrameView& frame) {
    std::array<std::uint8_t, kDmxMaxChannels + 1> packet{};
    packet[0] = frame.start_code;
    for (std::size_t index = 0; index < frame.channels.size(); ++index) {
        packet[index + 1] = frame.channels[index];
    }

    const auto packet_size = frame.channels.size() + 1;
    if (!write_all(fd_, packet.data(), packet_size)) {
        return fail_with_errno("write DMX frame");
    }
    return true;
}

bool DmxTransport::drain_output() {
    while (::ioctl(fd_, TCSBRK, 1) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return fail_with_errno("tcdrain");
    }
    return true;
}

bool DmxTransport::fail_with_errno(std::string_view operation) {
    const int error_number = errno;
    set_error(std::string{operation} + " failed on " + port_ + ": " + std::strerror(error_number));
    return false;
}

void DmxTransport::set_error(std::string message) {
    last_error_ = std::move(message);
}

}  // namespace dmxwb
