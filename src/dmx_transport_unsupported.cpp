#include "dmxwb/dmx_transport.hpp"

#include <string>

namespace dmxwb {

DmxTransport::DmxTransport(std::string port) : port_(std::move(port)) {}

DmxTransport::~DmxTransport() = default;

bool DmxTransport::open() {
    set_error("DMX serial transport is supported only on Linux; run the hardware test on Wiren Board");
    return false;
}

void DmxTransport::close() noexcept {}

bool DmxTransport::is_open() const noexcept {
    return false;
}

std::string_view DmxTransport::port() const noexcept {
    return port_;
}

std::string_view DmxTransport::last_error() const noexcept {
    return last_error_;
}

bool DmxTransport::send_frame(const DmxFrameView&) {
    set_error("DMX serial transport is supported only on Linux; run the hardware test on Wiren Board");
    return false;
}

bool DmxTransport::configure_data_mode() {
    return false;
}

bool DmxTransport::configure_break_mode() {
    return false;
}

bool DmxTransport::write_byte(std::uint8_t) {
    return false;
}

bool DmxTransport::write_frame_bytes(const DmxFrameView&) {
    return false;
}

bool DmxTransport::drain_output() {
    return false;
}

bool DmxTransport::fail_with_errno(std::string_view) {
    return false;
}

void DmxTransport::set_error(std::string message) {
    last_error_ = std::move(message);
}

}  // namespace dmxwb
