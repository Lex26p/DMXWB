#include "dmxwb/persistence_runtime.hpp"

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

void expect_true(bool condition, std::string_view test_name) {
    if (condition) {
        std::cout << "[PASS] " << test_name << '\n';
        return;
    }
    ++failures;
    std::cerr << "[FAIL] " << test_name << '\n';
}

class TempDirectory final {
public:
    TempDirectory() {
        auto base = std::filesystem::temp_directory_path();
        path_ = base / ("dmxwb-dev006-runtime-" + std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX");
        auto native = path_.string();
        native.push_back('\0');
        char* created = ::mkdtemp(native.data());
        if (created == nullptr) {
            path_.clear();
        } else {
            path_ = std::filesystem::path{created};
        }
    }

    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::string file(std::string_view name) const {
        return (path_ / std::string{name}).string();
    }

private:
    std::filesystem::path path_;
};

std::string read_plain_text(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

dmxwb::AppConfig make_two_fixture_config() {
    auto config = dmxwb::make_default_config();
    config.revision = 7;
    config.fixture_count = 2;
    config.start_address = 9;
    config.fixtures = {
        dmxwb::FixtureConfigRecord{10, "Front"},
        dmxwb::FixtureConfigRecord{20, "Back"}};
    config.id_counters.next_fixture_id = 21;
    return config;
}

dmxwb::AppState make_two_fixture_state(const dmxwb::AppConfig& config) {
    auto state = dmxwb::make_default_state(config);
    state.source = dmxwb::PersistedSource::artnet;
    state.fixtures[0].requested_power = true;
    state.fixtures[0].rgbw = dmxwb::RgbwValues{11, 22, 33, 44};
    state.fixtures[0].brightness = 55;
    state.fixtures[0].temperature = 66;
    state.fixtures[1].rgbw = dmxwb::RgbwValues{101, 102, 103, 104};
    state.fixtures[1].brightness = 75;
    state.fixtures[1].temperature = 80;
    return state;
}

bool write_initial_files(
    const std::string& config_path,
    const std::string& state_path,
    const dmxwb::AppConfig& config,
    const dmxwb::AppState& state) {
    const auto config_error = dmxwb::write_persistence_text_file_atomic(
        config_path,
        dmxwb::serialize_config_json(config));
    const auto state_error = dmxwb::save_state_file_atomic(state_path, state, config);
    return !config_error && !state_error;
}

void test_restart_restores_model_and_debounced_state() {
    using namespace std::chrono_literals;
    using Runtime = dmxwb::PersistenceRuntime;

    TempDirectory temp;
    expect_true(temp.valid(), "runtime restart temporary directory created");
    if (!temp.valid()) {
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_two_fixture_config();
    const auto state = make_two_fixture_state(config);
    expect_true(
        write_initial_files(config_path, state_path, config, state),
        "runtime restart initial persistence files written");

    Runtime first{config_path, state_path};
    expect_true(first.startup_status().ok(), "runtime startup loads valid config and state");
    expect_true(first.config() == config, "runtime startup preserves canonical config");
    expect_true(first.source() == dmxwb::PersistedSource::artnet, "runtime startup restores source");
    expect_true(first.fixtures().fixture_count() == 2, "runtime startup restores fixture count");
    expect_true(first.fixtures().start_address() == 9, "runtime startup restores start address");
    expect_true(first.fixtures().next_fixture_id() == 21, "runtime startup restores next fixture ID");

    auto* first_fixture = first.fixture_at(0);
    expect_true(first_fixture != nullptr, "runtime restored first fixture");
    if (first_fixture == nullptr) {
        return;
    }
    expect_true(first_fixture->id() == 10, "runtime restored stable fixture ID");
    expect_true(first_fixture->saved_rgbw() == dmxwb::RgbwValues{11, 22, 33, 44}, "runtime restored saved RGBW");
    expect_true(first_fixture->brightness() == 55, "runtime restored brightness");
    expect_true(first_fixture->temperature() == 66, "runtime restored temperature");
    expect_true(first_fixture->requested_power(), "runtime restored requested power");

    const Runtime::time_point t0{};
    first_fixture->set_color(201, 202, 203);
    expect_true(first_fixture->set_brightness(40), "runtime test changes brightness");
    first_fixture->set_power(false);
    first.mark_fixture_state_changed(t0);
    first.set_source(dmxwb::PersistedSource::mqtt, t0 + 500ms);

    auto save = first.save_state_if_due(t0 + 2499ms);
    expect_true(save.action == dmxwb::StateSaveAction::not_due, "runtime state waits for latest-change debounce");
    save = first.save_state_if_due(t0 + 2500ms);
    expect_true(save.action == dmxwb::StateSaveAction::saved, "runtime state saves at debounce deadline");
    expect_true(!first.state_dirty(), "runtime successful debounce save clears dirty flag");

    Runtime restarted{config_path, state_path};
    expect_true(restarted.startup_status().ok(), "second runtime simulates clean restart");
    expect_true(restarted.source() == dmxwb::PersistedSource::mqtt, "restart restores changed source");
    const auto* restarted_fixture = restarted.fixture_at(0);
    expect_true(restarted_fixture != nullptr, "restart restores first fixture");
    if (restarted_fixture != nullptr) {
        expect_true(restarted_fixture->id() == 10, "restart preserves stable fixture ID");
        expect_true(
            restarted_fixture->saved_rgbw() == dmxwb::RgbwValues{201, 202, 203, 0},
            "restart restores changed saved RGBW");
        expect_true(restarted_fixture->brightness() == 40, "restart restores changed brightness");
        expect_true(!restarted_fixture->requested_power(), "restart restores changed requested power");
    }
    expect_true(restarted.fixtures().next_fixture_id() == 21, "restart preserves monotonic next fixture ID");
}

void test_forced_flush_persists_shutdown_state() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "shutdown flush temporary directory created");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_two_fixture_config();
    const auto state = make_two_fixture_state(config);
    expect_true(write_initial_files(config_path, state_path, config, state), "shutdown initial files written");

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    auto* fixture = runtime.fixture_at(1);
    expect_true(fixture != nullptr, "shutdown runtime fixture available");
    if (fixture == nullptr) {
        return;
    }
    fixture->set_power(true);
    fixture->set_color(9, 8, 7);
    runtime.mark_fixture_state_changed(dmxwb::PersistenceRuntime::time_point{});
    expect_true(runtime.state_dirty(), "shutdown runtime state marked dirty");

    const auto flushed = runtime.flush_state();
    expect_true(flushed.action == dmxwb::StateSaveAction::saved, "forced shutdown flush saves dirty state");

    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    const auto* restored = restarted.fixture_at(1);
    expect_true(restored != nullptr, "forced-flush restart fixture available");
    if (restored != nullptr) {
        expect_true(restored->requested_power(), "forced flush persists requested power");
        expect_true(restored->saved_rgbw() == dmxwb::RgbwValues{9, 8, 7, 0}, "forced flush persists RGB takeover state");
    }
}

void test_config_transaction_is_atomic_and_preserves_matching_state() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "config transaction temporary directory created");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_two_fixture_config();
    const auto state = make_two_fixture_state(config);
    expect_true(write_initial_files(config_path, state_path, config, state), "config transaction initial files written");

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    const auto original_text = read_plain_text(config_path);

    auto proposed = runtime.config();
    proposed.start_address = 5;
    proposed.fixture_count = 3;
    proposed.fixtures.push_back(dmxwb::FixtureConfigRecord{21, "New"});
    proposed.id_counters.next_fixture_id = 22;

    const auto stale = runtime.apply_config_transaction(
        runtime.config().revision - 1,
        proposed,
        dmxwb::PersistenceRuntime::time_point{});
    expect_true(!stale.ok(), "stale runtime config transaction rejected");
    expect_true(
        stale.error.code == dmxwb::PersistenceFileErrorCode::revision_conflict,
        "stale runtime config reports revision conflict");
    expect_true(read_plain_text(config_path) == original_text, "stale runtime config leaves file unchanged");
    expect_true(runtime.fixtures().fixture_count() == 2, "stale runtime config leaves in-memory model unchanged");

    const auto committed = runtime.apply_config_transaction(
        runtime.config().revision,
        proposed,
        dmxwb::PersistenceRuntime::time_point{});
    expect_true(committed.ok(), "valid runtime config transaction commits");
    if (!committed.ok()) {
        return;
    }
    expect_true(runtime.config().revision == 8, "runtime config transaction increments revision");
    expect_true(runtime.fixtures().fixture_count() == 3, "runtime config transaction applies fixture count atomically");
    expect_true(runtime.fixtures().start_address() == 5, "runtime config transaction applies start address");
    expect_true(runtime.fixtures().next_fixture_id() == 22, "runtime config transaction applies next fixture ID");
    const auto* preserved = runtime.fixture_at(0);
    const auto* fresh = runtime.fixture_at(2);
    expect_true(preserved != nullptr && preserved->saved_rgbw() == dmxwb::RgbwValues{11, 22, 33, 44}, "config transaction preserves matching fixture state by stable ID");
    expect_true(fresh != nullptr && fresh->id() == 21, "config transaction creates configured new stable ID");
    if (fresh != nullptr) {
        expect_true(!fresh->requested_power(), "new configured fixture starts safely OFF");
        expect_true(fresh->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255}, "new configured fixture gets default saved RGBW");
    }
    expect_true(runtime.state_dirty(), "structural config change marks state dirty for matching state-file rewrite");

    const auto disk_config = dmxwb::load_config_file(config_path);
    expect_true(disk_config.ok(), "runtime committed config reloads from disk");
    if (disk_config.ok()) {
        expect_true(*disk_config.value == runtime.config(), "disk config equals runtime canonical config after commit");
    }

    const auto flush = runtime.flush_state();
    expect_true(flush.action == dmxwb::StateSaveAction::saved, "config transaction state flush succeeds");
    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.startup_status().ok(), "restart accepts transaction-updated config/state pair");
    expect_true(restarted.fixtures().fixture_count() == 3, "restart restores transaction-updated fixture count");
    expect_true(restarted.fixtures().next_fixture_id() == 22, "restart restores transaction-updated ID counter");
}

void test_corrupt_files_use_safe_runtime_fallbacks() {
    TempDirectory corrupt_config_temp;
    if (!corrupt_config_temp.valid()) {
        expect_true(false, "corrupt config runtime temporary directory created");
        return;
    }

    const auto corrupt_config_path = corrupt_config_temp.file("config.json");
    const auto missing_state_path = corrupt_config_temp.file("state.json");
    const std::string corrupt_config = "{bad-config";
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(corrupt_config_path, corrupt_config),
        "runtime corrupt config fixture written");
    dmxwb::PersistenceRuntime defaults{corrupt_config_path, missing_state_path};
    expect_true(static_cast<bool>(defaults.startup_status().config_error), "runtime exposes corrupt config diagnostic");
    expect_true(defaults.config() == dmxwb::make_default_config(), "runtime corrupt config uses safe defaults");
    expect_true(defaults.fixtures().fixture_count() == 0, "runtime corrupt config starts with zero fixtures");
    expect_true(read_plain_text(corrupt_config_path) == corrupt_config, "runtime does not overwrite corrupt config automatically");

    TempDirectory corrupt_state_temp;
    if (!corrupt_state_temp.valid()) {
        expect_true(false, "corrupt state runtime temporary directory created");
        return;
    }
    const auto config_path = corrupt_state_temp.file("config.json");
    const auto state_path = corrupt_state_temp.file("state.json");
    const auto valid_config = make_two_fixture_config();
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(valid_config)),
        "runtime valid config fixture written");
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(state_path, "[bad-state"),
        "runtime corrupt state fixture written");

    dmxwb::PersistenceRuntime safe_state{config_path, state_path};
    expect_true(!safe_state.startup_status().config_error, "runtime keeps valid config when state is corrupt");
    expect_true(static_cast<bool>(safe_state.startup_status().state_error), "runtime exposes corrupt state diagnostic");
    expect_true(safe_state.config() == valid_config, "runtime preserves valid config with corrupt state");
    expect_true(safe_state.fixtures().fixture_count() == 2, "runtime creates configured fixtures from safe state fallback");
    const auto* fixture = safe_state.fixture_at(0);
    if (fixture != nullptr) {
        expect_true(!fixture->requested_power(), "corrupt state fallback fixture is safely OFF");
        expect_true(fixture->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255}, "corrupt state fallback preserves default saved RGBW");
    }
}

}  // namespace

int main() {
    test_restart_restores_model_and_debounced_state();
    test_forced_flush_persists_shutdown_state();
    test_config_transaction_is_atomic_and_preserves_matching_state();
    test_corrupt_files_use_safe_runtime_fallbacks();

    if (failures != 0) {
        std::cerr << failures << " DEV-006 persistence runtime integration test(s) failed\n";
        return 1;
    }
    std::cout << "DEV-006 persistence runtime integration tests PASS\n";
    return 0;
}
