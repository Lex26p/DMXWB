#include "dmxwb/persistence_runtime.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
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

void test_runtime_persistence_health_recovers_without_restart() {
    using namespace std::chrono_literals;

    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "runtime health temporary directory created");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_two_fixture_config();
    const auto state = make_two_fixture_state(config);
    expect_true(write_initial_files(config_path, state_path, config, state),
                "runtime health initial files written");

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    const dmxwb::PersistenceRuntime::time_point t0{};
    runtime.mark_fixture_state_changed(t0);

    const std::filesystem::path blocking_temp{state_path + ".tmp"};
    std::error_code ec;
    expect_true(std::filesystem::create_directory(blocking_temp, ec),
                "runtime health state temp blocker created");
    auto result = runtime.save_state_if_due(t0 + 2s);
    expect_true(result.action == dmxwb::StateSaveAction::failed,
                "runtime exposes due state write failure");
    expect_true(!runtime.operational_status().ok() &&
                    !runtime.operational_status().fallback_active &&
                    !runtime.operational_status().last_error().empty(),
                "runtime persistence health reports current write error without false fallback");

    result = runtime.save_state_if_due(t0 + 3s);
    expect_true(result.action == dmxwb::StateSaveAction::not_due,
                "runtime persistence retry is suppressed during backoff");
    std::filesystem::remove(blocking_temp, ec);
    result = runtime.save_state_if_due(t0 + 4s);
    expect_true(result.action == dmxwb::StateSaveAction::saved,
                "runtime saves pending state after storage recovery");
    expect_true(runtime.operational_status().ok() &&
                    runtime.operational_status().last_error().empty() &&
                    !runtime.state_dirty(),
                "successful retry restores factual persistence health without restart");
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
    expect_true(!runtime.state_dirty(),
        "successful config transaction durably writes its matching state immediately");

    const auto disk_config = dmxwb::load_config_file(config_path);
    expect_true(disk_config.ok(), "runtime committed config reloads from disk");
    if (disk_config.ok()) {
        expect_true(*disk_config.value == runtime.config(), "disk config equals runtime canonical config after commit");
    }

    const auto flush = runtime.flush_state();
    expect_true(flush.action == dmxwb::StateSaveAction::not_dirty,
        "successful config transaction needs no deferred state flush");
    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.startup_status().ok(), "restart accepts transaction-updated config/state pair");
    expect_true(restarted.fixtures().fixture_count() == 3, "restart restores transaction-updated fixture count");
    expect_true(restarted.fixtures().next_fixture_id() == 22, "restart restores transaction-updated ID counter");
}

void test_config_transaction_rejects_stable_id_reuse() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "stable ID transaction temporary directory created");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = make_two_fixture_config();
    const auto state = make_two_fixture_state(config);
    expect_true(write_initial_files(config_path, state_path, config, state),
                "stable ID transaction initial files written");

    dmxwb::PersistenceRuntime runtime{config_path, state_path};
    auto removed = runtime.config();
    removed.fixture_count = 1;
    removed.fixtures.pop_back();
    const auto removal = runtime.apply_config_transaction(
        runtime.config().revision,
        removed,
        dmxwb::PersistenceRuntime::time_point{});
    expect_true(removal.ok(), "Fixture deletion commits without reducing its ID counter");
    if (!removal.ok()) {
        return;
    }

    const auto committed_text = read_plain_text(config_path);
    auto reused = runtime.config();
    reused.fixture_count = 2;
    reused.fixtures.push_back(dmxwb::FixtureConfigRecord{20, "Replacement"});
    const auto rejected = runtime.apply_config_transaction(
        runtime.config().revision,
        reused,
        dmxwb::PersistenceRuntime::time_point{});
    expect_true(!rejected.ok() && rejected.error.code == dmxwb::PersistenceFileErrorCode::validation,
                "runtime transaction rejects reuse of deleted Fixture stable ID");
    expect_true(runtime.fixtures().fixture_count() == 1 && read_plain_text(config_path) == committed_text,
                "rejected stable ID reuse leaves model and config file unchanged");
}

void test_config_commit_point_recovers_prepared_state_after_crash() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "commit-point recovery temporary directory created");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto old_config = make_two_fixture_config();
    const auto old_state = make_two_fixture_state(old_config);
    expect_true(write_initial_files(config_path, state_path, old_config, old_state),
        "commit-point recovery initial pair written");

    auto committed_config = old_config;
    committed_config.revision = 8;
    committed_config.fixture_count = 3;
    committed_config.start_address = 5;
    committed_config.fixtures = {
        dmxwb::FixtureConfigRecord{20, "Back"},
        dmxwb::FixtureConfigRecord{10, "Front"},
        dmxwb::FixtureConfigRecord{21, "New"}};
    committed_config.id_counters.next_fixture_id = 22;
    auto prepared_state = dmxwb::reconcile_state_for_config(old_state, committed_config);
    expect_true(prepared_state.ok(),
        "commit-point recovery builds the exact state for the new configuration");
    if (!prepared_state.ok()) {
        return;
    }
    for (auto& fixture : prepared_state.value->fixtures) {
        if (fixture.id == 10) {
            fixture.rgbw = dmxwb::RgbwValues{211, 212, 213, 214};
            fixture.brightness = 42;
        }
    }
    expect_true(
        !dmxwb::prepare_config_state_file(
            state_path,
            *prepared_state.value,
            committed_config),
        "matching state is durable before the config commit point");
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(
            config_path,
            dmxwb::serialize_config_json(committed_config)),
        "simulated crash commits new config while prepared state is pending");

    dmxwb::PersistenceRuntime recovered{config_path, state_path};
    expect_true(recovered.startup_status().ok(),
        "valid previous state is reconciled without startup fallback error");
    expect_true(recovered.config() == committed_config &&
                    recovered.fixtures().fixture_count() == 3,
        "restart selects the durably committed configuration");
    expect_true(!recovered.state_dirty(),
        "startup atomically promotes the prepared matching state");

    const auto* reordered_back = recovered.fixture_at(0);
    const auto* reordered_front = recovered.fixture_at(1);
    const auto* fresh = recovered.fixture_at(2);
    expect_true(reordered_back != nullptr && reordered_back->id() == 20 &&
                    reordered_back->saved_rgbw() == dmxwb::RgbwValues{101, 102, 103, 104},
        "recovery preserves reordered Fixture state by stable ID");
    expect_true(reordered_front != nullptr && reordered_front->id() == 10 &&
                    reordered_front->saved_rgbw() == dmxwb::RgbwValues{211, 212, 213, 214} &&
                    reordered_front->brightness() == 42,
        "recovery preserves the exact prepared state, including unsaved recent changes");
    expect_true(fresh != nullptr && fresh->id() == 21 &&
                    !fresh->requested_power() &&
                    fresh->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "recovery gives a newly configured Fixture safe defaults");

    const auto materialized = recovered.flush_state();
    expect_true(materialized.action == dmxwb::StateSaveAction::not_dirty,
        "promoted transaction needs no deferred state materialization");
    dmxwb::PersistenceRuntime restarted{config_path, state_path};
    expect_true(restarted.startup_status().ok() && !restarted.state_dirty() &&
                    restarted.config() == committed_config,
        "second restart loads the exact coherent config/state pair");
}

void test_config_transaction_io_boundaries() {
    {
        TempDirectory temp;
        if (!temp.valid()) {
            expect_true(false, "pre-commit failure temporary directory created");
            return;
        }

        const auto config_path = temp.file("config.json");
        const auto state_path = temp.file("state.json");
        const auto config = make_two_fixture_config();
        const auto state = make_two_fixture_state(config);
        expect_true(write_initial_files(config_path, state_path, config, state),
            "pre-commit failure initial pair written");
        const auto original_config_text = read_plain_text(config_path);
        const auto original_state_text = read_plain_text(state_path);

        dmxwb::PersistenceRuntime runtime{config_path, state_path};
        auto proposed = runtime.config();
        proposed.fixture_count = 3;
        proposed.fixtures.push_back(dmxwb::FixtureConfigRecord{21, "New"});
        proposed.id_counters.next_fixture_id = 22;

        std::error_code ec;
        const std::filesystem::path config_temp_blocker{config_path + ".tmp"};
        expect_true(std::filesystem::create_directory(config_temp_blocker, ec),
            "config commit temporary path blocked");
        const auto rejected = runtime.apply_config_transaction(
            runtime.config().revision,
            proposed,
            dmxwb::PersistenceRuntime::time_point{});
        expect_true(!rejected.ok(),
            "failure before atomic config commit is returned as transaction error");
        expect_true(runtime.config() == config && runtime.fixtures().fixture_count() == 2,
            "pre-commit failure leaves active in-memory model unchanged");
        expect_true(read_plain_text(config_path) == original_config_text &&
                        read_plain_text(state_path) == original_state_text,
            "pre-commit failure leaves the old durable pair unchanged");
    }

    {
        TempDirectory temp;
        if (!temp.valid()) {
            expect_true(false, "post-commit state failure temporary directory created");
            return;
        }

        const auto config_path = temp.file("config.json");
        const auto state_path = temp.file("state.json");
        const auto config = make_two_fixture_config();
        const auto state = make_two_fixture_state(config);
        expect_true(write_initial_files(config_path, state_path, config, state),
            "post-commit state failure initial pair written");

        dmxwb::PersistenceRuntime runtime{config_path, state_path};
        auto proposed = runtime.config();
        proposed.fixture_count = 3;
        proposed.fixtures.push_back(dmxwb::FixtureConfigRecord{21, "New"});
        proposed.id_counters.next_fixture_id = 22;

        std::error_code ec;
        expect_true(std::filesystem::remove(state_path, ec),
            "old state target removed for finalize-failure simulation");
        const std::filesystem::path state_target_blocker{state_path};
        expect_true(std::filesystem::create_directory(state_target_blocker, ec),
            "state finalize target blocked after prepared-state write remains possible");
        const auto committed = runtime.apply_config_transaction(
            runtime.config().revision,
            proposed,
            dmxwb::PersistenceRuntime::time_point{});
        expect_true(committed.ok() && runtime.fixtures().fixture_count() == 3,
            "durable config commit remains successful when state mirror is temporarily blocked");
        expect_true(runtime.state_dirty(),
            "failed post-commit state materialization remains pending");

        dmxwb::PersistenceRuntime recovered{config_path, state_path};
        expect_true(recovered.startup_status().ok() &&
                        recovered.fixtures().fixture_count() == 3 &&
                        recovered.state_dirty(),
            "restart reconstructs the committed model from the previous valid state");
        const auto* preserved = recovered.fixture_at(0);
        expect_true(preserved != nullptr &&
                        preserved->saved_rgbw() == dmxwb::RgbwValues{11, 22, 33, 44},
            "post-commit recovery does not reset preserved Fixture state");

        std::filesystem::remove(state_target_blocker, ec);
        const auto saved = recovered.flush_state();
        expect_true(saved.action == dmxwb::StateSaveAction::saved,
            "pending coherent state saves after storage recovery");
        dmxwb::PersistenceRuntime final_restart{config_path, state_path};
        expect_true(final_restart.startup_status().ok() &&
                        !final_restart.state_dirty() &&
                        final_restart.fixtures().fixture_count() == 3,
            "final restart loads the fully materialized committed pair");
    }
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
    const auto corrected_config = defaults.apply_config_transaction(
        defaults.config().revision,
        defaults.config(),
        dmxwb::PersistenceRuntime::time_point{});
    expect_true(corrected_config.ok() && defaults.operational_status().ok() &&
                    !defaults.operational_status().fallback_active,
                "explicit valid config commit clears startup config fallback without restart");

    TempDirectory preserved_state_temp;
    if (!preserved_state_temp.valid()) {
        expect_true(false, "preserved state fallback temporary directory created");
        return;
    }
    const auto preserved_config_path = preserved_state_temp.file("config.json");
    const auto preserved_state_path = preserved_state_temp.file("state.json");
    const auto original_config = make_two_fixture_config();
    const auto original_state = make_two_fixture_state(original_config);
    expect_true(write_initial_files(
                    preserved_config_path,
                    preserved_state_path,
                    original_config,
                    original_state),
                "valid state preservation fixture written");
    const auto original_state_bytes = read_plain_text(preserved_state_path);
    expect_true(!dmxwb::write_persistence_text_file_atomic(
                    preserved_config_path, "{corrupt-config"),
                "state preservation config corrupted");

    dmxwb::PersistenceRuntime protected_fallback{
        preserved_config_path, preserved_state_path};
    const auto t0 = dmxwb::PersistenceRuntime::time_point{};
    protected_fallback.set_source(dmxwb::PersistedSource::artnet, t0);
    protected_fallback.mark_fixture_state_changed(t0);
    const auto due = protected_fallback.save_state_if_due(t0 + std::chrono::seconds{20});
    const auto flush = protected_fallback.flush_state();
    expect_true(due.action == dmxwb::StateSaveAction::not_dirty &&
                    flush.action == dmxwb::StateSaveAction::not_dirty &&
                    !protected_fallback.state_dirty() &&
                    !protected_fallback.next_state_save_deadline().has_value(),
                "config fallback suppresses scheduled and forced state writes");
    expect_true(read_plain_text(preserved_state_path) == original_state_bytes,
                "config fallback leaves valid state byte-for-byte unchanged");

    expect_true(!dmxwb::write_persistence_text_file_atomic(
                    preserved_config_path,
                    dmxwb::serialize_config_json(original_config)),
                "valid config restored externally");
    dmxwb::PersistenceRuntime restored{
        preserved_config_path, preserved_state_path};
    const auto* restored_fixture = restored.fixture_at(0);
    expect_true(restored.startup_status().ok() && restored_fixture != nullptr &&
                    restored_fixture->requested_power() &&
                    restored_fixture->saved_rgbw() == dmxwb::RgbwValues{11, 22, 33, 44},
                "restored config recovers the previously preserved Fixture values");

    expect_true(!dmxwb::write_persistence_text_file_atomic(
                    preserved_config_path, "{corrupt-again"),
                "config corrupted again for explicit repair");
    dmxwb::PersistenceRuntime repairable_fallback{
        preserved_config_path, preserved_state_path};
    auto repair_config = original_config;
    repair_config.revision = repairable_fallback.config().revision;
    const auto repaired = repairable_fallback.apply_config_transaction(
        repairable_fallback.config().revision,
        repair_config,
        t0 + std::chrono::seconds{1});
    const auto* repaired_fixture = repairable_fallback.fixture_at(0);
    expect_true(repaired.ok() && repairable_fallback.operational_status().ok() &&
                    repaired_fixture != nullptr && repaired_fixture->requested_power() &&
                    repaired_fixture->saved_rgbw() == dmxwb::RgbwValues{11, 22, 33, 44},
                "explicit config repair reconciles from preserved state instead of fallback state");

    const auto shared_path = preserved_state_temp.file("shared.json");
    const std::string shared_contents{"shared-persistence-file"};
    expect_true(!dmxwb::write_persistence_text_file_atomic(shared_path, shared_contents),
                "runtime shared path fixture written");
    bool shared_path_rejected = false;
    try {
        dmxwb::PersistenceRuntime invalid_paths{shared_path, shared_path};
    } catch (const std::invalid_argument&) {
        shared_path_rejected = true;
    }
    expect_true(shared_path_rejected && read_plain_text(shared_path) == shared_contents,
                "runtime rejects identical paths before modifying their file");

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
    const auto corrected_state = safe_state.flush_state();
    expect_true(corrected_state.action == dmxwb::StateSaveAction::saved &&
                    safe_state.operational_status().ok() &&
                    !safe_state.operational_status().fallback_active,
                "successful corrective state write clears startup state fallback without restart");
}

}  // namespace

int main() {
    test_restart_restores_model_and_debounced_state();
    test_forced_flush_persists_shutdown_state();
    test_runtime_persistence_health_recovers_without_restart();
    test_config_transaction_is_atomic_and_preserves_matching_state();
    test_config_transaction_rejects_stable_id_reuse();
    test_config_commit_point_recovers_prepared_state_after_crash();
    test_config_transaction_io_boundaries();
    test_corrupt_files_use_safe_runtime_fallbacks();

    if (failures != 0) {
        std::cerr << failures << " DEV-006 persistence runtime integration test(s) failed\n";
        return 1;
    }
    std::cout << "DEV-006 persistence runtime integration tests PASS\n";
    return 0;
}
