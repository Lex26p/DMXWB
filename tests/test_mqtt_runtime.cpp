#include "dmxwb/mqtt_runtime.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
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

class TempDirectory final {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
            ("dmxwb-dev007-runtime-" + std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX")).string();
        pattern.push_back('\0');
        if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
            path_ = created;
        }
    }
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] std::string file(std::string_view name) const {
        return (path_ / std::string{name}).string();
    }
private:
    std::filesystem::path path_;
};

class FakeTransport final : public dmxwb::MqttRuntimeTransport {
public:
    [[nodiscard]] bool connected() const noexcept override { return connected_; }
    [[nodiscard]] bool publish_all(std::span<const dmxwb::MqttPublication> publications) override {
        if (!connected_ || fail_publish_) {
            republish_requested_ = true;
            return false;
        }
        batches_.emplace_back(publications.begin(), publications.end());
        return true;
    }
    [[nodiscard]] bool publish_retained_cleanup(
        std::span<const dmxwb::MqttPublication> publications) override {
        if (!connected_ || fail_cleanup_publish_) {
            cleanup_delivery_ = dmxwb::MqttRetainedCleanupDelivery::failed;
            return false;
        }
        cleanup_batches_.emplace_back(publications.begin(), publications.end());
        return true;
    }
    [[nodiscard]] dmxwb::MqttRetainedCleanupDelivery
    take_retained_cleanup_delivery() noexcept override {
        const auto result = cleanup_delivery_;
        cleanup_delivery_ = dmxwb::MqttRetainedCleanupDelivery::none;
        return result;
    }
    [[nodiscard]] bool take_full_republish_request() noexcept override {
        const bool result = republish_requested_;
        republish_requested_ = false;
        return result;
    }

    bool connected_{true};
    bool fail_publish_{false};
    bool fail_cleanup_publish_{false};
    bool republish_requested_{false};
    dmxwb::MqttRetainedCleanupDelivery cleanup_delivery_{
        dmxwb::MqttRetainedCleanupDelivery::none};
    std::vector<std::vector<dmxwb::MqttPublication>> batches_;
    std::vector<std::vector<dmxwb::MqttPublication>> cleanup_batches_;
};

class FakePhysical final {
public:
    [[nodiscard]] bool publish(const dmxwb::DmxSnapshot& snapshot) {
        if (fail_) {
            return false;
        }
        snapshots_.push_back(snapshot);
        return true;
    }

    bool fail_{false};
    std::vector<dmxwb::DmxSnapshot> snapshots_;
};

[[nodiscard]] dmxwb::AppConfig make_config() {
    auto config = dmxwb::make_default_config();
    config.revision = 1;
    config.fixture_count = 1;
    config.start_address = 1;
    config.fixtures = {dmxwb::FixtureConfigRecord{10, "Fixture 10"}};
    config.id_counters.next_fixture_id = 11;
    return config;
}

[[nodiscard]] std::string make_config_set_payload(
    std::string_view request_id,
    std::uint64_t expected_revision,
    const dmxwb::AppConfig& config) {
    return std::string{"{\"request_id\":\""} + std::string{request_id} +
        "\",\"expected_revision\":" + std::to_string(expected_revision) +
        ",\"config\":" + dmxwb::serialize_config_json(config) + "}";
}

void test_runtime_orchestration() {
    TempDirectory temp;
    expect_true(temp.valid(), "runtime temp directory created");
    if (!temp.valid()) return;

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    expect_true(!dmxwb::write_persistence_text_file_atomic(
        config_path, dmxwb::serialize_config_json(config)), "runtime config write");
    expect_true(!dmxwb::save_state_file_atomic(state_path, state, config), "runtime state write");

    dmxwb::PersistenceRuntime persistence{config_path, state_path};
    dmxwb::MqttCommandQueue queue;
    dmxwb::MqttController controller{persistence};
    FakeTransport transport;
    FakePhysical physical;
    dmxwb::DmxSourceRouter router{
        persistence.source(),
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::MqttRuntimeCoordinator runtime{
        persistence,
        queue,
        controller,
        transport,
        router};

    expect_true(runtime.publish_initial_snapshot(), "MQTT source startup snapshot routes");
    expect_true(physical.snapshots_.size() == 1, "one startup whole physical snapshot");
    if (!physical.snapshots_.empty()) {
        expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{0},
            "startup Fixture is physically off");
    }

    transport.republish_requested_ = true;
    runtime.step(dmxwb::PersistenceRuntime::time_point{});
    expect_true(runtime.diagnostics().full_republishes == 1, "reconnect triggers full retained republish");
    expect_true(!transport.batches_.empty(), "full republish produced MQTT batch");
    if (!transport.batches_.empty()) {
        bool has_operational_status = false;
        for (const auto& publication : transport.batches_.back()) {
            has_operational_status = has_operational_status ||
                publication.topic == dmxwb::kMqttStatusTopic ||
                publication.topic == "/devices/dmxwb/controls/status";
        }
        expect_true(!has_operational_status,
            "Controller reconnect batch leaves operational status to integrated runtime");
    }

    const auto batches_before_invalid_config = transport.batches_.size();
    auto invalid_config = dmxwb::parse_mqtt_command(dmxwb::kMqttConfigSetTopic, "{}", false);
    expect_true(invalid_config.accepted(), "malformed config payload is queued for Controller result");
    if (invalid_config.accepted()) queue.push(*invalid_config.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::microseconds{1});
    expect_true(runtime.diagnostics().commands_rejected == 1, "invalid config transaction counted as rejected command");
    expect_true(transport.batches_.size() == batches_before_invalid_config + 1,
        "rejected config transaction still publishes result batch");
    if (transport.batches_.size() > batches_before_invalid_config) {
        bool found_result = false;
        for (const auto& publication : transport.batches_.back()) {
            if (publication.topic == dmxwb::kMqttConfigResultTopic && !publication.retained &&
                publication.payload.find("\"ok\":false") != std::string::npos) {
                found_result = true;
            }
        }
        expect_true(found_result, "rejected config transaction publishes non-retained failure result");
    }

    auto power = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "1", false);
    expect_true(power.accepted(), "runtime Power command parses");
    if (power.accepted()) queue.push(*power.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{1});
    expect_true(physical.snapshots_.size() == 2, "Fixture command publishes one whole physical MQTT snapshot");
    if (physical.snapshots_.size() >= 2) {
        expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{255},
            "Power ON reaches physical MQTT snapshot");
    }

    auto artnet = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on", "artnet", false);
    if (artnet.accepted()) queue.push(*artnet.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{2});
    const auto before_inactive_change = physical.snapshots_.size();
    expect_true(router.selected_source() == dmxwb::PersistedSource::artnet,
        "source command switches router to ART-NET");
    expect_true(before_inactive_change == 2,
        "MQTT to ART-NET without ArtDmx preserves current physical output");

    auto red = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/red/on", "17", false);
    if (red.accepted()) queue.push(*red.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{3});
    expect_true(physical.snapshots_.size() == before_inactive_change,
        "inactive MQTT source updates router cache without touching physical DMX");
    expect_true(persistence.fixture_at(0) != nullptr && persistence.fixture_at(0)->saved_rgbw().red == 17,
        "inactive MQTT command still updates logical Fixture state");

    auto mqtt = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on", "mqtt", false);
    if (mqtt.accepted()) queue.push(*mqtt.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{4});
    expect_true(physical.snapshots_.size() == before_inactive_change + 1,
        "ART-NET to MQTT re-entry publishes cached latest whole MQTT snapshot");
    if (!physical.snapshots_.empty()) {
        expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{17},
            "re-entry snapshot contains background MQTT changes");
    }

    transport.connected_ = false;
    auto off = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "0", false);
    if (off.accepted()) queue.push(*off.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{5});
    expect_true(physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{0},
        "broker loss does not prevent DMX update of already queued command");

    const auto flush = runtime.flush_state();
    expect_true(flush.ok(), "runtime forced state flush succeeds");

    const auto& diagnostics = runtime.diagnostics();
    expect_true(diagnostics.commands_processed == 5, "runtime processed all accepted commands sequentially");
    expect_true(diagnostics.dmx_snapshots_published == 4,
        "runtime counts only snapshots that actually reached physical source router output");
    expect_true(diagnostics.dmx_publish_failures == 0, "runtime has no DMX route/publish failure");
    expect_true(diagnostics.state_save_failures == 0, "runtime has no persistence save failure");

    const auto router_diagnostics = router.diagnostics();
    expect_true(router_diagnostics.mqtt_snapshots_received == 4,
        "router receives selected and background MQTT whole snapshots");
    expect_true(router_diagnostics.source_switches == 2,
        "router observes both explicit source switches");
    expect_true(router_diagnostics.source_switches_without_snapshot == 1,
        "MQTT to empty ART-NET performs Hold Last instead of zero frame");
    expect_true(router_diagnostics.physical_snapshots_published == 4,
        "router physical publish count matches runtime diagnostics");
    expect_true(router_diagnostics.physical_publish_failures == 0,
        "router has no physical publish failure");
}

void test_retained_cleanup_survives_disconnect_and_restart_until_puback() {
    TempDirectory temp;
    expect_true(temp.valid(), "retained cleanup temp directory created");
    if (!temp.valid()) return;

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    auto config = make_config();
    config.fixture_count = 2;
    config.fixtures.push_back({11, "Fixture 11"});
    config.groups = {{5, "Group 5", {10, 11}}};
    config.scenes = {{7, "Scene 7", {{10, {1, 2, 3, 4}, 50, true}}}};
    config.id_counters = {12, 6, 8};
    const auto state = dmxwb::make_default_state(config);
    expect_true(!dmxwb::write_persistence_text_file_atomic(
        config_path, dmxwb::serialize_config_json(config)),
        "retained cleanup config write");
    expect_true(!dmxwb::save_state_file_atomic(state_path, state, config),
        "retained cleanup state write");

    {
        dmxwb::PersistenceRuntime persistence{config_path, state_path};
        dmxwb::MqttCommandQueue queue;
        dmxwb::MqttController controller{persistence};
        FakeTransport transport;
        FakePhysical physical;
        dmxwb::DmxSourceRouter router{
            persistence.source(),
            [&physical](const dmxwb::DmxSnapshot& snapshot) {
                return physical.publish(snapshot);
            }};
        dmxwb::MqttRuntimeCoordinator runtime{
            persistence, queue, controller, transport, router};

        auto proposed = config;
        proposed.fixture_count = 1;
        proposed.fixtures.pop_back();
        proposed.groups.clear();
        proposed.scenes.clear();
        auto command = dmxwb::parse_mqtt_command(
            dmxwb::kMqttConfigSetTopic,
            make_config_set_payload("durable-cleanup", config.revision, proposed),
            false);
        expect_true(command.accepted(), "retained cleanup config command parses");
        if (command.accepted()) queue.push(*command.command);
        runtime.step(dmxwb::PersistenceRuntime::time_point{});

        const auto& pending = persistence.pending_mqtt_retained_cleanup();
        expect_true(
            pending.fixture_ids == std::vector<dmxwb::Fixture::Id>{11} &&
                pending.group_ids == std::vector<dmxwb::GroupId>{5} &&
                pending.scene_ids == std::vector<dmxwb::SceneId>{7},
            "removed stable IDs become one durable cleanup intent");
        expect_true(transport.cleanup_batches_.size() == 1,
            "cleanup tombstones are submitted separately for delivery confirmation");
    }

    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    const auto& restored = restarted.pending_mqtt_retained_cleanup();
    expect_true(
        restored.fixture_ids == std::vector<dmxwb::Fixture::Id>{11} &&
            restored.group_ids == std::vector<dmxwb::GroupId>{5} &&
            restored.scene_ids == std::vector<dmxwb::SceneId>{7},
        "unacknowledged cleanup intent survives process restart");

    dmxwb::MqttCommandQueue queue;
    dmxwb::MqttController controller{restarted};
    FakeTransport transport;
    FakePhysical physical;
    dmxwb::DmxSourceRouter router{
        restarted.source(),
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        }};
    dmxwb::MqttRuntimeCoordinator runtime{
        restarted, queue, controller, transport, router};

    transport.republish_requested_ = true;
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{1});
    expect_true(transport.cleanup_batches_.size() == 1,
        "restart retries all durable cleanup tombstones");
    if (!transport.cleanup_batches_.empty()) {
        bool fixture_tombstone = false;
        bool group_tombstone = false;
        bool scene_tombstone = false;
        bool active_fixture_tombstone = false;
        for (const auto& publication : transport.cleanup_batches_.back()) {
            const bool tombstone = publication.retained && publication.payload.empty();
            fixture_tombstone = fixture_tombstone ||
                (tombstone && publication.topic == "/devices/dmxwb_fixture_11/meta");
            group_tombstone = group_tombstone ||
                (tombstone && publication.topic == "/devices/dmxwb_group_5/meta");
            scene_tombstone = scene_tombstone ||
                (tombstone && publication.topic == "/devices/dmxwb_scene_7/meta");
            active_fixture_tombstone = active_fixture_tombstone ||
                (tombstone && publication.topic.starts_with("/devices/dmxwb_fixture_10/"));
        }
        expect_true(fixture_tombstone && group_tombstone && scene_tombstone,
            "retry includes Fixture, Group and Scene tombstones");
        expect_true(!active_fixture_tombstone,
            "cleanup never tombstones a currently configured stable ID");
    }

    transport.cleanup_delivery_ = dmxwb::MqttRetainedCleanupDelivery::failed;
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{2});
    expect_true(transport.cleanup_batches_.size() == 2 &&
                    !restarted.pending_mqtt_retained_cleanup().empty(),
        "disconnect outcome keeps intent and retries the idempotent batch");

    transport.cleanup_delivery_ = dmxwb::MqttRetainedCleanupDelivery::delivered;
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{3});
    expect_true(restarted.pending_mqtt_retained_cleanup().empty(),
        "PUBACK completion clears the in-memory durable intent");
    expect_true(runtime.flush_state().ok(),
        "acknowledged cleanup state flush succeeds");

    dmxwb::PersistenceRuntime confirmed{config_path, state_path};
    expect_true(confirmed.pending_mqtt_retained_cleanup().empty(),
        "acknowledged cleanup remains cleared after restart");
}

void test_production_runtime_processes_commands_without_counters() {
    TempDirectory temp;
    expect_true(temp.valid(), "production runtime temp directory created");
    if (!temp.valid()) return;

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    expect_true(!dmxwb::write_persistence_text_file_atomic(
        config_path, dmxwb::serialize_config_json(config)),
        "production runtime config write");
    expect_true(!dmxwb::save_state_file_atomic(state_path, state, config),
        "production runtime state write");

    dmxwb::PersistenceRuntime persistence{config_path, state_path};
    dmxwb::MqttCommandQueue queue;
    dmxwb::MqttController controller{persistence};
    FakeTransport transport;
    FakePhysical physical;
    dmxwb::DmxSourceRouter router{
        persistence.source(),
        [&physical](const dmxwb::DmxSnapshot& snapshot) {
            return physical.publish(snapshot);
        },
        dmxwb::InstrumentationMode::production};
    dmxwb::MqttRuntimeCoordinator runtime{
        persistence,
        queue,
        controller,
        transport,
        router,
        dmxwb::InstrumentationMode::production};

    expect_true(runtime.publish_initial_snapshot(),
        "production MQTT runtime routes its initial whole snapshot");
    auto power = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "1", false);
    expect_true(power.accepted(), "production MQTT runtime Power command parses");
    if (power.accepted()) {
        queue.push(*power.command);
    }
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{1});

    expect_true(physical.snapshots_.size() == 2 &&
                    physical.snapshots_.back().channel(1) == std::optional<std::uint8_t>{255},
        "production MQTT runtime still updates physical DMX");
    expect_true(!transport.batches_.empty(),
        "production MQTT runtime still publishes factual MQTT state");

    const auto& diagnostics = runtime.diagnostics();
    expect_true(diagnostics.commands_processed == 0 &&
                    diagnostics.commands_rejected == 0 &&
                    diagnostics.dmx_snapshots_published == 0 &&
                    diagnostics.dmx_publish_failures == 0 &&
                    diagnostics.mqtt_publish_batches == 0 &&
                    diagnostics.mqtt_publish_failures == 0 &&
                    diagnostics.full_republishes == 0 &&
                    diagnostics.state_save_failures == 0,
        "production MQTT runtime does not accumulate engineering counters");
}

}  // namespace

int main() {
    test_runtime_orchestration();
    test_retained_cleanup_survives_disconnect_and_restart_until_puback();
    test_production_runtime_processes_commands_without_counters();
    if (failures != 0) {
        std::cerr << failures << " MQTT runtime test(s) failed\n";
        return 1;
    }
    std::cout << "All MQTT runtime tests passed\n";
    return 0;
}
