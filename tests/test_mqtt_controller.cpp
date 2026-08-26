#include "dmxwb/mqtt_controller.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
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

class TempDirectory final {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
            ("dmxwb-dev007-controller-" + std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX")).string();
        pattern.push_back('\0');
        char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
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

[[nodiscard]] dmxwb::AppConfig make_config() {
    auto config = dmxwb::make_default_config();
    config.revision = 3;
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
    std::string payload{"{\"request_id\":\""};
    payload += request_id;
    payload += "\",\"expected_revision\":" + std::to_string(expected_revision);
    payload += ",\"config\":";
    payload += dmxwb::serialize_config_json(config);
    payload += '}';
    return payload;
}

[[nodiscard]] bool contains_publication(
    const std::vector<dmxwb::MqttPublication>& publications,
    std::string_view topic,
    std::string_view payload) {
    for (const auto& publication : publications) {
        if (publication.topic == topic && publication.payload == payload) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const dmxwb::MqttPublication* find_publication(
    const std::vector<dmxwb::MqttPublication>& publications,
    std::string_view topic) {
    for (const auto& publication : publications) {
        if (publication.topic == topic) {
            return &publication;
        }
    }
    return nullptr;
}

void test_on_to_controller_snapshot_to_state() {
    TempDirectory temp;
    expect_true(temp.valid(), "controller temp directory created");
    if (!temp.valid()) return;

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    expect_true(!dmxwb::write_persistence_text_file_atomic(
        config_path, dmxwb::serialize_config_json(config)), "controller initial config write");
    expect_true(!dmxwb::save_state_file_atomic(state_path, state, config), "controller initial state write");

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    dmxwb::MqttCommandQueue queue;
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    const auto parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "1", false);
    expect_true(parsed.accepted(), "non-retained /on parsed");
    if (!parsed.accepted()) return;
    queue.push(*parsed.command);
    auto command = queue.try_pop();
    expect_true(command.has_value(), "MQTT callback queue delivers command");
    if (!command.has_value()) return;

    const auto update = controller.process_command(*command, t0);
    expect_true(update.applied, "Controller applies Fixture Power command");
    expect_true(update.snapshot != nullptr, "Controller builds whole DMX snapshot before state publication");
    if (update.snapshot) {
        expect_true(update.snapshot->generation() == 1, "first Controller snapshot generation is one");
        expect_true(update.snapshot->channel(1) == std::optional<std::uint8_t>{255}, "Power ON snapshot R=255");
        expect_true(update.snapshot->channel(2) == std::optional<std::uint8_t>{255}, "Power ON snapshot G=255");
        expect_true(update.snapshot->channel(3) == std::optional<std::uint8_t>{255}, "Power ON snapshot B=255");
        expect_true(update.snapshot->channel(4) == std::optional<std::uint8_t>{255}, "Power ON snapshot W=255");
    }
    expect_true(contains_publication(
        update.publications,
        "/devices/dmxwb_fixture_10/controls/power",
        "1"), "Controller confirms factual Fixture power state");
    expect_true(find_publication(update.publications, dmxwb::kMqttStateTopic) != nullptr,
        "Controller confirms canonical /dmxwb/state");
    expect_true(runtime.state_dirty(), "Fixture command marks persisted runtime state dirty");
}

void test_factual_brightness_and_power_off() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    auto state = dmxwb::make_default_state(config);
    state.fixtures[0].requested_power = true;
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    auto brightness = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/brightness/on", "50", false);
    const auto bright_update = controller.process_command(*brightness.command, t0);
    expect_true(bright_update.snapshot != nullptr, "Brightness builds snapshot");
    if (bright_update.snapshot) {
        expect_true(bright_update.snapshot->channel(1) == std::optional<std::uint8_t>{127},
            "Brightness 50 snapshot uses factual scaled RGBW");
    }
    expect_true(contains_publication(
        bright_update.publications,
        "/devices/dmxwb_fixture_10/controls/red",
        "127"), "Brightness publishes factual Red=127");

    auto off = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/power/on", "0", false);
    const auto off_update = controller.process_command(*off.command, t0 + std::chrono::milliseconds{1});
    expect_true(off_update.snapshot != nullptr, "Power OFF builds snapshot");
    if (off_update.snapshot) {
        expect_true(off_update.snapshot->channel(1) == std::optional<std::uint8_t>{0}, "Power OFF snapshot R=0");
        expect_true(off_update.snapshot->channel(4) == std::optional<std::uint8_t>{0}, "Power OFF snapshot W=0");
    }
    expect_true(contains_publication(
        off_update.publications,
        "/devices/dmxwb_fixture_10/controls/color",
        "0;0;0"), "Power OFF publishes factual Color=0;0;0");

    const auto* state_publication = find_publication(off_update.publications, dmxwb::kMqttStateTopic);
    expect_true(state_publication != nullptr, "Power OFF publishes saved logical state snapshot");
    if (state_publication != nullptr) {
        const auto parsed_state = dmxwb::parse_state_json(state_publication->payload);
        expect_true(parsed_state.ok(), "/dmxwb/state publication parses as canonical state");
        if (parsed_state.ok() && !parsed_state.value->fixtures.empty()) {
            const auto& saved = parsed_state.value->fixtures.front();
            expect_true(!saved.requested_power, "/dmxwb/state stores requested Power OFF");
            expect_true(saved.rgbw == dmxwb::RgbwValues{255, 255, 255, 255},
                "/dmxwb/state keeps saved RGBW while physical output is off");
            expect_true(saved.brightness == 50, "/dmxwb/state keeps Brightness setting");
        }
    }
}

void test_name_uses_atomic_config_transaction_and_survives_restart() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    auto name = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_10/controls/name/on", "Новый свет", false);
    const auto update = controller.process_command(*name.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(update.applied, "Fixture Name command applies through config transaction");
    expect_true(update.snapshot == nullptr, "Name-only command does not create unnecessary DMX snapshot");
    expect_true(runtime.config().revision == 4, "Name transaction increments config revision");
    expect_true(runtime.fixture_at(0) != nullptr && runtime.fixture_at(0)->name() == "Новый свет",
        "Name transaction updates current Fixture model");
    expect_true(find_publication(update.publications, dmxwb::kMqttConfigTopic) != nullptr,
        "Name transaction republishes canonical config");

    const auto flush = runtime.flush_state();
    expect_true(flush.ok(), "Name transaction dirty state can flush");
    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.config().revision == 4, "Name config revision survives restart");
    expect_true(restarted.fixture_at(0) != nullptr && restarted.fixture_at(0)->name() == "Новый свет",
        "Fixture Name survives restart from disk");
}

void test_source_and_full_republish() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    auto source = dmxwb::parse_mqtt_command(dmxwb::kMqttSystemSourceCommandTopic, "artnet", false);
    const auto update = controller.process_command(*source.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(update.applied && update.snapshot == nullptr, "Source command changes selector state without fake MQTT snapshot");
    expect_true(runtime.source() == dmxwb::PersistedSource::artnet, "Source command updates persisted Source model");
    expect_true(contains_publication(
        update.publications,
        "/devices/dmxwb/controls/source",
        "artnet"), "Source state confirmed through retained system topic");

    const auto republish = controller.build_full_republish();
    expect_true(find_publication(republish, "/devices/dmxwb/meta") != nullptr,
        "full reconnect republish includes system metadata");
    expect_true(find_publication(republish, "/devices/dmxwb_fixture_10/meta") != nullptr,
        "full reconnect republish includes Fixture metadata");
    expect_true(find_publication(republish, "/devices/dmxwb_fixture_10/controls/power") != nullptr,
        "full reconnect republish includes Fixture state");
    expect_true(find_publication(republish, dmxwb::kMqttConfigTopic) != nullptr,
        "full reconnect republish includes canonical config");
    expect_true(find_publication(republish, dmxwb::kMqttStateTopic) != nullptr,
        "full reconnect republish includes canonical state");
    expect_true(find_publication(republish, dmxwb::kMqttStatusTopic) != nullptr,
        "full reconnect republish includes status snapshot");
    bool all_retained = true;
    for (const auto& publication : republish) {
        all_retained = all_retained && publication.retained;
    }
    expect_true(all_retained, "full reconnect metadata/state publications are retained");
}

void test_config_set_atomic_success_and_restart() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    auto state = dmxwb::make_default_state(config);
    state.fixtures[0].requested_power = true;
    state.fixtures[0].rgbw = dmxwb::RgbwValues{101, 102, 103, 0};
    state.fixtures[0].brightness = 40;
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};

    auto proposed = config;
    proposed.start_address = 5;
    proposed.fixture_count = 2;
    proposed.fixtures.push_back(dmxwb::FixtureConfigRecord{11, "Fixture 11"});
    proposed.id_counters.next_fixture_id = 12;

    const auto payload = make_config_set_payload("cfg-ok", 3, proposed);
    const auto parsed = dmxwb::parse_mqtt_command(dmxwb::kMqttConfigSetTopic, payload, false);
    expect_true(parsed.accepted(), "config/set success command reaches Controller queue contract");
    if (!parsed.accepted()) return;

    const auto update = controller.process_command(*parsed.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(update.applied, "valid config/set atomically applies");
    expect_true(runtime.config().revision == 4, "config/set increments canonical revision once");
    expect_true(runtime.config().start_address == 5 && runtime.config().fixture_count == 2,
        "config/set replaces structural Fixture configuration");
    expect_true(update.snapshot != nullptr && update.snapshot->slot_count() == 12,
        "config/set builds one whole snapshot for new addressing");
    if (update.snapshot) {
        expect_true(update.snapshot->channel(5) == std::optional<std::uint8_t>{40},
            "existing Fixture state survives structural config transaction");
        expect_true(update.snapshot->channel(9) == std::optional<std::uint8_t>{0},
            "new Fixture starts physically OFF");
    }

    const auto* config_pub = find_publication(update.publications, dmxwb::kMqttConfigTopic);
    const auto* result_pub = find_publication(update.publications, dmxwb::kMqttConfigResultTopic);
    expect_true(config_pub != nullptr && config_pub->retained,
        "successful config/set republishes retained canonical config");
    expect_true(result_pub != nullptr && !result_pub->retained,
        "successful config/set publishes non-retained result");
    expect_true(result_pub != nullptr && result_pub->payload.find("\"request_id\":\"cfg-ok\"") != std::string::npos &&
        result_pub->payload.find("\"ok\":true") != std::string::npos &&
        result_pub->payload.find("\"revision\":4") != std::string::npos,
        "successful config result correlates request and committed revision");

    const auto flush = runtime.flush_state();
    expect_true(flush.ok(), "config/set reconciled state flush succeeds");
    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.config().revision == 4 && restarted.config().fixture_count == 2,
        "config/set canonical config survives restart");
    expect_true(restarted.fixture_at(0) != nullptr && restarted.fixture_at(0)->saved_rgbw().red == 101,
        "existing stable-ID state survives restart after config/set");
    expect_true(restarted.fixture_at(1) != nullptr && !restarted.fixture_at(1)->requested_power(),
        "new stable-ID Fixture safe default survives restart");
}

void test_config_set_revision_conflict_and_validation_reject() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};

    auto proposed = config;
    proposed.start_address = 9;
    proposed.revision = 2;
    auto stale = dmxwb::parse_mqtt_command(
        dmxwb::kMqttConfigSetTopic,
        make_config_set_payload("cfg-stale", 2, proposed),
        false);
    const auto stale_update = controller.process_command(*stale.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(!stale_update.applied, "stale expected_revision rejects whole config transaction");
    expect_true(runtime.config().revision == 3 && runtime.config().start_address == 1,
        "revision conflict leaves in-memory config unchanged");
    const auto* stale_result = find_publication(stale_update.publications, dmxwb::kMqttConfigResultTopic);
    expect_true(stale_result != nullptr && !stale_result->retained &&
        stale_result->payload.find("revision_conflict") != std::string::npos,
        "revision conflict returns non-retained correlated result");

    auto invalid = config;
    invalid.start_address = 300;
    auto invalid_command = dmxwb::parse_mqtt_command(
        dmxwb::kMqttConfigSetTopic,
        make_config_set_payload("cfg-invalid", 3, invalid),
        false);
    const auto invalid_update = controller.process_command(*invalid_command.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(!invalid_update.applied, "invalid nested config rejected before disk mutation");
    expect_true(runtime.config().revision == 3 && runtime.config().start_address == 1,
        "invalid config leaves working config unchanged");
    const auto* invalid_result = find_publication(invalid_update.publications, dmxwb::kMqttConfigResultTopic);
    expect_true(invalid_result != nullptr && invalid_result->payload.find("\"request_id\":\"cfg-invalid\"") != std::string::npos &&
        invalid_result->payload.find("\"ok\":false") != std::string::npos,
        "invalid config returns request-correlated failure result");

    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.config().revision == 3 && restarted.config().start_address == 1,
        "rejected config/set never replaced working disk config");
}

void test_config_set_fixture_removal_cleans_retained_topics() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    auto config = make_config();
    config.fixture_count = 2;
    config.fixtures.push_back(dmxwb::FixtureConfigRecord{11, "Fixture 11"});
    config.id_counters.next_fixture_id = 12;
    const auto state = dmxwb::make_default_state(config);
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    auto proposed = config;
    proposed.fixture_count = 1;
    proposed.fixtures.pop_back();

    auto command = dmxwb::parse_mqtt_command(
        dmxwb::kMqttConfigSetTopic,
        make_config_set_payload("cfg-remove", 3, proposed),
        false);
    const auto update = controller.process_command(*command.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(update.applied, "Fixture removal config/set applies");
    const auto* removed_meta = find_publication(update.publications, "/devices/dmxwb_fixture_11/meta");
    const auto* removed_power = find_publication(update.publications, "/devices/dmxwb_fixture_11/controls/power");
    expect_true(removed_meta != nullptr && removed_meta->retained && removed_meta->payload.empty(),
        "removed Fixture device metadata retained topic is cleared");
    expect_true(removed_power != nullptr && removed_power->retained && removed_power->payload.empty(),
        "removed Fixture retained state topic is cleared");
}

void test_unknown_fixture_does_not_mutate_model() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_config();
    const auto state = dmxwb::make_default_state(config);
    (void)dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config));
    (void)dmxwb::save_state_file_atomic(state_path, state, config);

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    dmxwb::MqttController controller{runtime};
    auto command = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_999/controls/power/on", "1", false);
    const auto update = controller.process_command(*command.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(!update.applied, "unknown Fixture ID is rejected by Controller");
    expect_true(!runtime.state_dirty(), "unknown Fixture ID does not mark state dirty");
    expect_true(runtime.fixture_at(0) != nullptr && !runtime.fixture_at(0)->requested_power(),
        "unknown Fixture ID does not mutate existing Fixture");
}

}  // namespace

int main() {
    test_on_to_controller_snapshot_to_state();
    test_factual_brightness_and_power_off();
    test_name_uses_atomic_config_transaction_and_survives_restart();
    test_source_and_full_republish();
    test_config_set_atomic_success_and_restart();
    test_config_set_revision_conflict_and_validation_reject();
    test_config_set_fixture_removal_cleans_retained_topics();
    test_unknown_fixture_does_not_mutate_model();

    if (failures != 0) {
        std::cerr << failures << " DEV-007B Controller test(s) failed\n";
        return 1;
    }
    std::cout << "DEV-007B MQTT Controller tests passed\n";
    return 0;
}
