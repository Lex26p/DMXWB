#include "dmxwb/artnet_transport_linux.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dmxwb {
namespace {

[[nodiscard]] ArtNetIpv4Address from_in_addr(in_addr address) noexcept {
    const auto host = ntohl(address.s_addr);
    return ArtNetIpv4Address{{
        static_cast<std::uint8_t>((host >> 24U) & 0xffU),
        static_cast<std::uint8_t>((host >> 16U) & 0xffU),
        static_cast<std::uint8_t>((host >> 8U) & 0xffU),
        static_cast<std::uint8_t>(host & 0xffU)}};
}

[[nodiscard]] in_addr to_in_addr(ArtNetIpv4Address address) noexcept {
    const auto host =
        (static_cast<std::uint32_t>(address.octets[0]) << 24U) |
        (static_cast<std::uint32_t>(address.octets[1]) << 16U) |
        (static_cast<std::uint32_t>(address.octets[2]) << 8U) |
        static_cast<std::uint32_t>(address.octets[3]);
    return in_addr{htonl(host)};
}

[[nodiscard]] bool retryable_receive_error(int error_number) noexcept {
    return error_number == EAGAIN || error_number == EWOULDBLOCK;
}

}  // namespace

LinuxArtNetDatagramTransport::~LinuxArtNetDatagramTransport() {
    close();
}

bool LinuxArtNetDatagramTransport::open_and_bind(std::uint16_t port) noexcept {
    if (is_open()) {
        return false;
    }

    const int socket_fd = ::socket(
        AF_INET,
        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_UDP);
    if (socket_fd < 0) {
        return false;
    }

    const int packet_info_enabled = 1;
    if (::setsockopt(
            socket_fd,
            IPPROTO_IP,
            IP_PKTINFO,
            &packet_info_enabled,
            static_cast<socklen_t>(sizeof(packet_info_enabled))) != 0) {
        ::close(socket_fd);
        return false;
    }

    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(port);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&local_address),
            static_cast<socklen_t>(sizeof(local_address))) != 0) {
        ::close(socket_fd);
        return false;
    }

    sockaddr_in bound_address{};
    socklen_t bound_address_size = static_cast<socklen_t>(sizeof(bound_address));
    if (::getsockname(
            socket_fd,
            reinterpret_cast<sockaddr*>(&bound_address),
            &bound_address_size) != 0 ||
        bound_address.sin_family != AF_INET) {
        ::close(socket_fd);
        return false;
    }

    socket_fd_ = socket_fd;
    bound_port_ = ntohs(bound_address.sin_port);
    return true;
}

bool LinuxArtNetDatagramTransport::is_open() const noexcept {
    return socket_fd_ >= 0;
}

ArtNetTransportReceiveResult LinuxArtNetDatagramTransport::receive(
    std::span<std::uint8_t> buffer) noexcept {
    if (!is_open() || buffer.empty()) {
        return ArtNetTransportReceiveResult{ArtNetTransportReceiveStatus::error};
    }

    sockaddr_in source_address{};
    std::array<std::uint8_t, 128> control{};
    iovec io_vector{};
    io_vector.iov_base = buffer.data();
    io_vector.iov_len = buffer.size();

    msghdr message{};
    message.msg_name = &source_address;
    message.msg_namelen = static_cast<socklen_t>(sizeof(source_address));
    message.msg_iov = &io_vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t received = -1;
    do {
        received = ::recvmsg(socket_fd_, &message, MSG_TRUNC);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        if (retryable_receive_error(errno)) {
            return ArtNetTransportReceiveResult{ArtNetTransportReceiveStatus::no_data};
        }
        return ArtNetTransportReceiveResult{ArtNetTransportReceiveStatus::error};
    }

    if (source_address.sin_family != AF_INET) {
        return ArtNetTransportReceiveResult{ArtNetTransportReceiveStatus::error};
    }

    std::optional<in_addr> local_address;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message);
         header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != IPPROTO_IP ||
            header->cmsg_type != IP_PKTINFO ||
            header->cmsg_len < CMSG_LEN(sizeof(in_pktinfo))) {
            continue;
        }

        in_pktinfo packet_info{};
        std::memcpy(&packet_info, CMSG_DATA(header), sizeof(packet_info));
        local_address = packet_info.ipi_spec_dst;
        if (local_address->s_addr == htonl(INADDR_ANY)) {
            local_address = packet_info.ipi_addr;
        }
        break;
    }

    if (!local_address.has_value()) {
        return ArtNetTransportReceiveResult{ArtNetTransportReceiveStatus::error};
    }

    const auto datagram_size = static_cast<std::size_t>(received);
    const auto captured_size = std::min(datagram_size, buffer.size());
    return ArtNetTransportReceiveResult{
        ArtNetTransportReceiveStatus::datagram,
        from_in_addr(source_address.sin_addr),
        from_in_addr(*local_address),
        captured_size,
        datagram_size};
}

bool LinuxArtNetDatagramTransport::send_to(
    ArtNetIpv4Address destination,
    std::uint16_t port,
    std::span<const std::uint8_t> payload) noexcept {
    if (!is_open() || port == 0) {
        return false;
    }

    sockaddr_in destination_address{};
    destination_address.sin_family = AF_INET;
    destination_address.sin_port = htons(port);
    destination_address.sin_addr = to_in_addr(destination);

    ssize_t sent = -1;
    do {
        sent = ::sendto(
            socket_fd_,
            payload.data(),
            payload.size(),
            0,
            reinterpret_cast<const sockaddr*>(&destination_address),
            static_cast<socklen_t>(sizeof(destination_address)));
    } while (sent < 0 && errno == EINTR);

    return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
}

void LinuxArtNetDatagramTransport::close() noexcept {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    bound_port_.reset();
}

std::optional<std::uint16_t> LinuxArtNetDatagramTransport::bound_port() const noexcept {
    return bound_port_;
}

}  // namespace dmxwb
