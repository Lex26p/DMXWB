#include "dmxwb/artnet_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr dmxwb::ArtNetIpv4Address kIpA{{10, 20, 30, 40}};
constexpr dmxwb::ArtNetIpv4Address kIpB{{10, 20, 30, 41}};

void append_artnet_header(std::vector<std::uint8_t>& packet, std::uint16_t opcode) {
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
    packet.push_back(static_cast<std::uint8_t>(opcode & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((opcode >> 8U) & 0xffU));
}

void append_protocol_revision(std::vector<std::uint8_t>& packet) {
    packet.push_back(static_cast<std::uint8_t>((dmxwb::kArtNetProtocolRevision >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(dmxwb::kArtNetProtocolRevision & 0xffU));
}

std::vector<std::uint8_t> make_dmx(
    std::uint16_t port_address,
    std::uint8_t physical,
    std::uint8_t sequence,
    std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> packet;
    packet.reserve(18 + data.size());
    append_artnet_header(packet, dmxwb::kArtNetOpDmx);
    append_protocol_revision(packet);
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

std::vector<std::uint8_t> make_poll(bool targeted = false) {
    std::vector<std::uint8_t> packet;
    packet.reserve(14);
    append_artnet_header(packet, dmxwb::kArtNetOpPoll);
    append_protocol_revision(packet);
    packet.push_back(targeted ? 0x20 : 0x00);
    packet.push_back(0);
    return packet;
}

std::vector<std::uint8_t> make_sync() {
    std::vector<std::uint8_t> packet;
    packet.reserve(14);
    append_artnet_header(packet, dmxwb::kArtNetOpSync);
    append_protocol_revision(packet);
    packet.push_back(0);
    packet.push_back(0);
    return packet;
}

struct FakeReceiveEvent final {
    dmxwb::ArtNetTransportReceiveStatus status{dmxwb::ArtNetTransportReceiveStatus::no_data};
    dmxwb::ArtNetIpv4Address source{};
    dmxwb::ArtNetIpv4Address local{{192, 168, 1, 50}};
    std::vector<std::uint8_t> payload;
};

struct FakeSentDatagram final {
    dmxwb::ArtNetIpv4Address destination{};
    std::uint16_t port{0};
    std::vector<std::uint8_t> payload;
};

class FakeTransport final : public dmxwb::IArtNetDatagramTransport {
public:
    bool open_and_bind(std::uint16_t port) noexcept override {
        ++open_calls;
        last_bind_port = port;
        bool result = true;
        if (!open_results.empty()) {
            result = open_results.front();
            open_results.pop_front();
        }
        open = result;
        return result;
    }

    bool is_open() const noexcept override {
        return open;
    }

    dmxwb::ArtNetTransportReceiveResult receive(
        std::span<std::uint8_t> buffer) noexcept override {
        ++receive_calls;
        if (!open) {
            return {dmxwb::ArtNetTransportReceiveStatus::error, {}, {}, 0, 0};
        }
        if (receive_events.empty()) {
            return {};
        }

        auto event = std::move(receive_events.front());
        receive_events.pop_front();
        if (event.status != dmxwb::ArtNetTransportReceiveStatus::datagram) {
            return {event.status, event.source, event.local, 0, 0};
        }

        const auto captured = std::min(buffer.size(), event.payload.size());
        std::copy_n(event.payload.begin(), captured, buffer.begin());
        return {
            dmxwb::ArtNetTransportReceiveStatus::datagram,
            event.source,
            event.local,
            captured,
            event.payload.size()};
    }

    bool send_to(
        dmxwb::ArtNetIpv4Address destination,
        std::uint16_t port,
        std::span<const std::uint8_t> payload) noexcept override {
        bool result = true;
        if (!send_results.empty()) {
            result = send_results.front();
            send_results.pop_front();
        }
        if (!result) {
            return false;
        }
        sent.push_back(FakeSentDatagram{
            destination,
            port,
            std::vector<std::uint8_t>{payload.begin(), payload.end()}});
        return true;
    }

    void close() noexcept override {
        ++close_calls;
        open = false;
    }

    void queue_datagram(dmxwb::ArtNetIpv4Address source, std::vector<std::uint8_t> payload) {
        receive_events.push_back(FakeReceiveEvent{
            dmxwb::ArtNetTransportReceiveStatus::datagram,
            source,
            {{192, 168, 1, 50}},
            std::move(payload)});
    }

    void queue_receive_error() {
        receive_events.push_back(FakeReceiveEvent{
            dmxwb::ArtNetTransportReceiveStatus::error,
            {},
            {},
            {}});
    }

    bool open{false};
    std::uint64_t open_calls{0};
    std::uint64_t receive_calls{0};
    std::uint64_t close_calls{0};
    std::uint16_t last_bind_port{0};
    std::deque<bool> open_results;
    std::deque<bool> send_results;
    std::deque<FakeReceiveEvent> receive_events;
    std::vector<FakeSentDatagram> sent;
};

class FakeDelaySource final : public dmxwb::IArtNetPollReplyDelaySource {
public:
    std::chrono::milliseconds next_delay() noexcept override {
        if (values.empty()) {
            return std::chrono::milliseconds{0};
        }
        const auto value = values.front();
        values.pop_front();
        return value;
    }

    std::deque<std::chrono::milliseconds> values;
};

dmxwb::ArtNetRuntimeConfig make_runtime_config(std::uint16_t port_address = 1) {
    dmxwb::ArtNetRuntimeConfig config;
    config.core.port_address = port_address;
    config.poll_reply_identity.ip = {{192, 168, 1, 50}};
    config.poll_reply_identity.mac = {{0x02, 0x10, 0x20, 0x30, 0x40, 0x50}};
    config.poll_reply_identity.oem_code = 0xbeef;
    config.poll_reply_identity.firmware_version = 0x0100;
    return config;
}

void test_runtime_config_and_bind_retry() {
    FakeTransport transport;
    FakeDelaySource delays;

    auto invalid = make_runtime_config();
    invalid.rebind_delay = std::chrono::milliseconds{0};
    expect_true(!dmxwb::ArtNetRuntime::create(invalid, transport, delays),
        "runtime rejects zero rebind delay");
    invalid = make_runtime_config();
    invalid.max_datagrams_per_step = 0;
    expect_true(!dmxwb::ArtNetRuntime::create(invalid, transport, delays),
        "runtime rejects zero receive budget");
    invalid = make_runtime_config();
    invalid.pending_poll_reply_limit = 0;
    expect_true(!dmxwb::ArtNetRuntime::create(invalid, transport, delays),
        "runtime rejects zero pending PollReply limit");

    transport.open_results.push_back(false);
    transport.open_results.push_back(true);
    auto runtime = dmxwb::ArtNetRuntime::create(make_runtime_config(), transport, delays);
    expect_true(runtime != nullptr, "valid runtime created");
    if (!runtime) return;

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    runtime->step(t0);
    expect_true(runtime->diagnostics().bind_attempts == 1 && runtime->diagnostics().bind_failures == 1,
        "initial bind failure recorded");
    expect_true(transport.last_bind_port == dmxwb::kArtNetUdpPort,
        "runtime binds the Art-Net UDP port 6454");

    runtime->step(t0 + std::chrono::milliseconds{999});
    expect_true(runtime->diagnostics().bind_attempts == 1,
        "runtime does not spin bind attempts before rebind deadline");

    runtime->step(t0 + std::chrono::seconds{1});
    expect_true(runtime->diagnostics().bind_attempts == 2 &&
                runtime->diagnostics().transport_recoveries == 1 &&
                runtime->diagnostics().transport_open,
        "runtime rebinds after delay and records recovery");
}

void test_latest_snapshot_and_receive_budget() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto config = make_runtime_config(2);
    config.max_datagrams_per_step = 2;
    auto runtime = dmxwb::ArtNetRuntime::create(config, transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for latest-snapshot test");
        return;
    }

    expect_true(!runtime->latest_physical_snapshot(),
        "runtime exposes no Art-Net physical snapshot before first valid ArtDmx");

    const std::array<std::uint8_t, 2> one{{10, 11}};
    const std::array<std::uint8_t, 2> two{{20, 21}};
    const std::array<std::uint8_t, 2> three{{30, 31}};
    transport.queue_datagram(kIpA, make_dmx(2, 1, 1, one));
    transport.queue_datagram(kIpA, make_dmx(2, 1, 2, two));
    transport.queue_datagram(kIpA, make_dmx(2, 1, 3, three));

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    runtime->step(t0);
    auto snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->generation() == 2 && snapshot->slot_count() == 300,
        "two received commits replace one latest immutable 300-slot snapshot without FIFO output queue");
    if (snapshot) {
        expect_true(snapshot->channel(1) == std::optional<std::uint8_t>{20},
            "latest snapshot after bounded step contains second ArtDmx state");
    }
    expect_true(runtime->diagnostics().datagrams_received == 2,
        "per-step receive budget bounds network work");

    runtime->step(t0 + std::chrono::milliseconds{1});
    snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->generation() == 3 &&
                snapshot->channel(1) == std::optional<std::uint8_t>{30},
        "next step consumes remaining packet and replaces latest snapshot");
}

void test_delayed_unicast_poll_reply() {
    FakeTransport transport;
    FakeDelaySource delays;
    delays.values.push_back(std::chrono::milliseconds{500});
    auto runtime = dmxwb::ArtNetRuntime::create(make_runtime_config(0x1234), transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for PollReply test");
        return;
    }

    transport.queue_datagram(kIpB, make_poll());
    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    runtime->step(t0);
    expect_true(runtime->pending_poll_replies() == 1 && transport.sent.empty(),
        "ArtPoll schedules non-blocking delayed reply instead of sleeping/sending immediately");

    runtime->step(t0 + std::chrono::milliseconds{499});
    expect_true(transport.sent.empty(), "PollReply remains pending before randomized deadline");

    runtime->set_artnet_output_active(true);
    runtime->step(t0 + std::chrono::milliseconds{500});
    expect_true(transport.sent.size() == 1 && runtime->pending_poll_replies() == 0,
        "PollReply is sent when randomized deadline becomes due");
    if (transport.sent.empty()) return;

    const auto& sent = transport.sent.front();
    expect_true(sent.destination == kIpB && sent.port == dmxwb::kArtNetUdpPort,
        "PollReply is unicast back to polling controller on UDP 6454");
    expect_true(sent.payload.size() == dmxwb::kArtNetPollReplyPacketSize,
        "runtime sends full ArtPollReply built by confirmed DEV-009 core");
    if (sent.payload.size() == dmxwb::kArtNetPollReplyPacketSize) {
        expect_true(sent.payload[10] == 192 && sent.payload[11] == 168 && sent.payload[12] == 1 && sent.payload[13] == 50,
            "PollReply uses local destination IP captured with received ArtPoll");
        expect_true(sent.payload[190] == 0x04 && sent.payload[18] == 0x12 && sent.payload[19] == 0x03,
            "PollReply keeps configured 15-bit SwOut subscription");
        expect_true(sent.payload[182] == 0x80,
            "GoodOutputA active bit is evaluated at send time");
        expect_true(sent.payload[226] == 0 && sent.payload[227] == 44,
            "runtime PollReply advertises fixed RefreshRate 44");
    }
}

void test_missing_oem_does_not_break_receiver() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto config = make_runtime_config(3);
    config.poll_reply_identity.oem_code.reset();
    auto runtime = dmxwb::ArtNetRuntime::create(config, transport, delays);
    if (!runtime) {
        expect_true(false, "runtime allows receive operation without production OEM assignment");
        return;
    }

    transport.queue_datagram(kIpA, make_poll());
    runtime->step(dmxwb::ArtNetRuntime::time_point{});
    expect_true(transport.sent.empty() && runtime->diagnostics().poll_replies_not_built == 1,
        "missing OEM prevents PollReply construction without crashing network receiver");
    expect_true(runtime->diagnostics().transport_open,
        "missing OEM does not close Art-Net receive transport");
}

void test_transport_error_rebind_hold_last_and_new_source() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto runtime = dmxwb::ArtNetRuntime::create(make_runtime_config(4), transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for recovery test");
        return;
    }

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    const std::array<std::uint8_t, 2> first{{44, 45}};
    transport.queue_datagram(kIpA, make_dmx(4, 7, 1, first));
    runtime->step(t0);
    auto held = runtime->latest_physical_snapshot();
    expect_true(held && held->channel(1) == std::optional<std::uint8_t>{44},
        "recovery test establishes committed Hold Last snapshot");

    transport.queue_receive_error();
    runtime->step(t0 + std::chrono::milliseconds{100});
    expect_true(!runtime->diagnostics().transport_open && runtime->diagnostics().receive_errors == 1,
        "receive error closes transport and schedules recovery");
    auto after_error = runtime->latest_physical_snapshot();
    expect_true(after_error && after_error->generation() == held->generation(),
        "socket failure does not clear latest committed Art-Net snapshot");

    transport.open_results.push_back(false);
    transport.open_results.push_back(true);
    runtime->step(t0 + std::chrono::milliseconds{1100});
    expect_true(runtime->diagnostics().bind_failures == 1,
        "failed rebind is recorded without process restart");
    runtime->step(t0 + std::chrono::milliseconds{2100});
    expect_true(runtime->diagnostics().transport_open && runtime->diagnostics().transport_recoveries == 1,
        "subsequent rebind succeeds automatically");

    runtime->step(t0 + std::chrono::milliseconds{3100});
    expect_true(runtime->core().source_state() == dmxwb::ArtNetSourceState::lost &&
                runtime->diagnostics().source_lost_events == 1,
        "core LOST timer continues while network transport is unavailable/recovering");
    auto after_lost = runtime->latest_physical_snapshot();
    expect_true(after_lost && after_lost->channel(1) == std::optional<std::uint8_t>{44},
        "LOST releases source lock but runtime keeps Hold Last data");

    const std::array<std::uint8_t, 2> second{{99, 100}};
    transport.queue_datagram(kIpB, make_dmx(4, 2, 50, second));
    runtime->step(t0 + std::chrono::milliseconds{3200});
    auto recovered = runtime->latest_physical_snapshot();
    expect_true(recovered && recovered->channel(1) == std::optional<std::uint8_t>{99} &&
                runtime->core().active_source() == std::optional<dmxwb::ArtNetSource>{{kIpB, 2}},
        "new source can acquire released lock after transport recovery and LOST");
}

void test_send_error_enters_rebind_path() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto runtime = dmxwb::ArtNetRuntime::create(make_runtime_config(5), transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for send-error test");
        return;
    }

    transport.send_results.push_back(false);
    transport.queue_datagram(kIpA, make_poll());
    runtime->step(dmxwb::ArtNetRuntime::time_point{});
    expect_true(runtime->diagnostics().send_errors == 1 && !runtime->diagnostics().transport_open,
        "PollReply send failure closes transport and enters common rebind path");
    expect_true(runtime->pending_poll_replies() == 0,
        "failed PollReply is not retained as an unbounded stale send queue");
}

void test_sync_commit_publishes_only_committed_state() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto runtime = dmxwb::ArtNetRuntime::create(make_runtime_config(6), transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for ArtSync publication test");
        return;
    }

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    const std::array<std::uint8_t, 2> one{{1, 1}};
    const std::array<std::uint8_t, 2> two{{2, 2}};

    transport.queue_datagram(kIpA, make_dmx(6, 1, 1, one));
    runtime->step(t0);
    auto snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->generation() == 1,
        "asynchronous ArtDmx publishes committed snapshot");

    transport.queue_datagram(kIpA, make_sync());
    runtime->step(t0 + std::chrono::milliseconds{1});
    transport.queue_datagram(kIpA, make_dmx(6, 1, 2, two));
    runtime->step(t0 + std::chrono::milliseconds{2});
    snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->generation() == 1 &&
                snapshot->channel(1) == std::optional<std::uint8_t>{1},
        "synchronous staged ArtDmx does not leak into published snapshot");

    transport.queue_datagram(kIpA, make_sync());
    runtime->step(t0 + std::chrono::milliseconds{3});
    snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->generation() == 2 &&
                snapshot->channel(1) == std::optional<std::uint8_t>{2},
        "matching ArtSync atomically releases one new runtime snapshot");
}

void test_pending_limit_and_delay_clamp() {
    FakeTransport transport;
    FakeDelaySource delays;
    delays.values.push_back(std::chrono::milliseconds{1500});
    auto config = make_runtime_config(7);
    config.pending_poll_reply_limit = 1;
    auto runtime = dmxwb::ArtNetRuntime::create(config, transport, delays);
    if (!runtime) {
        expect_true(false, "runtime created for pending-limit test");
        return;
    }

    transport.queue_datagram(kIpA, make_poll());
    transport.queue_datagram(kIpB, make_poll());
    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    runtime->step(t0);
    expect_true(runtime->pending_poll_replies() == 1 &&
                runtime->diagnostics().poll_replies_dropped == 1,
        "pending PollReply queue is explicitly bounded");
    expect_true(runtime->diagnostics().delay_values_clamped == 1,
        "delay source values above Art-Net one-second maximum are clamped");

    runtime->step(t0 + std::chrono::milliseconds{999});
    expect_true(transport.sent.empty(), "clamped PollReply is not sent before one-second deadline");
    runtime->step(t0 + std::chrono::seconds{1});
    expect_true(transport.sent.size() == 1,
        "clamped PollReply is sent at one-second maximum delay");
}

void test_production_runtime_recovers_without_counters() {
    FakeTransport transport;
    FakeDelaySource delays;
    auto runtime = dmxwb::ArtNetRuntime::create(
        make_runtime_config(8),
        transport,
        delays,
        dmxwb::InstrumentationMode::production);
    if (!runtime) {
        expect_true(false, "production Art-Net runtime created");
        return;
    }

    const auto t0 = dmxwb::ArtNetRuntime::time_point{};
    const std::array<std::uint8_t, 2> values{{71, 72}};
    transport.queue_datagram(kIpA, make_dmx(8, 1, 1, values));
    runtime->step(t0);
    auto snapshot = runtime->latest_physical_snapshot();
    expect_true(snapshot && snapshot->channel(1) == std::optional<std::uint8_t>{71},
        "production Art-Net runtime still commits a whole physical snapshot");

    transport.queue_receive_error();
    runtime->step(t0 + std::chrono::milliseconds{100});
    expect_true(!runtime->diagnostics().transport_open,
        "production Art-Net runtime still enters rebind after receive failure");
    runtime->step(t0 + std::chrono::milliseconds{1100});
    expect_true(runtime->diagnostics().transport_open,
        "production Art-Net runtime still recovers transport in process");

    transport.queue_datagram(kIpA, make_poll());
    runtime->step(t0 + std::chrono::milliseconds{1101});
    expect_true(transport.sent.size() == 1,
        "production Art-Net runtime still sends a scheduled PollReply");

    const auto& diagnostics = runtime->diagnostics();
    expect_true(diagnostics.bind_attempts == 0 &&
                    diagnostics.bind_failures == 0 &&
                    diagnostics.transport_recoveries == 0 &&
                    diagnostics.datagrams_received == 0 &&
                    diagnostics.receive_errors == 0 &&
                    diagnostics.send_errors == 0 &&
                    diagnostics.core_rejections == 0 &&
                    diagnostics.conflicts == 0 &&
                    diagnostics.source_lost_events == 0 &&
                    diagnostics.snapshots_published == 0 &&
                    diagnostics.poll_replies_scheduled == 0 &&
                    diagnostics.poll_replies_sent == 0 &&
                    diagnostics.poll_replies_dropped == 0 &&
                    diagnostics.poll_replies_not_built == 0 &&
                    diagnostics.delay_values_clamped == 0,
        "production Art-Net runtime does not accumulate engineering counters");
    expect_true(diagnostics.transport_open &&
                    runtime->core().committed_revision() == 1 &&
                    runtime->pending_poll_replies() == 0,
        "production Art-Net runtime retains factual and algorithmic state");
}

}  // namespace

int main() {
    test_runtime_config_and_bind_retry();
    test_latest_snapshot_and_receive_budget();
    test_delayed_unicast_poll_reply();
    test_missing_oem_does_not_break_receiver();
    test_transport_error_rebind_hold_last_and_new_source();
    test_send_error_enters_rebind_path();
    test_sync_commit_publishes_only_committed_state();
    test_pending_limit_and_delay_clamp();
    test_production_runtime_recovers_without_counters();

    if (failures != 0) {
        std::cerr << failures << " DEV-010A Art-Net runtime test(s) failed\n";
        return 1;
    }
    std::cout << "DMXWB DEV-010A Art-Net runtime scheduling/recovery tests PASS\n";
    return 0;
}
