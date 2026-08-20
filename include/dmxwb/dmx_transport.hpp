#pragma once

#include "dmxwb/dmx_snapshot.hpp"

#include <string>
#include <string_view>

namespace dmxwb {

inline constexpr std::string_view kDefaultDmxPort = "/dev/ttyRS485-1";

class DmxTransport final {
public:
    explicit DmxTransport(std::string port = std::string{kDefaultDmxPort});
    ~DmxTransport();

    DmxTransport(const DmxTransport&) = delete;
    DmxTransport& operator=(const DmxTransport&) = delete;
    DmxTransport(DmxTransport&&) = delete;
    DmxTransport& operator=(DmxTransport&&) = delete;

    [[nodiscard]] bool open();
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::string_view port() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;

    [[nodiscard]] bool send_frame(const DmxFrameView& frame);

private:
    [[nodiscard]] bool configure_data_mode();
    [[nodiscard]] bool configure_break_mode();
    [[nodiscard]] bool write_byte(std::uint8_t value);
    [[nodiscard]] bool write_frame_bytes(const DmxFrameView& frame);
    [[nodiscard]] bool drain_output();
    [[nodiscard]] bool fail_with_errno(std::string_view operation);
    void set_error(std::string message);

    std::string port_;
    std::string last_error_;
    int fd_{-1};
};

}  // namespace dmxwb
