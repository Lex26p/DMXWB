#include "dmxwb/dmx_transport.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/serial.h>
#include <string>
#include <time.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

namespace dmxwb {
namespace {

constexpr unsigned int kDataBaud = 250000U;
constexpr unsigned int kLegacyBreakBaud = 38400U;
constexpr std::uint32_t kDmxBreakHoldUs = 120U;
constexpr std::uint32_t kDmxMarkAfterBreakUs = 20U;
constexpr std::uint32_t kTemtPollSleepUs = 25U;
constexpr std::uint32_t kTemtTimeoutUs = 100000U;

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

struct DmxTransport::PlatformState final {
    serial_rs485 original_rs485{};
    bool original_rs485_saved{false};
    bool fast_dmx_mode{false};
};

DmxTransport::DmxTransport(std::string port)
    : port_(std::move(port)),
      platform_(std::make_unique<PlatformState>()) {}

DmxTransport::~DmxTransport() {
    close();
}

bool DmxTransport::open() {
    close();
    last_error_.clear();
    platform_->original_rs485_saved = false;
    platform_->fast_dmx_mode = false;

    fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        return fail_with_errno("open");
    }

    if (!configure_data_mode()) {
        close();
        return false;
    }

    if (::ioctl(fd_, TCFLSH, TCIOFLUSH) != 0) {
        (void)fail_with_errno("tcflush");
        close();
        return false;
    }

    // Prefer the hardware-BREAK/manual-DE path proven on WB8.5/T507. If the
    // required standard Linux ioctls are unavailable, retain the DEV-003
    // 38400-baud BREAK method as a compatibility fallback for other WB8 ports.
    (void)try_enable_fast_dmx_mode();
    last_error_.clear();
    return true;
}

void DmxTransport::close() noexcept {
    if (fd_ >= 0) {
        if (platform_ && platform_->fast_dmx_mode) {
            best_effort_fast_cleanup();
        }
        restore_rs485_mode();
        (void)::close(fd_);
        fd_ = -1;
    }
    if (platform_) {
        platform_->fast_dmx_mode = false;
        platform_->original_rs485_saved = false;
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

    if (platform_->fast_dmx_mode) {
        return send_frame_fast(frame);
    }
    return send_frame_legacy(frame);
}

bool DmxTransport::configure_data_mode() {
    if (!configure_line(fd_, kDataBaud)) {
        return fail_with_errno("configure 250000 8N2");
    }
    return true;
}

bool DmxTransport::configure_break_mode() {
    if (!configure_line(fd_, kLegacyBreakBaud)) {
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

bool DmxTransport::try_enable_fast_dmx_mode() {
    serial_rs485 original{};
    if (::ioctl(fd_, TIOCGRS485, &original) != 0) {
        return false;
    }

    int lsr = 0;
    if (::ioctl(fd_, TIOCSERGETLSR, &lsr) != 0) {
        return false;
    }

    platform_->original_rs485 = original;
    platform_->original_rs485_saved = true;

    auto disabled = original;
    disabled.flags &= ~static_cast<decltype(disabled.flags)>(SER_RS485_ENABLED);
    if (::ioctl(fd_, TIOCSRS485, &disabled) != 0) {
        platform_->original_rs485_saved = false;
        return false;
    }

    std::string probe_error;
    const auto saved_error = last_error_;
    if (!set_manual_de(false)) {
        probe_error = last_error_;
    } else if (!set_hardware_break(true)) {
        probe_error = last_error_;
    } else if (!set_hardware_break(false)) {
        probe_error = last_error_;
    }

    if (!probe_error.empty()) {
        (void)::ioctl(fd_, TIOCCBRK);
        int bits = TIOCM_RTS;
        (void)::ioctl(fd_, TIOCMBIC, &bits);
        (void)::ioctl(fd_, TIOCSRS485, &platform_->original_rs485);
        platform_->original_rs485_saved = false;
        platform_->fast_dmx_mode = false;
        last_error_ = saved_error;
        return false;
    }

    platform_->fast_dmx_mode = true;
    last_error_ = saved_error;
    return true;
}

void DmxTransport::restore_rs485_mode() noexcept {
    if (fd_ < 0 || !platform_ || !platform_->original_rs485_saved) {
        return;
    }
    (void)::ioctl(fd_, TIOCSRS485, &platform_->original_rs485);
}

bool DmxTransport::send_frame_fast(const DmxFrameView& frame) {
    if (!set_manual_de(true)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!set_hardware_break(true)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!sleep_microseconds(kDmxBreakHoldUs)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!set_hardware_break(false)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!sleep_microseconds(kDmxMarkAfterBreakUs)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!write_frame_bytes(frame)) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!wait_transmitter_empty()) {
        best_effort_fast_cleanup();
        return false;
    }
    if (!set_manual_de(false)) {
        best_effort_fast_cleanup();
        return false;
    }
    return true;
}

bool DmxTransport::send_frame_legacy(const DmxFrameView& frame) {
    // Compatibility fallback: Wiren Board DEV-003 userspace BREAK proof method.
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

bool DmxTransport::set_manual_de(bool enabled) {
    int bits = TIOCM_RTS;
    const unsigned long request = enabled ? TIOCMBIS : TIOCMBIC;
    if (::ioctl(fd_, request, &bits) != 0) {
        return fail_with_errno(enabled ? "assert manual RS485 DE/RTS" : "deassert manual RS485 DE/RTS");
    }
    return true;
}

bool DmxTransport::set_hardware_break(bool enabled) {
    const unsigned long request = enabled ? TIOCSBRK : TIOCCBRK;
    if (::ioctl(fd_, request) != 0) {
        return fail_with_errno(enabled ? "enable hardware BREAK" : "disable hardware BREAK");
    }
    return true;
}

bool DmxTransport::wait_transmitter_empty() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds{kTemtTimeoutUs};

    while (std::chrono::steady_clock::now() < deadline) {
        int queued = 0;
        if (::ioctl(fd_, TIOCOUTQ, &queued) != 0) {
            return fail_with_errno("query serial output queue");
        }

        int lsr = 0;
        if (::ioctl(fd_, TIOCSERGETLSR, &lsr) != 0) {
            return fail_with_errno("query serial transmitter empty");
        }

        if (queued == 0 && (lsr & TIOCSER_TEMT) != 0) {
            return true;
        }

        if (!sleep_microseconds(kTemtPollSleepUs)) {
            return false;
        }
    }

    set_error("serial transmitter-empty timeout on " + port_);
    return false;
}

bool DmxTransport::sleep_microseconds(std::uint32_t microseconds) {
    timespec request{};
    request.tv_sec = static_cast<time_t>(microseconds / 1000000U);
    request.tv_nsec = static_cast<long>(microseconds % 1000000U) * 1000L;

    for (;;) {
        timespec remaining{};
        const int result = ::clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remaining);
        if (result == 0) {
            return true;
        }
        if (result == EINTR) {
            request = remaining;
            continue;
        }
        set_error("clock_nanosleep failed on " + port_ + ": " + std::strerror(result));
        return false;
    }
}

void DmxTransport::best_effort_fast_cleanup() noexcept {
    if (fd_ < 0) {
        return;
    }
    (void)::ioctl(fd_, TIOCCBRK);
    int bits = TIOCM_RTS;
    (void)::ioctl(fd_, TIOCMBIC, &bits);
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
