#pragma once

#include "dmxwb/dmx_snapshot.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace dmxwb {

inline constexpr std::string_view kDefaultDmxPort = "/dev/ttyRS485-1";

class DmxTransportInterface {
public:
    virtual ~DmxTransportInterface() = default;

    [[nodiscard]] virtual bool open() = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual std::string_view port() const noexcept = 0;
    [[nodiscard]] virtual std::string_view last_error() const noexcept = 0;

    [[nodiscard]] virtual bool send_frame(const DmxFrameView& frame) = 0;
};

class DmxTransport final : public DmxTransportInterface {
public:
    explicit DmxTransport(std::string port = std::string{kDefaultDmxPort});
    ~DmxTransport() override;

    DmxTransport(const DmxTransport&) = delete;
    DmxTransport& operator=(const DmxTransport&) = delete;
    DmxTransport(DmxTransport&&) = delete;
    DmxTransport& operator=(DmxTransport&&) = delete;

    [[nodiscard]] bool open() override;
    void close() noexcept override;

    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] std::string_view port() const noexcept override;
    [[nodiscard]] std::string_view last_error() const noexcept override;

    [[nodiscard]] bool send_frame(const DmxFrameView& frame) override;

private:
    struct PlatformState;

    [[nodiscard]] bool configure_data_mode();
    [[nodiscard]] bool configure_break_mode();
    [[nodiscard]] bool write_byte(std::uint8_t value);
    [[nodiscard]] bool write_frame_bytes(const DmxFrameView& frame);
    [[nodiscard]] bool drain_output();

    [[nodiscard]] bool try_enable_fast_dmx_mode();
    void restore_rs485_mode() noexcept;
    [[nodiscard]] bool send_frame_fast(const DmxFrameView& frame);
    [[nodiscard]] bool send_frame_legacy(const DmxFrameView& frame);
    [[nodiscard]] bool set_manual_de(bool enabled);
    [[nodiscard]] bool set_hardware_break(bool enabled);
    [[nodiscard]] bool wait_transmitter_empty();
    [[nodiscard]] bool sleep_microseconds(std::uint32_t microseconds);
    void best_effort_fast_cleanup() noexcept;

    [[nodiscard]] bool fail_with_errno(std::string_view operation);
    void set_error(std::string message);

    std::string port_;
    std::string last_error_;
    std::unique_ptr<PlatformState> platform_;
    int fd_{-1};
};

}  // namespace dmxwb
