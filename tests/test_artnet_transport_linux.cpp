#include "dmxwb/artnet_transport_linux.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int failures = 0;

void expect_true(bool condition, std::string_view name) {
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failures;
        std::cerr << "[FAIL] " << name << '\n';
    }
}

class ZeroDelaySource final : public dmxwb::IArtNetPollReplyDelaySource {
public:
    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept override {
        return std::chrono::milliseconds{0};
    }
};

[[nodiscard]] int make_udp_socket() {
    return ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
}

[[nodiscard]] std::optional<std::uint16_t> bind_loopback_ephemeral(int socket_fd) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0) {
        return std::nullopt;
    }

    socklen_t address_size = static_cast<socklen_t>(sizeof(address));
    if (::getsockname(
            socket_fd,
            reinterpret_cast<sockaddr*>(&address),
            &address_size) != 0) {
        return std::nullopt;
    }
    return ntohs(address.sin_port);
}

[[nodiscard]] bool send_loopback(
    int socket_fd,
    std::uint16_t port,
    std::span<const std::uint8_t> payload) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto sent = ::sendto(
        socket_fd,
        payload.data(),
        payload.size(),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        static_cast<socklen_t>(sizeof(destination)));
    return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
}

[[nodiscard]] dmxwb::ArtNetTransportReceiveResult wait_transport_receive(
    dmxwb::LinuxArtNetDatagramTransport& transport,
    std::span<std::uint8_t> buffer) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto result = transport.receive(buffer);
        if (result.status != dmxwb::ArtNetTransportReceiveStatus::no_data) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return dmxwb::ArtNetTransportReceiveResult{dmxwb::ArtNetTransportReceiveStatus::no_data};
}

[[nodiscard]] std::vector<std::uint8_t> make_art_dmx(
    std::uint16_t port_address,
    std::uint8_t physical,
    std::uint8_t sequence,
    std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> packet;
    packet.reserve(18 + data.size());
    constexpr std::array<std::uint8_t, 8> id{
        static_cast<std::uint8_t>('A'),
        static_cast<std::uint8_t>('r'),
        static_cast<std::uint8_t>('t'),
        static_cast<std::uint8_t>('-'),
        static_cast<std::uint8_t>('N'),
        static_cast<std::uint8_t>('e'),
        static_cast<std::uint8_t>('t'),
        0};
    packet.insert(packet.end(), id.begin(), id.end());
    packet.push_back(0x00);
    packet.push_back(0x50);
    packet.push_back(0x00);
    packet.push_back(0x0e);
    packet.push_back(sequence);
    packet.push_back(physical);
    packet.push_back(static_cast<std::uint8_t>(port_address & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((port_address >> 8U) & 0x7fU));
    const auto length = static_cast<std::uint16_t>(data.size());
    packet.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(length & 0xffU));
    packet.insert(packet.end(), data.begin(), data.end());
    return packet;
}

void test_bind_nonblocking_close_and_rebind() {
    dmxwb::LinuxArtNetDatagramTransport first;
    std::array<std::uint8_t, 16> buffer{};
    expect_true(!first.is_open() && !first.bound_port().has_value(),
        "Linux Art-Net transport starts closed");
    expect_true(first.receive(buffer).status == dmxwb::ArtNetTransportReceiveStatus::error,
        "receive on closed transport reports error");

    expect_true(first.open_and_bind(0), "Linux Art-Net transport binds an ephemeral UDP port");
    const auto first_port = first.bound_port();
    expect_true(first.is_open() && first_port.has_value() && *first_port != 0,
        "bound transport exposes actual UDP port");
    expect_true(first.receive(buffer).status == dmxwb::ArtNetTransportReceiveStatus::no_data,
        "non-blocking receive returns no_data when socket has no datagram");
    expect_true(!first.open_and_bind(0), "open_and_bind rejects accidental second open");

    dmxwb::LinuxArtNetDatagramTransport second;
    expect_true(first_port.has_value() && !second.open_and_bind(*first_port),
        "second transport cannot share the same UDP port");

    first.close();
    expect_true(!first.is_open() && !first.bound_port().has_value(),
        "close releases descriptor and bound-port state");
    expect_true(first_port.has_value() && second.open_and_bind(*first_port),
        "released UDP port can be rebound immediately for recovery");
    second.close();
}

void test_receive_source_local_ip_and_truncation() {
    dmxwb::LinuxArtNetDatagramTransport transport;
    expect_true(transport.open_and_bind(0), "receive test transport opened");
    const auto port = transport.bound_port();
    const int sender = make_udp_socket();
    expect_true(port.has_value() && sender >= 0, "receive test sender created");
    if (!port.has_value() || sender < 0) {
        if (sender >= 0) ::close(sender);
        return;
    }

    const std::array<std::uint8_t, 6> payload{{1, 2, 3, 4, 5, 6}};
    expect_true(send_loopback(sender, *port, payload), "loopback UDP datagram sent to transport");

    std::array<std::uint8_t, 32> receive_buffer{};
    const auto result = wait_transport_receive(transport, receive_buffer);
    expect_true(result.status == dmxwb::ArtNetTransportReceiveStatus::datagram,
        "transport receives loopback UDP datagram");
    expect_true(result.captured_size == payload.size() && result.datagram_size == payload.size(),
        "untruncated datagram reports captured and wire sizes");
    expect_true(result.source_ip == dmxwb::ArtNetIpv4Address{{127, 0, 0, 1}},
        "transport reports sender IPv4 address");
    expect_true(result.local_ip == dmxwb::ArtNetIpv4Address{{127, 0, 0, 1}},
        "IP_PKTINFO reports local interface IPv4 address");
    expect_true(std::equal(payload.begin(), payload.end(), receive_buffer.begin()),
        "transport preserves received datagram bytes");

    std::array<std::uint8_t, 32> large_payload{};
    for (std::size_t index = 0; index < large_payload.size(); ++index) {
        large_payload[index] = static_cast<std::uint8_t>(index + 10U);
    }
    expect_true(send_loopback(sender, *port, large_payload), "oversized test datagram sent");
    std::array<std::uint8_t, 4> short_buffer{};
    const auto truncated = wait_transport_receive(transport, short_buffer);
    expect_true(truncated.status == dmxwb::ArtNetTransportReceiveStatus::datagram &&
                truncated.captured_size == short_buffer.size() &&
                truncated.datagram_size == large_payload.size(),
        "MSG_TRUNC preserves full datagram size while bounding captured bytes");
    expect_true(short_buffer[0] == 10 && short_buffer[3] == 13,
        "truncated receive keeps prefix bytes intact");

    ::close(sender);
    transport.close();
}

void test_unicast_send_to() {
    const int receiver = make_udp_socket();
    expect_true(receiver >= 0, "unicast receiver socket created");
    if (receiver < 0) return;

    const auto receiver_port = bind_loopback_ephemeral(receiver);
    dmxwb::LinuxArtNetDatagramTransport transport;
    expect_true(receiver_port.has_value() && transport.open_and_bind(0),
        "unicast sender transport and receiver port ready");
    if (!receiver_port.has_value() || !transport.is_open()) {
        ::close(receiver);
        return;
    }

    const std::array<std::uint8_t, 5> payload{{9, 8, 7, 6, 5}};
    expect_true(transport.send_to({{127, 0, 0, 1}}, *receiver_port, payload),
        "transport sends unicast UDP datagram");

    pollfd descriptor{receiver, POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, 1000);
    expect_true(poll_result == 1 && (descriptor.revents & POLLIN) != 0,
        "unicast receiver becomes readable");

    std::array<std::uint8_t, 32> buffer{};
    sockaddr_in source{};
    socklen_t source_size = static_cast<socklen_t>(sizeof(source));
    const auto received = ::recvfrom(
        receiver,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    expect_true(received == static_cast<ssize_t>(payload.size()) &&
                std::equal(payload.begin(), payload.end(), buffer.begin()),
        "unicast receiver gets exact transport payload");
    expect_true(transport.bound_port().has_value() &&
                ntohs(source.sin_port) == *transport.bound_port(),
        "unicast datagram originates from the bound Art-Net socket");
    expect_true(!transport.send_to({{127, 0, 0, 1}}, 0, payload),
        "transport rejects destination UDP port zero");

    transport.close();
    expect_true(!transport.send_to({{127, 0, 0, 1}}, *receiver_port, payload),
        "closed transport cannot send");
    ::close(receiver);
}

void test_runtime_over_real_udp_6454() {
    dmxwb::LinuxArtNetDatagramTransport transport;
    ZeroDelaySource delay_source;
    dmxwb::ArtNetRuntimeConfig config;
    config.core.port_address = 1;
    config.poll_reply_identity.oem_code = 0xbeef;  // test-only sentinel
    auto runtime = dmxwb::ArtNetRuntime::create(config, transport, delay_source);
    expect_true(runtime != nullptr, "Art-Net runtime created with Linux UDP transport");
    if (!runtime) return;

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    runtime->step(t0);
    expect_true(transport.is_open() && transport.bound_port() == std::optional<std::uint16_t>{dmxwb::kArtNetUdpPort},
        "runtime binds real non-blocking IPv4 UDP port 6454");
    if (!transport.is_open()) return;

    const int sender = make_udp_socket();
    expect_true(sender >= 0, "runtime loopback ArtDmx sender created");
    if (sender < 0) return;

    const std::array<std::uint8_t, 2> data{{41, 42}};
    const auto packet = make_art_dmx(1, 3, 1, data);
    expect_true(send_loopback(sender, dmxwb::kArtNetUdpPort, packet),
        "ArtDmx sent over real loopback UDP 6454");

    std::shared_ptr<const dmxwb::DmxSnapshot> snapshot;
    for (int attempt = 1; attempt <= 100 && !snapshot; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        runtime->step(t0 + std::chrono::milliseconds{attempt});
        snapshot = runtime->latest_physical_snapshot();
    }
    expect_true(snapshot != nullptr, "real UDP ArtDmx reaches runtime and publishes snapshot");
    if (snapshot) {
        expect_true(snapshot->slot_count() == dmxwb::kDmxPhysicalMaxSlots &&
                    snapshot->channel(1) == std::optional<std::uint8_t>{41} &&
                    snapshot->channel(2) == std::optional<std::uint8_t>{42},
            "real UDP runtime preserves ArtDmx channel data in physical projection");
    }
    expect_true(runtime->diagnostics().datagrams_received >= 1 &&
                runtime->diagnostics().snapshots_published == 1,
        "real UDP runtime diagnostics record receive and one committed snapshot");

    ::close(sender);
    runtime->shutdown();
    expect_true(!transport.is_open(), "runtime shutdown closes UDP 6454 transport");

    dmxwb::LinuxArtNetDatagramTransport probe;
    expect_true(probe.open_and_bind(dmxwb::kArtNetUdpPort),
        "UDP 6454 is immediately reusable after runtime shutdown");
    probe.close();
}

}  // namespace

int main() {
    test_bind_nonblocking_close_and_rebind();
    test_receive_source_local_ip_and_truncation();
    test_unicast_send_to();
    test_runtime_over_real_udp_6454();

    if (failures != 0) {
        std::cerr << failures << " Linux Art-Net UDP transport test(s) failed\n";
        return 1;
    }
    std::cout << "DMXWB DEV-010A Linux Art-Net UDP transport tests PASS\n";
    return 0;
}
