#pragma once

#include "dmxwb/dmx_snapshot.hpp"

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
