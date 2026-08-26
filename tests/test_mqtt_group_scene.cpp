#include "dmxwb/mqtt_controller.hpp"
#include "dmxwb/persistence_storage.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
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
            ("dmxwb-dev008-mqtt-group-scene-" +
             std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX")).string();
        pattern.push_back('\0');
        char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) path_ = created;
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
    config.revision = 9;
    config.fixture_count = 3;
    config.start_address = 1;
    config.fixtures = {
        {10, "Fixture 10"},
        {20, "Fixture 20"},
        {30, "Fixture 30"},
    };
    config.groups = {
        {100, "Front", {10, 20}},
        {101, "Overlap", {20, 30}},
        {102, "Empty", {}},
    };
    config.scenes = {
        {200, "Blue pair", {
            {10, {0, 0, 255, 0}, 100, true},
            {20, {0, 0, 128, 0}, 50, true},
        }},
    };
    config.id_counters = {31, 103, 201};
    return config;
}

[[nodiscard]] bool prepare_runtime_files(
    const TempDirectory& temp,
    const dmxwb::AppConfig& config) {
    auto state = dmxwb::make_default_state(config);
    if (dmxwb::write_persistence_text_file_atomic(
            temp.file("config.json"), dmxwb::serialize_config_json(config))) {
        return false;
    }
    return !dmxwb::save_state_file_atomic(temp.file("state.json"), state, config);
}

[[nodiscard]] const dmxwb::MqttPublication* find_publication(
    const std::vector<dmxwb::MqttPublication>& publications,
    std::string_view topic) {
    const auto found = std::find_if(
        publications.begin(), publications.end(),
        [topic](const dmxwb::MqttPublication& publication) { return publication.topic == topic; });
    return found == publications.end() ? nullptr : &*found;
}


[[nodiscard]] std::string make_config_set_payload(
    std::string_view request_id,
    std::uint64_t expected_revision,
    const dmxwb::AppConfig& config) {
    std::string payload{"{\"request_id\":\""};
    payload += request_id;
    payload += "\",\"expected_revision\":";
    payload += std::to_string(expected_revision);
    payload += ",\"config\":";
    payload += dmxwb::serialize_config_json(config);
    payload += '}';
    return payload;
}

[[nodiscard]] bool contains_publication(
    const std::vector<dmxwb::MqttPublication>& publications,
    std::string_view topic,
    std::string_view payload) {
    const auto* found = find_publication(publications, topic);
    return found != nullptr && found->payload == payload;
}

void test_group_scene_parser_contract() {
    auto parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/color/on", "12;34;56", false);
    expect_true(parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::group_color,
        "Group Color /on parsed");
    expect_true(parsed.accepted() && parsed.command->group_id == 100 &&
        parsed.command->color == dmxwb::RgbwValues{12, 34, 56, 0},
        "Group stable ID and Color parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/brightness/on", "101", false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected,
        "Group Brightness above 100 rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/power/on", "1", true);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::ignored,
        "retained Group command ignored");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/name/on", "Вечер", false);
    expect_true(parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::scene_name &&
        parsed.command->scene_id == 200 && parsed.command->text == "Вечер",
        "Scene UTF-8 Name /on parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/apply/on", "1", false);
    expect_true(parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::scene_apply,
        "Scene Apply pushbutton parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/apply/on", "0", false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected,
        "Scene Apply payload other than 1 rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/apply/on", "1", true);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::ignored,
        "retained Scene Apply ignored");
}

void test_group_scene_publication_contract() {
    const dmxwb::GroupConfigRecord group{100, "Front", {10, 20}};
    const dmxwb::GroupControlState state{100, true, 1, 2, 3, 44, 55};
    const auto metadata = dmxwb::build_group_metadata_publications(group);
    expect_true(metadata.size() == 10, "Group metadata has device plus nine controls");
    bool hidden = true;
    for (const auto& publication : metadata) {
        if (publication.topic.ends_with("/meta") && publication.topic != "/devices/dmxwb_group_100/meta") {
            hidden = hidden && publication.payload.find("\"hidden\":true") != std::string::npos;
        }
    }
    expect_true(hidden, "all Group controls are hidden in standard WB web");

    const auto states = dmxwb::build_group_state_publications(group, state);
    expect_true(contains_publication(states, "/devices/dmxwb_group_100/controls/power", "1"),
        "Group factual Power state published");
    expect_true(contains_publication(states, "/devices/dmxwb_group_100/controls/color", "1;2;3"),
        "Group Color state is last Group setting");

    const dmxwb::SceneConfigRecord scene{200, "Blue pair", {}};
    const auto scene_meta = dmxwb::build_scene_metadata_publications(scene);
    expect_true(scene_meta.size() == 3, "Scene metadata has device, Name and Apply");
    expect_true(find_publication(scene_meta, "/devices/dmxwb_scene_200/controls/apply/meta") != nullptr,
        "Scene Apply metadata published");
    const auto scene_state = dmxwb::build_scene_state_publications(scene);
    expect_true(scene_state.size() == 1 && scene_state.front().payload == "Blue pair",
        "Scene only has retained Name state; Apply is stateless");

    const auto group_cleanup = dmxwb::build_group_retained_cleanup_publications(100);
    const auto scene_cleanup = dmxwb::build_scene_retained_cleanup_publications(200);
    expect_true(group_cleanup.size() == 18, "removed Group clears retained metadata/states");
    expect_true(scene_cleanup.size() == 4, "removed Scene clears retained metadata/Name state");
}

void test_group_controller_and_factual_overlap_power() {
    TempDirectory temp;
    expect_true(temp.valid(), "MQTT Group temp directory created");
    if (!temp.valid()) return;
    const auto config = make_config();
    expect_true(prepare_runtime_files(temp, config), "MQTT Group runtime files prepared");

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::MqttController controller{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    auto color = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/color/on", "255;0;0", false);
    auto update = controller.process_command(*color.command, t0);
    expect_true(update.applied && update.snapshot != nullptr,
        "Group Color builds one whole DMX snapshot");
    if (update.snapshot != nullptr) {
        expect_true(update.snapshot->channel(1) == std::optional<std::uint8_t>{0},
            "Group Color keeps OFF Fixture 10 physically off");
        expect_true(update.snapshot->channel(5) == std::optional<std::uint8_t>{0},
            "Group Color keeps OFF Fixture 20 physically off");
    }
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_100/controls/color", "255;0;0"),
        "Group last Color state confirmed");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_fixture_10/controls/color", "0;0;0"),
        "member Fixture factual Color remains zero while Power OFF");

    auto power = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/power/on", "1", false);
    update = controller.process_command(*power.command, t0 + std::chrono::milliseconds{1});
    expect_true(update.applied && update.snapshot != nullptr,
        "Group Power ON builds whole snapshot");
    if (update.snapshot != nullptr) {
        expect_true(update.snapshot->channel(1) == std::optional<std::uint8_t>{255},
            "Group Power ON restores Fixture 10 saved red");
        expect_true(update.snapshot->channel(5) == std::optional<std::uint8_t>{255},
            "Group Power ON restores Fixture 20 saved red");
        expect_true(update.snapshot->channel(9) == std::optional<std::uint8_t>{0},
            "Group Power leaves non-member Fixture 30 untouched");
    }
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_100/controls/power", "1"),
        "commanded Group factual Power becomes ON");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_101/controls/power", "1"),
        "overlapping Group factual Power republishes ON from shared Fixture");

    auto fixture_off = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_20/controls/power/on", "0", false);
    update = controller.process_command(*fixture_off.command, t0 + std::chrono::milliseconds{2});
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_100/controls/power", "1"),
        "individual Fixture change republishes affected Group factual Power");
}

void test_empty_group_and_scene_atomic_apply() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    expect_true(prepare_runtime_files(temp, config), "Scene runtime files prepared");

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::MqttController controller{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    auto empty = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_102/controls/brightness/on", "25", false);
    auto update = controller.process_command(*empty.command, t0);
    expect_true(update.applied && update.snapshot == nullptr,
        "empty Group accepts last setting without unnecessary DMX snapshot");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_102/controls/brightness", "25"),
        "empty Group publishes its last Brightness setting");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_102/controls/power", "0"),
        "empty Group factual Power is OFF");

    const auto source_before = runtime.source();
    auto apply = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/apply/on", "1", false);
    update = controller.process_command(*apply.command, t0 + std::chrono::milliseconds{1});
    expect_true(update.applied && update.snapshot != nullptr,
        "Scene Apply produces exactly one Controller whole snapshot result");
    expect_true(runtime.source() == source_before, "Scene Apply does not switch Source");
    if (update.snapshot != nullptr) {
        expect_true(update.snapshot->channel(1) == std::optional<std::uint8_t>{0} &&
                    update.snapshot->channel(3) == std::optional<std::uint8_t>{255},
            "Scene atomically applies Fixture 10 blue output");
        expect_true(update.snapshot->channel(5) == std::optional<std::uint8_t>{0} &&
                    update.snapshot->channel(7) == std::optional<std::uint8_t>{64},
            "Scene atomically applies Fixture 20 blue with Brightness 50");
        expect_true(update.snapshot->channel(9) == std::optional<std::uint8_t>{0},
            "Scene leaves Fixture 30 outside saved snapshot untouched");
    }
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_fixture_10/controls/color", "0;0;255"),
        "Scene publishes Fixture 10 factual state after atomic model update");
    expect_true(find_publication(update.publications, dmxwb::kMqttStateTopic) != nullptr,
        "Scene Apply republishes canonical saved logical state");
}

void test_names_and_full_republish() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    expect_true(prepare_runtime_files(temp, config), "name/reconnect runtime files prepared");

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::MqttController controller{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    auto group_name = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_group_100/controls/name/on", "Передний ряд", false);
    auto update = controller.process_command(*group_name.command, t0);
    expect_true(update.applied && update.snapshot == nullptr && runtime.config().revision == 10,
        "Group Name uses atomic config transaction without DMX snapshot");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_group_100/controls/name", "Передний ряд"),
        "Group Name retained state confirmed");

    auto scene_name = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_scene_200/controls/name/on", "Синяя сцена", false);
    update = controller.process_command(*scene_name.command, t0 + std::chrono::milliseconds{1});
    expect_true(update.applied && update.snapshot == nullptr && runtime.config().revision == 11,
        "Scene Name persists through atomic config transaction");
    expect_true(contains_publication(update.publications,
        "/devices/dmxwb_scene_200/controls/name", "Синяя сцена"),
        "Scene Name retained state confirmed");

    const auto republish = controller.build_full_republish();
    expect_true(find_publication(republish, "/devices/dmxwb_group_100/meta") != nullptr,
        "reconnect full republish includes Group metadata");
    expect_true(find_publication(republish, "/devices/dmxwb_group_100/controls/power") != nullptr,
        "reconnect full republish includes Group factual state");
    expect_true(find_publication(republish, "/devices/dmxwb_scene_200/meta") != nullptr,
        "reconnect full republish includes Scene metadata");
    expect_true(find_publication(republish, "/devices/dmxwb_scene_200/controls/name") != nullptr,
        "reconnect full republish includes Scene Name state");
    bool all_retained = true;
    for (const auto& publication : republish) all_retained = all_retained && publication.retained;
    expect_true(all_retained, "Group/Scene reconnect full republish is retained");
}


void test_config_removes_group_scene_retained_topics() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    expect_true(prepare_runtime_files(temp, config), "cleanup runtime files prepared");

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::MqttController controller{runtime};
    auto proposed = runtime.config();
    std::erase_if(proposed.groups, [](const dmxwb::GroupConfigRecord& group) { return group.id == 101; });
    std::erase_if(proposed.scenes, [](const dmxwb::SceneConfigRecord& scene) { return scene.id == 200; });

    const auto payload = make_config_set_payload("cleanup", runtime.config().revision, proposed);
    auto command = dmxwb::parse_mqtt_command(dmxwb::kMqttConfigSetTopic, payload, false);
    const auto update = controller.process_command(*command.command, dmxwb::PersistenceRuntime::time_point{});
    expect_true(update.applied, "structural config removing Group/Scene applies");
    const auto* group_meta = find_publication(update.publications, "/devices/dmxwb_group_101/meta");
    const auto* scene_meta = find_publication(update.publications, "/devices/dmxwb_scene_200/meta");
    expect_true(group_meta != nullptr && group_meta->retained && group_meta->payload.empty(),
        "removed Group device metadata is cleared with empty retained publication");
    expect_true(scene_meta != nullptr && scene_meta->retained && scene_meta->payload.empty(),
        "removed Scene device metadata is cleared with empty retained publication");
    expect_true(find_publication(update.publications, "/devices/dmxwb_group_100/meta") != nullptr,
        "surviving Group metadata is republished after structural config");
}

}  // namespace

int main() {
    test_group_scene_parser_contract();
    test_group_scene_publication_contract();
    test_group_controller_and_factual_overlap_power();
    test_empty_group_and_scene_atomic_apply();
    test_names_and_full_republish();
    test_config_removes_group_scene_retained_topics();

    if (failures == 0) {
        std::cout << "DMXWB DEV-008B1 MQTT Group/Scene tests: PASS\n";
        return 0;
    }
    std::cerr << "DMXWB DEV-008B1 MQTT Group/Scene tests: " << failures << " failure(s)\n";
    return 1;
}
