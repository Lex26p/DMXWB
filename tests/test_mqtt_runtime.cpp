#include "dmxwb/mqtt_runtime.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
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
    [[nodiscard]] bool take_full_republish_request() noexcept override {
        const bool result = republish_requested_;
        republish_requested_ = false;
        return result;
    }

    bool connected_{true};
    bool fail_publish_{false};
    bool republish_requested_{false};
    std::vector<std::vector<dmxwb::MqttPublication>> batches_;
};

class FakeDmx final : public dmxwb::MqttDmxSnapshotSink {
public:
    [[nodiscard]] bool publish_snapshot(const dmxwb::DmxSnapshot& snapshot) override {
        if (fail_) return false;
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
    FakeDmx dmx;
    dmxwb::MqttRuntimeCoordinator runtime{persistence, queue, controller, transport, dmx};

    expect_true(runtime.publish_initial_snapshot(), "MQTT source startup snapshot publishes");
    expect_true(dmx.snapshots_.size() == 1, "one startup whole snapshot");
    if (!dmx.snapshots_.empty()) {
        expect_true(dmx.snapshots_.back().channel(1) == std::optional<std::uint8_t>{0},
            "startup Fixture is physically off");
    }

    transport.republish_requested_ = true;
    runtime.step(dmxwb::PersistenceRuntime::time_point{});
    expect_true(runtime.diagnostics().full_republishes == 1, "reconnect triggers full retained republish");
    expect_true(!transport.batches_.empty(), "full republish produced MQTT batch");

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
    expect_true(dmx.snapshots_.size() == 2, "Fixture command publishes one whole DMX snapshot");
    if (dmx.snapshots_.size() >= 2) {
        expect_true(dmx.snapshots_.back().channel(1) == std::optional<std::uint8_t>{255},
            "Power ON reaches physical MQTT snapshot");
    }

    auto artnet = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on", "artnet", false);
    if (artnet.accepted()) queue.push(*artnet.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{2});
    const auto before_inactive_change = dmx.snapshots_.size();

    auto red = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/red/on", "17", false);
    if (red.accepted()) queue.push(*red.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{3});
    expect_true(dmx.snapshots_.size() == before_inactive_change,
        "inactive MQTT source updates model without touching physical DMX");
    expect_true(persistence.fixture_at(0) != nullptr && persistence.fixture_at(0)->saved_rgbw().red == 17,
        "inactive MQTT command still updates logical Fixture state");

    auto mqtt = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on", "mqtt", false);
    if (mqtt.accepted()) queue.push(*mqtt.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{4});
    expect_true(dmx.snapshots_.size() == before_inactive_change + 1,
        "ART-NET to MQTT re-entry publishes latest whole MQTT snapshot");
    if (!dmx.snapshots_.empty()) {
        expect_true(dmx.snapshots_.back().channel(1) == std::optional<std::uint8_t>{17},
            "re-entry snapshot contains background MQTT changes");
    }

    transport.connected_ = false;
    auto off = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "0", false);
    if (off.accepted()) queue.push(*off.command);
    runtime.step(dmxwb::PersistenceRuntime::time_point{} + std::chrono::milliseconds{5});
    expect_true(dmx.snapshots_.back().channel(1) == std::optional<std::uint8_t>{0},
        "broker loss does not prevent DMX update of already queued command");

    const auto flush = runtime.flush_state();
    expect_true(flush.ok(), "runtime forced state flush succeeds");

    const auto& diagnostics = runtime.diagnostics();
    expect_true(diagnostics.commands_processed == 5, "runtime processed all accepted commands sequentially");
    expect_true(diagnostics.dmx_publish_failures == 0, "runtime has no DMX publish failure");
    expect_true(diagnostics.state_save_failures == 0, "runtime has no persistence save failure");
}

}  // namespace

int main() {
    test_runtime_orchestration();
    if (failures != 0) {
        std::cerr << failures << " MQTT runtime test(s) failed\n";
        return 1;
    }
    std::cout << "All MQTT runtime tests passed\n";
    return 0;
}
