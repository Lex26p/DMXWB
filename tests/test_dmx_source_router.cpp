#include "dmxwb/dmx_source_router.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
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

[[nodiscard]] dmxwb::DmxSnapshot make_snapshot(
    std::size_t slot_count,
    dmxwb::DmxSnapshot::Generation generation,
    std::uint8_t first,
    std::uint8_t second = 0) {
    auto builder = dmxwb::DmxSnapshotBuilder::create(slot_count);
    if (!builder.has_value()) {
        return {};
    }
    if (slot_count >= 1) {
        static_cast<void>(builder->set_channel(1, first));
    }
    if (slot_count >= 2) {
        static_cast<void>(builder->set_channel(2, second));
    }
    const auto snapshot = builder->build(generation);
    return snapshot ? *snapshot : dmxwb::DmxSnapshot{};
}

class RecordingPhysical final {
public:
    [[nodiscard]] bool publish(const dmxwb::DmxSnapshot& snapshot) {
        ++attempts_;
        if (fail_) {
            return false;
        }
        snapshots_.push_back(snapshot);
        return true;
    }

    bool fail_{false};
    std::uint64_t attempts_{0};
    std::vector<dmxwb::DmxSnapshot> snapshots_;
};

void test_latest_source_caches_and_switching() {
    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::mqtt,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};

    const auto mqtt_a = make_snapshot(4, 100, 10, 11);
    const auto artnet_a = make_snapshot(4, 200, 20, 21);
    const auto mqtt_b = make_snapshot(4, 101, 30, 31);
    const auto artnet_b = make_snapshot(4, 201, 40, 41);

    const auto mqtt_first = router.publish_mqtt_snapshot(mqtt_a);
    expect_true(mqtt_first.ok() && mqtt_first.physical_published,
        "selected MQTT snapshot reaches physical sink");
    expect_true(physical.snapshots_.size() == 1,
        "one physical snapshot after initial MQTT publish");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{10},
        "initial physical value comes from MQTT");

    const auto artnet_background = router.publish_artnet_snapshot(artnet_a);
    expect_true(artnet_background.ok() && !artnet_background.physical_publish_attempted,
        "background Art-Net snapshot is cached without touching physical output");
    expect_true(physical.snapshots_.size() == 1,
        "background Art-Net does not add physical frame");

    const auto mqtt_second = router.publish_mqtt_snapshot(mqtt_b);
    expect_true(mqtt_second.ok() && mqtt_second.physical_published,
        "new selected MQTT snapshot reaches physical sink");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{30},
        "latest MQTT value is physical before source switch");

    const auto to_artnet = router.select_source(dmxwb::PersistedSource::artnet);
    expect_true(to_artnet.ok() && to_artnet.source_changed && to_artnet.physical_published,
        "MQTT to ART-NET switch publishes cached Art-Net snapshot");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{20},
        "cached Art-Net value becomes physical on switch");
    expect_true(router.artnet_output_active(),
        "Art-Net output becomes active only after physical Art-Net publish");

    const auto mqtt_background = router.publish_mqtt_snapshot(make_snapshot(4, 102, 50, 51));
    expect_true(mqtt_background.ok() && !mqtt_background.physical_publish_attempted,
        "MQTT continues updating background cache while ART-NET selected");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{20},
        "background MQTT change does not touch Art-Net physical output");

    const auto artnet_second = router.publish_artnet_snapshot(artnet_b);
    expect_true(artnet_second.ok() && artnet_second.physical_published,
        "selected Art-Net latest snapshot reaches physical sink");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{40},
        "latest Art-Net value becomes physical");

    const auto to_mqtt = router.select_source(dmxwb::PersistedSource::mqtt);
    expect_true(to_mqtt.ok() && to_mqtt.source_changed && to_mqtt.physical_published,
        "ART-NET to MQTT switch publishes latest background MQTT snapshot");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{50},
        "re-entry uses latest whole MQTT state");
    expect_true(!router.artnet_output_active(),
        "Art-Net output active flag clears when MQTT is selected");

    const auto same_source = router.select_source(dmxwb::PersistedSource::mqtt);
    expect_true(same_source.ok() && !same_source.source_changed &&
                    !same_source.physical_publish_attempted,
        "selecting current source is a no-op");

    expect_true(physical.snapshots_.size() == 5,
        "router publishes only selected-source changes and real switches");
    for (std::size_t index = 0; index < physical.snapshots_.size(); ++index) {
        expect_true(
            physical.snapshots_[index].generation() ==
                static_cast<dmxwb::DmxSnapshot::Generation>(index + 1),
            "physical generations are router-owned and monotonic");
    }

    const auto diagnostics = router.diagnostics();
    expect_true(diagnostics.mqtt_snapshots_received == 3,
        "router counts all MQTT snapshots including background updates");
    expect_true(diagnostics.artnet_snapshots_received == 2,
        "router counts all Art-Net snapshots including background updates");
    expect_true(diagnostics.source_switches == 2,
        "router counts real source switches");
    expect_true(diagnostics.physical_snapshots_published == 5,
        "router diagnostics count physical snapshot publications");
    expect_true(diagnostics.physical_publish_failures == 0,
        "router has no publish failures on success path");
}

void test_switch_to_artnet_without_snapshot_holds_last() {
    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::mqtt,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};

    static_cast<void>(router.publish_mqtt_snapshot(make_snapshot(4, 1, 77, 78)));
    expect_true(physical.snapshots_.size() == 1,
        "precondition physical MQTT snapshot exists");

    const auto switch_result = router.select_source(dmxwb::PersistedSource::artnet);
    expect_true(switch_result.ok() && switch_result.source_changed &&
                    !switch_result.physical_publish_attempted,
        "switch to ART-NET without ArtDmx does not synthesize a snapshot");
    expect_true(physical.snapshots_.size() == 1,
        "physical output is preserved when ART-NET has no snapshot");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{77},
        "Hold Last preserves previous physical value before first ArtDmx");
    expect_true(!router.artnet_output_active(),
        "Art-Net output is not active before first physical Art-Net snapshot");

    const auto first_artnet =
        router.publish_artnet_snapshot(make_snapshot(4, 2, 88, 89));
    expect_true(first_artnet.ok() && first_artnet.physical_published,
        "first ArtDmx snapshot becomes physical after ART-NET selection");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{88},
        "first Art-Net data replaces held physical output");
    expect_true(router.artnet_output_active(),
        "Art-Net output active flag sets after first Art-Net physical publish");

    const auto diagnostics = router.diagnostics();
    expect_true(diagnostics.source_switches_without_snapshot == 1,
        "router records source switch that intentionally held previous output");
}

void test_initial_artnet_keeps_mqtt_background_only() {
    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::artnet,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};

    const auto mqtt =
        router.publish_mqtt_snapshot(make_snapshot(4, 1, 12, 13));
    expect_true(mqtt.ok() && !mqtt.physical_publish_attempted,
        "persisted ART-NET startup caches MQTT snapshot without physical publish");
    expect_true(physical.snapshots_.empty(),
        "persisted ART-NET startup does not inject MQTT frame");

    const auto to_mqtt = router.select_source(dmxwb::PersistedSource::mqtt);
    expect_true(to_mqtt.ok() && to_mqtt.physical_published,
        "switch to MQTT publishes startup background MQTT snapshot");
    expect_true(
        physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{12},
        "startup MQTT cache is ready for explicit source return");
}

void test_rejects_nonphysical_snapshot_and_reports_sink_failure() {
    RecordingPhysical physical;
    dmxwb::DmxSourceRouter router{
        dmxwb::PersistedSource::mqtt,
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};

    const auto invalid =
        router.publish_mqtt_snapshot(make_snapshot(301, 1, 1, 2));
    expect_true(!invalid.accepted && !invalid.ok(),
        "router rejects snapshot above physical 300-slot profile");
    expect_true(physical.attempts_ == 0,
        "invalid snapshot never reaches physical sink");

    physical.fail_ = true;
    const auto failed =
        router.publish_mqtt_snapshot(make_snapshot(4, 2, 9, 10));
    expect_true(failed.accepted && failed.physical_publish_failed() && !failed.ok(),
        "physical sink failure is surfaced without rejecting cached source state");
    expect_true(router.has_mqtt_snapshot(),
        "latest MQTT snapshot remains cached after physical sink failure");

    const auto diagnostics = router.diagnostics();
    expect_true(diagnostics.rejected_snapshots == 1,
        "invalid physical snapshot is diagnosed");
    expect_true(diagnostics.physical_publish_failures == 1,
        "physical sink failure is diagnosed");
    expect_true(diagnostics.physical_snapshots_published == 0,
        "failed sink publish is not counted as physical success");
}

}  // namespace

int main() {
    test_latest_source_caches_and_switching();
    test_switch_to_artnet_without_snapshot_holds_last();
    test_initial_artnet_keeps_mqtt_background_only();
    test_rejects_nonphysical_snapshot_and_reports_sink_failure();

    if (failures != 0) {
        std::cerr << failures << " DMX source router test(s) failed\n";
        return 1;
    }

    std::cout << "DMXWB DEV-010B1 source router tests PASS\n";
    return 0;
}
