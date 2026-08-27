#pragma once

#include "dmxwb/artnet_runtime.hpp"

#include <cstdint>
#include <optional>

namespace dmxwb {

class LinuxArtNetDatagramTransport final : public IArtNetDatagramTransport {
public:
    LinuxArtNetDatagramTransport() = default;
    ~LinuxArtNetDatagramTransport() override;

    LinuxArtNetDatagramTransport(const LinuxArtNetDatagramTransport&) = delete;
    LinuxArtNetDatagramTransport& operator=(const LinuxArtNetDatagramTransport&) = delete;
    LinuxArtNetDatagramTransport(LinuxArtNetDatagramTransport&&) = delete;
    LinuxArtNetDatagramTransport& operator=(LinuxArtNetDatagramTransport&&) = delete;

    [[nodiscard]] bool open_and_bind(std::uint16_t port) noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] ArtNetTransportReceiveResult receive(
        std::span<std::uint8_t> buffer) noexcept override;
    [[nodiscard]] bool send_to(
        ArtNetIpv4Address destination,
        std::uint16_t port,
        std::span<const std::uint8_t> payload) noexcept override;
    void close() noexcept override;

    [[nodiscard]] std::optional<std::uint16_t> bound_port() const noexcept;

private:
    int socket_fd_{-1};
    std::optional<std::uint16_t> bound_port_;
};

}  // namespace dmxwb
