#include "dmxwb/artnet_source_coordinator.hpp"

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

constexpr dmxwb::ArtNetIpv4Address kControllerIp{{10, 200, 200, 2}};
constexpr dmxwb::ArtNetIpv4Address kLocalIp{{10, 200, 200, 1}};

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

[[nodiscard]] std::vector<std::uint8_t> make_dmx(
    std::uint16_t port_address,
    std::uint8_t sequence,
    std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> packet;
    packet.reserve(18 + data.size());
    append_artnet_header(packet, dmxwb::kArtNetOpDmx);
    append_protocol_revision(packet);
    packet.push_back(sequence);
    packet.push_back(1);
    packet.push_back(static_cast<std::uint8_t>(port_address & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((port_address >> 8U) & 0x7fU));
    const auto length = static_cast<std::uint16_t>(data.size());
    packet.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(length & 0xffU));
    packet.insert(packet.end(), data.begin(), data.end());
    return packet;
}

[[nodiscard]] std::vector<std::uint8_t> make_poll() {
    std::vector<std::uint8_t> packet;
    packet.reserve(14);
    append_artnet_header(packet, dmxwb::kArtNetOpPoll);
    append_protocol_revision(packet);
    packet.push_back(0);
    packet.push_back(0);
    return packet;
}

[[nodiscard]] std::vector<std::uint8_t> make_sync() {
    std::vector<std::uint8_t> packet;
    packet.reserve(14);
    append_artnet_header(packet, dmxwb::kArtNetOpSync);
    append_protocol_revision(packet);
    packet.push_back(0);
    packet.push_back(0);
    return packet;
}

[[nodiscard]] dmxwb::DmxSnapshot make_snapshot(
    std::uint8_t first,
    dmxwb::DmxSnapshot::Generation generation) {
    auto builder = dmxwb::DmxSnapshotBuilder::create(4);
    if (!builder.has_value()) {
        return {};
    }
    static_cast<void>(builder->set_channel(1, first));
    const auto snapshot = builder->build(generation);
    return snapshot ? *snapshot : dmxwb::DmxSnapshot{};
}

struct ReceiveEvent final {
    dmxwb::ArtNetIpv4Address source{};
    std::vector<std::uint8_t> payload;
};

struct SentDatagram final {
    std::vector<std::uint8_t> payload;
};

class FakeTransport final : public dmxwb::IArtNetDatagramTransport {
public:
    [[nodiscard]] bool open_and_bind(std::uint16_t) noexcept override {
        open_ = true;
        return true;
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return open_;
    }

    [[nodiscard]] dmxwb::ArtNetTransportReceiveResult receive(
        std::span<std::uint8_t> buffer) noexcept override {
        if (events_.empty()) {
            return {};
        }
        auto event = std::move(events_.front());
        events_.pop_front();
        const auto captured = std::min(buffer.size(), event.payload.size());
        std::copy_n(event.payload.begin(), captured, buffer.begin());
        return {
            dmxwb::ArtNetTransportReceiveStatus::datagram,
            event.source,
            kLocalIp,
            captured,
            event.payload.size()};
    }

    [[nodiscard]] bool send_to(
        dmxwb::ArtNetIpv4Address,
        std::uint16_t,
        std::span<const std::uint8_t> payload) noexcept override {
        sent_.push_back(SentDatagram{std::vector<std::uint8_t>{payload.begin(), payload.end()}});
        return true;
    }

    void close() noexcept override {
        open_ = false;
    }

    void queue(std::vector<std::uint8_t> packet) {
        events_.push_back(ReceiveEvent{kControllerIp, std::move(packet)});
    }

    bool open_{false};
    std::deque<ReceiveEvent> events_;
    std::vector<SentDatagram> sent_;
};

class ZeroDelay final : public dmxwb::IArtNetPollReplyDelaySource {
public:
    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept override {
        return std::chrono::milliseconds{0};
    }
};

class RecordingPhysical final {
public:
    [[nodiscard]] bool publish(const dmxwb::DmxSnapshot& snapshot) {
        snapshots_.push_back(snapshot);
        return true;
    }

    std::vector<dmxwb::DmxSnapshot> snapshots_;
};

class FailOncePhysical final {
public:
    [[nodiscard]] bool publish(const dmxwb::DmxSnapshot& snapshot) {
        ++attempts_;
        if (attempts_ == 1) {
            return false;
        }
        snapshots_.push_back(snapshot);
        return true;
    }

    std::uint64_t attempts_{0};
    std::vector<dmxwb::DmxSnapshot> snapshots_;
};

[[nodiscard]] dmxwb::ArtNetRuntimeConfig make_config() {
    dmxwb::ArtNetRuntimeConfig config;
    config.core.port_address = 0;
    config.poll_reply_identity.ip = kLocalIp;
    config.poll_reply_identity.mac = {{0x02, 0x10, 0x20, 0x30, 0x40, 0x50}};
    config.poll_reply_identity.oem_code = 0xbeef;
    config.poll_reply_identity.firmware_version = 0x0100;
    config.max_datagrams_per_step = 64;
    return config;
}

[[nodiscard]] bool last_good_output_active(const FakeTransport& transport) {
    return !transport.sent_.empty() &&
           transport.sent_.back().payload.size() == dmxwb::kArtNetPollReplyPacketSize &&
           (transport.sent_.back().payload[182] & 0x80U) != 0U;
}

void test_background_routing_source_switch_and_good_output() {
    FakeTransport transport;
    ZeroDelay delay;
    auto runtime = dmxwb::ArtNetRuntime::create(make_config(), transport, delay);
    expect_true(runtime != nullptr, "Art-Net runtime created for source coordinator");
    if (!runtime) {
        return;
    }

    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::mqtt,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::ArtNetSourceCoordinator coordinator{*runtime, router};

    static_cast<void>(router.publish_mqtt_snapshot(make_snapshot(9, 100)));
    expect_true(physical.snapshots_.size() == 1,
        "selected MQTT establishes initial physical snapshot");

    const std::array<std::uint8_t, 4> first{{17, 34, 51, 68}};
    transport.queue(make_dmx(0, 1, first));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{});
    expect_true(router.has_artnet_snapshot(),
        "Art-Net runtime commit is cached in source router while MQTT selected");
    expect_true(physical.snapshots_.size() == 1,
        "background Art-Net does not touch MQTT physical output");
    expect_true(!router.artnet_output_active(),
        "GoodOutput source state stays inactive while MQTT is selected");

    transport.queue(make_poll());
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{1});
    expect_true(!last_good_output_active(transport),
        "PollReply GoodOutputA bit7 is clear while MQTT is physical source");

    const auto to_artnet = router.select_source(dmxwb::PersistedSource::artnet);
    expect_true(to_artnet.ok() && to_artnet.physical_published,
        "MQTT to ART-NET switch uses already cached Art-Net snapshot");
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{17},
        "cached Art-Net channel becomes physical at source switch");

    transport.queue(make_poll());
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{2});
    expect_true(last_good_output_active(transport),
        "PollReply GoodOutputA bit7 becomes set after Art-Net is physically selected");

    static_cast<void>(router.publish_mqtt_snapshot(make_snapshot(77, 101)));
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{17},
        "MQTT continues as background cache while ART-NET selected");

    const std::array<std::uint8_t, 4> second{{101, 102, 103, 104}};
    transport.queue(make_dmx(0, 2, second));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{3});
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{101},
        "new selected Art-Net commit reaches physical router sink");

    const auto to_mqtt = router.select_source(dmxwb::PersistedSource::mqtt);
    expect_true(to_mqtt.ok() && to_mqtt.physical_published,
        "ART-NET to MQTT switch publishes latest background MQTT snapshot");
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{77},
        "return to MQTT uses current whole logical MQTT state");

    transport.queue(make_poll());
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{4});
    expect_true(!last_good_output_active(transport),
        "PollReply GoodOutputA bit7 clears again after MQTT selection");

    const auto& diagnostics = coordinator.diagnostics();
    expect_true(diagnostics.snapshots_observed == 2 &&
                diagnostics.snapshots_routed == 2 &&
                diagnostics.route_failures == 0,
        "coordinator routes each new Art-Net committed generation once");
}

void test_no_first_artdmx_preserves_physical_and_no_fifo() {
    FakeTransport transport;
    ZeroDelay delay;
    auto runtime = dmxwb::ArtNetRuntime::create(make_config(), transport, delay);
    if (!runtime) {
        expect_true(false, "runtime created for no-first-ArtDmx test");
        return;
    }

    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::mqtt,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::ArtNetSourceCoordinator coordinator{*runtime, router};

    static_cast<void>(router.publish_mqtt_snapshot(make_snapshot(55, 1)));
    const auto before_switch = physical.snapshots_.size();
    const auto to_artnet = router.select_source(dmxwb::PersistedSource::artnet);
    expect_true(to_artnet.ok() && !to_artnet.physical_publish_attempted,
        "selecting ART-NET before first ArtDmx performs no physical publish");
    expect_true(physical.snapshots_.size() == before_switch &&
                physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{55},
        "previous physical frame is held before first valid ArtDmx");

    const std::array<std::uint8_t, 2> one{{10, 11}};
    const std::array<std::uint8_t, 2> two{{20, 21}};
    const std::array<std::uint8_t, 2> three{{30, 31}};
    transport.queue(make_dmx(0, 1, one));
    transport.queue(make_dmx(0, 2, two));
    transport.queue(make_dmx(0, 3, three));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{});

    expect_true(physical.snapshots_.size() == before_switch + 1,
        "multiple ArtDmx commits in one network step publish one latest physical snapshot");
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{30},
        "latest ArtDmx wins without FIFO latency accumulation");
    expect_true(coordinator.diagnostics().snapshots_observed == 1,
        "coordinator observes only final immutable runtime snapshot for the step");
}

void test_sync_and_lost_hold_last() {
    FakeTransport transport;
    ZeroDelay delay;
    auto runtime = dmxwb::ArtNetRuntime::create(make_config(), transport, delay);
    if (!runtime) {
        expect_true(false, "runtime created for sync/LOST coordinator test");
        return;
    }

    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::artnet,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::ArtNetSourceCoordinator coordinator{*runtime, router};

    const std::array<std::uint8_t, 2> initial{{40, 41}};
    transport.queue(make_dmx(0, 1, initial));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{});
    expect_true(physical.snapshots_.size() == 1 &&
                physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{40},
        "first selected Art-Net commit reaches physical output");

    transport.queue(make_sync());
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{1});
    const std::array<std::uint8_t, 2> staged{{90, 91}};
    transport.queue(make_dmx(0, 2, staged));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{2});
    expect_true(physical.snapshots_.size() == 1,
        "staged ArtDmx does not leak through router before ArtSync");

    transport.queue(make_sync());
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{3});
    expect_true(physical.snapshots_.size() == 2 &&
                physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{90},
        "ArtSync commit routes one whole staged snapshot");

    const auto held_count = physical.snapshots_.size();
    coordinator.step(dmxwb::ArtNetRuntime::time_point{} + std::chrono::milliseconds{3003});
    expect_true(runtime->core().source_state() == dmxwb::ArtNetSourceState::lost,
        "Art-Net source reaches LOST after three seconds without ArtDmx");
    expect_true(physical.snapshots_.size() == held_count &&
                physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{90},
        "LOST keeps last committed Art-Net physical snapshot without blackout");
    expect_true(router.selected_source() == dmxwb::PersistedSource::artnet &&
                router.artnet_output_active(),
        "LOST does not automatically switch Source away from ART-NET");
}

void test_failed_generation_retries_at_bounded_interval() {
    FakeTransport transport;
    ZeroDelay delay;
    auto runtime = dmxwb::ArtNetRuntime::create(make_config(), transport, delay);
    if (!runtime) {
        expect_true(false, "runtime created for bounded route retry test");
        return;
    }

    FailOncePhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::artnet,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::ArtNetSourceCoordinator coordinator{*runtime, router};

    const auto start = dmxwb::ArtNetRuntime::time_point{};
    const std::array<std::uint8_t, 2> values{{71, 72}};
    transport.queue(make_dmx(0, 1, values));
    coordinator.step(start);

    expect_true(physical.attempts_ == 1 && physical.snapshots_.empty(),
        "first physical publication failure leaves Art-Net generation pending");
    expect_true(coordinator.diagnostics().snapshots_observed == 1 &&
                    coordinator.diagnostics().snapshots_routed == 0 &&
                    coordinator.diagnostics().route_failures == 1,
        "failed generation is observed once and not reported as routed");

    coordinator.step(
        start + dmxwb::ArtNetSourceCoordinator::kRouteRetryInterval -
            std::chrono::milliseconds{1});
    expect_true(physical.attempts_ == 1,
        "pending generation is not retried before the bounded interval");

    coordinator.step(start + dmxwb::ArtNetSourceCoordinator::kRouteRetryInterval);
    expect_true(physical.attempts_ == 2 && physical.snapshots_.size() == 1 &&
                    physical.snapshots_.front().channel(1) ==
                        std::optional<std::uint8_t>{71},
        "same Art-Net generation retries successfully without a new ArtDmx");
    expect_true(coordinator.diagnostics().snapshots_observed == 1 &&
                    coordinator.diagnostics().snapshots_routed == 1 &&
                    coordinator.diagnostics().route_failures == 1,
        "successful retry completes the one pending generation");

    coordinator.step(
        start + dmxwb::ArtNetSourceCoordinator::kRouteRetryInterval * 2);
    expect_true(physical.attempts_ == 2,
        "successfully routed generation is not published again");
}

void test_production_coordinator_routes_without_counters() {
    FakeTransport transport;
    ZeroDelay delay;
    auto runtime = dmxwb::ArtNetRuntime::create(
        make_config(),
        transport,
        delay,
        dmxwb::InstrumentationMode::production);
    if (!runtime) {
        expect_true(false, "runtime created for production coordinator test");
        return;
    }

    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::artnet,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        },
        dmxwb::InstrumentationMode::production};
    dmxwb::ArtNetSourceCoordinator coordinator{
        *runtime,
        router,
        dmxwb::InstrumentationMode::production};

    const std::array<std::uint8_t, 2> values{{61, 62}};
    transport.queue(make_dmx(0, 1, values));
    coordinator.step(dmxwb::ArtNetRuntime::time_point{});

    expect_true(physical.snapshots_.size() == 1 &&
                    physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{61},
        "production coordinator still routes the latest whole Art-Net snapshot");
    const auto& diagnostics = coordinator.diagnostics();
    expect_true(diagnostics.steps == 0 &&
                    diagnostics.snapshots_observed == 0 &&
                    diagnostics.snapshots_routed == 0 &&
                    diagnostics.route_failures == 0,
        "production coordinator does not accumulate engineering counters");
    expect_true(diagnostics.last_artnet_generation != 0 && diagnostics.artnet_output_active,
        "production coordinator retains factual output and generation state");
}

}  // namespace

int main() {
    test_background_routing_source_switch_and_good_output();
    test_no_first_artdmx_preserves_physical_and_no_fifo();
    test_sync_and_lost_hold_last();
    test_failed_generation_retries_at_bounded_interval();
    test_production_coordinator_routes_without_counters();

    if (failures != 0) {
        std::cerr << failures << " DEV-010B2 Art-Net source coordinator test(s) failed\n";
        return 1;
    }
    std::cout << "DMXWB DEV-010B2 Art-Net/source router integration tests PASS\n";
    return 0;
}
