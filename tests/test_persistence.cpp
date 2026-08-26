#include "dmxwb/persistence.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect_true(bool condition, std::string_view name) {
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
        return;
    }
    ++failures;
    std::cerr << "[FAIL] " << name << '\n';
}

dmxwb::AppConfig make_populated_config() {
    dmxwb::AppConfig config;
    config.revision = 7;
    config.dmx_port = "/dev/ttyRS485-2";
    config.artnet_universe = 123;
    config.fixture_count = 2;
    config.start_address = 9;
    config.fixtures = {
        {42, "Сцена \"лево\""},
        {90, "Светильник 90"},
    };
    config.groups = {
        {5, "Группа 1", {42, 90}},
    };
    config.scenes = {
        {8,
         "Вечер",
         {
             {42, {1, 2, 3, 4}, 55, true},
             {90, {10, 20, 30, 40}, 75, false},
         }},
    };
    config.id_counters = {101, 9, 12};
    return config;
}

dmxwb::AppState make_populated_state() {
    dmxwb::AppState state;
    state.source = dmxwb::PersistedSource::artnet;
    state.fixtures = {
        {42, true, {100, 50, 25, 12}, 80, 33},
        {90, false, {255, 255, 255, 128}, 50, 50},
    };
    return state;
}

void test_default_documents() {
    const auto config = dmxwb::make_default_config();
    expect_true(!dmxwb::validate_config(config), "default config validates");
    expect_true(config.fixture_count == 0 && config.start_address == 1, "default config has zero fixtures at start 1");
    expect_true(config.dmx_port == "/dev/ttyRS485-1", "default config uses ttyRS485-1");
    expect_true(config.artnet_universe == 0, "default config uses Art-Net universe 0");

    const auto state = dmxwb::make_default_state(config);
    expect_true(state.source == dmxwb::PersistedSource::mqtt, "safe default source is mqtt");
    expect_true(state.fixtures.empty(), "default state follows empty config");
}

void test_config_round_trip() {
    const auto original = make_populated_config();
    expect_true(!dmxwb::validate_config(original), "populated config validates");
    const auto json = dmxwb::serialize_config_json(original);
    const auto parsed = dmxwb::parse_config_json(json);
    expect_true(parsed.ok(), "config JSON round-trip parses");
    if (parsed.value.has_value()) {
        expect_true(*parsed.value == original, "config JSON round-trip preserves all fields");
    }
}

void test_state_round_trip() {
    const auto config = make_populated_config();
    const auto original = make_populated_state();
    expect_true(!dmxwb::validate_state(original, config), "populated state validates against config");
    const auto json = dmxwb::serialize_state_json(original);
    const auto parsed = dmxwb::parse_state_json(json);
    expect_true(parsed.ok(), "state JSON round-trip parses");
    if (parsed.value.has_value()) {
        expect_true(*parsed.value == original, "state JSON round-trip preserves saved logical state");
        expect_true(!dmxwb::validate_state(*parsed.value, config), "parsed state validates against config");
    }
}

void test_json_unicode_escape() {
    const std::string json =
        "{\"version\":1,\"revision\":1,\"dmx\":{\"port\":\"/dev/ttyRS485-1\"},"
        "\"artnet\":{\"universe\":0},\"fixtures\":{\"count\":1,\"start_address\":1,"
        "\"items\":[{\"id\":1,\"name\":\"\\u0421\\u0432\\u0435\\u0442\"}]},"
        "\"groups\":[],\"scenes\":[],\"id_counters\":{\"next_fixture_id\":2,"
        "\"next_group_id\":1,\"next_scene_id\":1}}";
    const auto parsed = dmxwb::parse_config_json(json);
    expect_true(parsed.ok(), "JSON parser accepts unicode escapes");
    if (parsed.value.has_value()) {
        expect_true(parsed.value->fixtures[0].name == "Свет", "unicode escape decoded to UTF-8 name");
    }
}

void test_schema_and_version_rejection() {
    const auto corrupt = dmxwb::parse_config_json("{not-json}");
    expect_true(!corrupt.ok() && corrupt.error.code == dmxwb::PersistenceErrorCode::json_syntax,
                "corrupt JSON rejected as syntax error");

    auto config = make_populated_config();
    config.version = 2;
    const auto version_result = dmxwb::parse_config_json(dmxwb::serialize_config_json(config));
    expect_true(!version_result.ok() && version_result.error.code == dmxwb::PersistenceErrorCode::version,
                "unsupported config version rejected");

    const auto extra_field = dmxwb::parse_state_json(
        "{\"version\":1,\"source\":\"mqtt\",\"fixtures\":[],\"unexpected\":1}");
    expect_true(!extra_field.ok() && extra_field.error.code == dmxwb::PersistenceErrorCode::schema,
                "unknown state schema field rejected");
}

void test_config_validation() {
    {
        auto config = make_populated_config();
        config.fixture_count = 75;
        config.start_address = 2;
        config.fixtures.clear();
        config.fixtures.reserve(75);
        for (std::uint64_t id = 1; id <= 75; ++id) {
            config.fixtures.push_back({id, "Fixture"});
        }
        config.groups.clear();
        config.scenes.clear();
        config.id_counters = {76, 1, 1};
        expect_true(dmxwb::validate_config(config).code == dmxwb::PersistenceErrorCode::validation,
                    "fixture range crossing slot 300 rejected");
    }
    {
        auto config = make_populated_config();
        config.groups[0].members.push_back(999);
        expect_true(dmxwb::validate_config(config).code == dmxwb::PersistenceErrorCode::validation,
                    "group with missing fixture rejected");
    }
    {
        auto config = make_populated_config();
        config.scenes[0].fixtures[0].fixture_id = 999;
        expect_true(dmxwb::validate_config(config).code == dmxwb::PersistenceErrorCode::validation,
                    "scene with future/unallocated fixture ID rejected");
    }
    {
        auto config = make_populated_config();
        config.scenes[0].fixtures[0].fixture_id = 43;
        expect_true(!dmxwb::validate_config(config),
                    "scene may keep historical deleted Fixture ID below next_fixture_id");
    }
    {
        auto config = make_populated_config();
        config.id_counters.next_fixture_id = 90;
        expect_true(dmxwb::validate_config(config).code == dmxwb::PersistenceErrorCode::validation,
                    "next fixture ID cannot reuse existing ID");
    }
    {
        auto config = make_populated_config();
        config.dmx_port = "/dev/ttyS0";
        expect_true(dmxwb::validate_config(config).code == dmxwb::PersistenceErrorCode::validation,
                    "unsupported DMX port rejected");
    }
}

void test_revision_conflict() {
    expect_true(!dmxwb::validate_expected_revision(17, 17), "matching expected revision accepted");
    expect_true(
        dmxwb::validate_expected_revision(17, 16).code == dmxwb::PersistenceErrorCode::revision_conflict,
        "stale expected revision rejected atomically");
}

void test_state_validation_and_safe_defaults() {
    const auto config = make_populated_config();
    auto state = make_populated_state();
    state.fixtures.pop_back();
    expect_true(dmxwb::validate_state(state, config).code == dmxwb::PersistenceErrorCode::validation,
                "state missing configured fixture rejected");

    const auto safe = dmxwb::make_default_state(config);
    expect_true(!dmxwb::validate_state(safe, config), "safe fallback state validates against working config");
    expect_true(safe.fixtures.size() == 2, "safe fallback creates one state per configured fixture");
    if (!safe.fixtures.empty()) {
        const auto& first = safe.fixtures.front();
        expect_true(!first.requested_power, "safe fallback fixture is OFF");
        expect_true(first.rgbw == dmxwb::RgbwValues{255, 255, 255, 255}, "safe fallback preserves default RGBW 255");
        expect_true(first.brightness == 100 && first.temperature == 100, "safe fallback brightness/temperature are 100");
    }
}

void test_stable_ids_survive_restore() {
    const auto config = make_populated_config();
    const auto state = make_populated_state();
    dmxwb::FixtureCollection collection;

    const auto restore_error = dmxwb::restore_fixture_collection(config, state, collection);
    expect_true(!restore_error, "persisted Fixture collection restores transactionally");
    expect_true(collection.fixture_count() == 2 && collection.start_address() == 9, "restored collection preserves count/address");
    expect_true(collection.next_fixture_id() == 101, "restored collection preserves monotonic next_fixture_id");

    const auto* first = collection.fixture_at(0);
    const auto* second = collection.fixture_at(1);
    expect_true(first != nullptr && first->id() == 42 && first->name() == "Сцена \"лево\"", "first stable ID/name survive restart");
    expect_true(second != nullptr && second->id() == 90, "second stable ID survives restart");
    if (first != nullptr) {
        expect_true(first->requested_power(), "requested power restored");
        expect_true(first->saved_rgbw() == dmxwb::RgbwValues{100, 50, 25, 12}, "saved RGBW restored without color takeover");
        expect_true(first->brightness() == 80 && first->temperature() == 33, "brightness and last temperature restored independently");
    }

    expect_true(collection.set_fixture_count(1), "restored collection can shrink");
    expect_true(collection.set_fixture_count(2), "restored collection can grow again");
    const auto* replacement = collection.fixture_at(1);
    expect_true(replacement != nullptr && replacement->id() == 101, "deleted persisted ID is not reused after restart");
    expect_true(collection.next_fixture_id() == 102, "next_fixture_id advances after post-restart creation");
}

void test_invalid_restore_does_not_replace_working_collection() {
    dmxwb::FixtureCollection collection;
    expect_true(collection.set_fixture_count(1), "preexisting collection prepared");
    auto* original = collection.fixture_at(0);
    if (original != nullptr) {
        original->set_name("Рабочий");
    }

    const auto config = make_populated_config();
    auto invalid_state = make_populated_state();
    invalid_state.fixtures[0].brightness = 101;
    const auto error = dmxwb::restore_fixture_collection(config, invalid_state, collection);
    expect_true(error.code == dmxwb::PersistenceErrorCode::validation, "invalid persisted state rejected before apply");
    const auto* still_working = collection.fixture_at(0);
    expect_true(collection.fixture_count() == 1 && still_working != nullptr && still_working->name() == "Рабочий",
                "failed restore leaves previous working Fixture collection intact");
}

}  // namespace

int main() {
    test_default_documents();
    test_config_round_trip();
    test_state_round_trip();
    test_json_unicode_escape();
    test_schema_and_version_rejection();
    test_config_validation();
    test_revision_conflict();
    test_state_validation_and_safe_defaults();
    test_stable_ids_survive_restore();
    test_invalid_restore_does_not_replace_working_collection();

    if (failures != 0) {
        std::cerr << failures << " persistence test(s) failed\n";
        return 1;
    }
    std::cout << "All DEV-006A persistence tests passed\n";
    return 0;
}
