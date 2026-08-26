#include "dmxwb/group_scene.hpp"
#include "dmxwb/persistence_storage.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
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
            ("dmxwb-dev008-group-scene-" + std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX")).string();
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
    config.revision = 7;
    config.fixture_count = 3;
    config.start_address = 1;
    config.fixtures = {
        {10, "Fixture 10"},
        {20, "Fixture 20"},
        {30, "Fixture 30"},
    };
    config.groups = {
        {100, "Group A", {10, 20}},
        {101, "Group B", {20, 30}},
        {102, "Empty", {}},
    };
    config.scenes.clear();
    config.id_counters = {31, 103, 200};
    return config;
}

[[nodiscard]] dmxwb::AppState make_state(const dmxwb::AppConfig& config) {
    auto state = dmxwb::make_default_state(config);
    state.fixtures[0] = {10, false, {200, 10, 20, 30}, 80, 11};
    state.fixtures[1] = {20, false, {10, 180, 30, 40}, 60, 22};
    state.fixtures[2] = {30, false, {20, 30, 160, 50}, 40, 33};
    return state;
}

[[nodiscard]] bool prepare_runtime_files(
    const TempDirectory& temp,
    const dmxwb::AppConfig& config,
    const dmxwb::AppState& state) {
    if (dmxwb::write_persistence_text_file_atomic(
            temp.file("config.json"), dmxwb::serialize_config_json(config))) {
        return false;
    }
    return !dmxwb::save_state_file_atomic(temp.file("state.json"), state, config);
}

[[nodiscard]] dmxwb::Fixture* fixture_by_id(dmxwb::PersistenceRuntime& runtime, dmxwb::Fixture::Id id) {
    for (std::size_t index = 0; index < runtime.fixtures().fixture_count(); ++index) {
        auto* fixture = runtime.fixture_at(index);
        if (fixture != nullptr && fixture->id() == id) {
            return fixture;
        }
    }
    return nullptr;
}

void test_multiple_groups_and_last_command_wins() {
    TempDirectory temp;
    expect_true(temp.valid(), "group temp directory created");
    if (!temp.valid()) return;

    const auto config = make_config();
    const auto state = make_state(config);
    expect_true(prepare_runtime_files(temp, config, state), "group runtime files prepared");

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::GroupSceneManager manager{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    dmxwb::GroupControlCommand first;
    first.group_id = 100;
    first.type = dmxwb::GroupControlType::color;
    first.color = {255, 0, 0, 0};
    const auto first_result = manager.apply_group_command(first, t0);
    expect_true(first_result.applied && first_result.fixture_state_changed,
        "first Group Color mutates its member Fixtures");

    auto* f10 = fixture_by_id(runtime, 10);
    auto* f20 = fixture_by_id(runtime, 20);
    auto* f30 = fixture_by_id(runtime, 30);
    expect_true(f10 != nullptr && f10->saved_rgbw() == dmxwb::RgbwValues{255, 0, 0, 0},
        "Group A Color applies to Fixture 10");
    expect_true(f20 != nullptr && f20->saved_rgbw() == dmxwb::RgbwValues{255, 0, 0, 0},
        "Group A Color applies to shared Fixture 20");
    expect_true(f30 != nullptr && f30->saved_rgbw() == dmxwb::RgbwValues{20, 30, 160, 50},
        "Group A leaves non-member Fixture 30 untouched");

    dmxwb::GroupControlCommand second;
    second.group_id = 101;
    second.type = dmxwb::GroupControlType::color;
    second.color = {0, 0, 255, 0};
    const auto second_result = manager.apply_group_command(second, t0 + std::chrono::milliseconds{1});
    expect_true(second_result.applied, "second Group Color applies");
    expect_true(f10 != nullptr && f10->saved_rgbw() == dmxwb::RgbwValues{255, 0, 0, 0},
        "last command from another Group does not touch Fixture 10");
    expect_true(f20 != nullptr && f20->saved_rgbw() == dmxwb::RgbwValues{0, 0, 255, 0},
        "shared Fixture 20 follows last Group command");
    expect_true(f30 != nullptr && f30->saved_rgbw() == dmxwb::RgbwValues{0, 0, 255, 0},
        "Group B Color applies to Fixture 30");

    const auto group_a = manager.group_state(100);
    const auto group_b = manager.group_state(101);
    expect_true(group_a.has_value() && group_a->red == 255 && group_a->green == 0 && group_a->blue == 0,
        "Group A keeps its own last Color setting after shared Fixture changes elsewhere");
    expect_true(group_b.has_value() && group_b->red == 0 && group_b->green == 0 && group_b->blue == 255,
        "Group B exposes its last Color setting");
    expect_true(runtime.state_dirty(), "Group live command marks Fixture logical state dirty once");
}

void test_group_power_restore_reset_factual_and_empty() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    const auto state = make_state(config);
    if (!prepare_runtime_files(temp, config, state)) return;

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::GroupSceneManager manager{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    dmxwb::GroupControlCommand on;
    on.group_id = 100;
    on.type = dmxwb::GroupControlType::power;
    on.boolean_value = true;
    const auto on_result = manager.apply_group_command(on, t0);
    expect_true(on_result.applied && on_result.state.actual_power,
        "Group Power ON reports factual OR member power");

    const auto* f10 = fixture_by_id(runtime, 10);
    const auto* f20 = fixture_by_id(runtime, 20);
    expect_true(f10 != nullptr && f10->actual_rgbw() == dmxwb::RgbwValues{160, 8, 16, 24},
        "Group Power ON restores Fixture 10 own saved RGBW through own Brightness");
    expect_true(f20 != nullptr && f20->actual_rgbw() == dmxwb::RgbwValues{6, 108, 18, 24},
        "Group Power ON restores Fixture 20 independently rather than homogenizing Group");

    dmxwb::GroupControlCommand off = on;
    off.boolean_value = false;
    const auto off_result = manager.apply_group_command(off, t0 + std::chrono::milliseconds{1});
    expect_true(off_result.applied && !off_result.state.actual_power,
        "Group Power OFF factual state is OFF when all members are OFF");
    expect_true(f10 != nullptr && f10->saved_rgbw() == dmxwb::RgbwValues{200, 10, 20, 30} && f10->brightness() == 80,
        "Group Power OFF preserves Fixture 10 saved state");
    expect_true(f20 != nullptr && f20->saved_rgbw() == dmxwb::RgbwValues{10, 180, 30, 40} && f20->brightness() == 60,
        "Group Power OFF preserves Fixture 20 saved state");

    dmxwb::GroupControlCommand reset;
    reset.group_id = 100;
    reset.type = dmxwb::GroupControlType::reset;
    const auto reset_result = manager.apply_group_command(reset, t0 + std::chrono::milliseconds{2});
    expect_true(reset_result.applied && reset_result.state.actual_power,
        "Group Reset applies Fixture Reset and factual Power becomes ON");
    expect_true(f10 != nullptr && f10->requested_power() && f10->brightness() == 100 &&
        f10->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "Group Reset resets first member");
    expect_true(f20 != nullptr && f20->requested_power() && f20->brightness() == 100 &&
        f20->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "Group Reset resets second member");

    dmxwb::GroupControlCommand empty_brightness;
    empty_brightness.group_id = 102;
    empty_brightness.type = dmxwb::GroupControlType::brightness;
    empty_brightness.value = 25;
    const auto empty_result = manager.apply_group_command(empty_brightness, t0 + std::chrono::milliseconds{3});
    expect_true(empty_result.applied && !empty_result.fixture_state_changed,
        "empty Group accepts live setting without touching Fixtures");
    expect_true(!empty_result.state.actual_power && empty_result.state.brightness == 25,
        "empty Group factual Power stays OFF while last Brightness is remembered");
}

void test_scene_lifecycle_atomic_model_apply_and_source_preserved() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    auto state = make_state(config);
    state.source = dmxwb::PersistedSource::artnet;
    state.fixtures[0].requested_power = true;
    state.fixtures[1].requested_power = true;
    if (!prepare_runtime_files(temp, config, state)) return;

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::GroupSceneManager manager{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    const auto created = manager.create_scene("Look A", t0);
    expect_true(created.ok && created.config_changed && created.scene_id == 200,
        "Scene Create allocates next stable monotonic ID");
    expect_true(created.revision == 8 && runtime.config().id_counters.next_scene_id == 201,
        "Scene Create atomically increments config revision and next_scene_id");
    expect_true(runtime.config().scenes.size() == 1 && runtime.config().scenes[0].fixtures.size() == 3,
        "Scene Create captures all current Fixture stable IDs");

    auto* f10 = fixture_by_id(runtime, 10);
    auto* f20 = fixture_by_id(runtime, 20);
    auto* f30 = fixture_by_id(runtime, 30);
    if (f10 == nullptr || f20 == nullptr || f30 == nullptr) return;

    f10->set_color(1, 2, 3);
    (void)f10->set_brightness(10);
    (void)f10->set_temperature(77);
    f10->set_power(false);
    f20->set_color(4, 5, 6);
    (void)f20->set_brightness(20);
    (void)f20->set_temperature(66);
    f20->set_power(false);
    f30->set_color(7, 8, 9);
    (void)f30->set_brightness(30);
    (void)f30->set_temperature(55);
    f30->set_power(true);
    runtime.mark_fixture_state_changed(t0 + std::chrono::milliseconds{1});

    const auto applied = manager.apply_scene(200, t0 + std::chrono::milliseconds{2});
    expect_true(applied.ok && applied.fixture_state_changed && !applied.config_changed,
        "Scene Apply mutates Fixture model without structural config transaction");
    expect_true(runtime.source() == dmxwb::PersistedSource::artnet,
        "Scene Apply never switches Source");
    expect_true(f10->saved_rgbw() == dmxwb::RgbwValues{200, 10, 20, 30} && f10->brightness() == 80 && f10->requested_power(),
        "Scene Apply restores Fixture 10 saved snapshot");
    expect_true(f20->saved_rgbw() == dmxwb::RgbwValues{10, 180, 30, 40} && f20->brightness() == 60 && f20->requested_power(),
        "Scene Apply restores Fixture 20 saved snapshot");
    expect_true(f10->temperature() == 77 && f20->temperature() == 66,
        "Scene Apply preserves last Temperature because Scene schema does not store Temperature");

    const auto one_snapshot = runtime.fixtures().build_snapshot(1);
    expect_true(one_snapshot != nullptr, "one whole DmxSnapshot can be built after complete Scene model apply");
    if (one_snapshot != nullptr) {
        expect_true(one_snapshot->channel(1) == std::optional<std::uint8_t>{160} &&
                    one_snapshot->channel(5) == std::optional<std::uint8_t>{6},
            "single post-Apply snapshot contains restored values for multiple Fixtures together");
    }

    f10->set_color(90, 91, 92);
    f10->set_power(true);
    f20->set_color(80, 81, 82);
    f20->set_power(true);
    const auto overwritten = manager.overwrite_scene(200, t0 + std::chrono::milliseconds{3});
    expect_true(overwritten.ok && overwritten.revision == 9,
        "Scene Overwrite atomically captures new current state");

    const auto renamed = manager.rename_scene(200, "Renamed", t0 + std::chrono::milliseconds{4});
    expect_true(renamed.ok && renamed.revision == 10 && runtime.config().scenes[0].name == "Renamed",
        "Scene Rename persists through config transaction");

    const auto deleted = manager.delete_scene(200, t0 + std::chrono::milliseconds{5});
    expect_true(deleted.ok && deleted.revision == 11 && runtime.config().scenes.empty(),
        "Scene Delete removes Scene without rewinding ID counter");

    const auto recreated = manager.create_scene("Look B", t0 + std::chrono::milliseconds{6});
    expect_true(recreated.ok && recreated.scene_id == 201 && runtime.config().id_counters.next_scene_id == 202,
        "deleted Scene stable ID is never reused");

    const auto flush = runtime.flush_state();
    expect_true(flush.ok(), "Scene lifecycle dirty Fixture state flushes before restart");
    dmxwb::PersistenceRuntime restarted{temp.file("config.json"), temp.file("state.json")};
    expect_true(restarted.config().scenes.size() == 1 && restarted.config().scenes[0].id == 201,
        "Scene lifecycle survives PersistenceRuntime restart");
}

void test_fixture_deletion_cleans_groups_and_scene_ignores_missing_then_new_fixture() {
    TempDirectory temp;
    if (!temp.valid()) return;
    auto config = make_config();
    config.fixture_count = 2;
    config.fixtures = {{10, "Fixture 10"}, {20, "Fixture 20"}};
    config.groups = {{100, "Group", {10, 20}}};
    config.scenes = {
        {200, "Historical", {
            {10, {100, 0, 0, 0}, 100, true},
            {20, {0, 100, 0, 0}, 100, true},
        }},
    };
    config.id_counters = {21, 101, 201};
    auto state = dmxwb::make_default_state(config);
    if (!prepare_runtime_files(temp, config, state)) return;

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::GroupSceneManager manager{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    auto shrink = runtime.config();
    shrink.fixture_count = 1;
    shrink.fixtures.erase(shrink.fixtures.begin() + 1);
    // Intentionally leave deleted ID=20 in Group and Scene proposal. Runtime
    // must clean Group membership automatically while Scene keeps history.
    const auto shrink_result = runtime.apply_config_transaction(runtime.config().revision, shrink, t0);
    expect_true(shrink_result.ok(), "Fixture deletion transaction accepts historical Scene snapshot");
    expect_true(runtime.config().groups[0].members == std::vector<dmxwb::Fixture::Id>{10},
        "Fixture deletion automatically cleans Group membership");
    expect_true(runtime.config().scenes[0].fixtures.size() == 2 &&
                runtime.config().scenes[0].fixtures[1].fixture_id == 20,
        "Scene keeps deleted stable ID as historical snapshot");

    manager.synchronize_config();
    const auto historical_apply = manager.apply_scene(200, t0 + std::chrono::milliseconds{1});
    expect_true(historical_apply.ok && historical_apply.fixture_state_changed,
        "Scene Apply ignores missing deleted Fixture instead of failing");
    auto* f10 = fixture_by_id(runtime, 10);
    expect_true(f10 != nullptr && f10->saved_rgbw() == dmxwb::RgbwValues{100, 0, 0, 0},
        "Scene still applies surviving Fixture record");

    auto grow = runtime.config();
    grow.fixture_count = 2;
    grow.fixtures.push_back({21, "New Fixture"});
    grow.id_counters.next_fixture_id = 22;
    const auto grow_result = runtime.apply_config_transaction(runtime.config().revision, grow, t0 + std::chrono::milliseconds{2});
    expect_true(grow_result.ok(), "Fixture addition after deletion uses fresh stable ID");
    manager.synchronize_config();

    auto* fresh = fixture_by_id(runtime, 21);
    expect_true(fresh != nullptr && fresh->id() == 21,
        "new Fixture does not reuse deleted stable ID 20");
    if (fresh == nullptr) return;
    fresh->set_color(9, 8, 7);
    (void)fresh->set_brightness(50);
    fresh->set_power(true);
    const auto fresh_before = runtime.capture_state().fixtures[1];

    const auto apply_again = manager.apply_scene(200, t0 + std::chrono::milliseconds{3});
    expect_true(apply_again.ok, "historical Scene applies after new Fixture addition");
    const auto fresh_after = runtime.capture_state().fixtures[1];
    expect_true(fresh_after == fresh_before,
        "Fixture created after Scene snapshot remains untouched by Scene Apply");
}

void test_group_state_survives_same_stable_id_config_change() {
    TempDirectory temp;
    if (!temp.valid()) return;
    const auto config = make_config();
    const auto state = make_state(config);
    if (!prepare_runtime_files(temp, config, state)) return;

    dmxwb::PersistenceRuntime runtime{temp.file("config.json"), temp.file("state.json")};
    dmxwb::GroupSceneManager manager{runtime};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};

    dmxwb::GroupControlCommand brightness;
    brightness.group_id = 100;
    brightness.type = dmxwb::GroupControlType::brightness;
    brightness.value = 42;
    expect_true(manager.apply_group_command(brightness, t0).applied,
        "Group last setting prepared before config revision change");

    auto proposed = runtime.config();
    proposed.groups[0].name = "Group renamed";
    proposed.groups[0].members = {10};
    const auto committed = runtime.apply_config_transaction(runtime.config().revision, proposed, t0 + std::chrono::milliseconds{1});
    expect_true(committed.ok(), "same Group stable ID survives structural config change");

    const auto state_after = manager.group_state(100);
    expect_true(state_after.has_value() && state_after->brightness == 42,
        "Group ephemeral last control state follows stable ID across config revision");
}

}  // namespace

int main() {
    test_multiple_groups_and_last_command_wins();
    test_group_power_restore_reset_factual_and_empty();
    test_scene_lifecycle_atomic_model_apply_and_source_preserved();
    test_fixture_deletion_cleans_groups_and_scene_ignores_missing_then_new_fixture();
    test_group_state_survives_same_stable_id_config_change();

    if (failures != 0) {
        std::cerr << failures << " DEV-008 Group/Scene test(s) failed\n";
        return 1;
    }
    std::cout << "All DEV-008 Group/Scene tests passed\n";
    return 0;
}
