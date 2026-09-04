#include "dmxwb/persistence_storage.hpp"

#include <chrono>
#include <cstdlib>
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
        path_ = base / ("dmxwb-dev006b-" + std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX");
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

void test_atomic_write_success_and_failure_preserves_old_file() {
    TempDirectory temp;
    expect_true(temp.valid(), "temporary directory created");
    if (!temp.valid()) {
        return;
    }

    const auto path = temp.file("config.json");
    auto error = dmxwb::write_persistence_text_file_atomic(path, "old-value");
    expect_true(!error, "initial atomic write succeeds");
    expect_true(read_plain_text(path) == "old-value", "initial atomic content persisted");

    const std::filesystem::path blocking_temp{path + ".tmp"};
    std::error_code ec;
    expect_true(std::filesystem::create_directory(blocking_temp, ec), "blocking temporary directory created");

    error = dmxwb::write_persistence_text_file_atomic(path, "new-value");
    expect_true(static_cast<bool>(error), "atomic write failure is reported");
    expect_true(read_plain_text(path) == "old-value", "failed atomic write preserves old target");
}

void test_read_size_limit() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for size limit");
        return;
    }

    const auto path = temp.file("large.json");
    const auto error = dmxwb::write_persistence_text_file_atomic(path, "1234567890");
    expect_true(!error, "size-limit fixture written");

    const auto result = dmxwb::read_persistence_text_file(path, 4);
    expect_true(!result.ok(), "oversized persistence file rejected");
    expect_true(
        result.error.code == dmxwb::PersistenceFileErrorCode::too_large,
        "oversized persistence file reports too_large");
}

void test_config_transaction_and_revision_conflict() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for config transaction");
        return;
    }

    const auto path = temp.file("config.json");
    const auto proposed = dmxwb::make_default_config();
    const auto committed = dmxwb::commit_config_file_atomic(path, 0, 0, proposed);
    expect_true(committed.ok(), "config transaction commits valid proposal");
    if (!committed.ok()) {
        return;
    }
    expect_true(committed.value->revision == 1, "config transaction increments revision");

    const auto first_file = read_plain_text(path);
    const auto stale = dmxwb::commit_config_file_atomic(path, 1, 0, proposed);
    expect_true(!stale.ok(), "stale expected revision rejected");
    expect_true(
        stale.error.code == dmxwb::PersistenceFileErrorCode::revision_conflict,
        "stale expected revision reports revision conflict");
    expect_true(read_plain_text(path) == first_file, "rejected config transaction leaves file unchanged");

    const auto loaded = dmxwb::load_config_file(path);
    expect_true(loaded.ok(), "committed config loads from disk");
    if (loaded.ok()) {
        expect_true(*loaded.value == *committed.value, "loaded config equals committed canonical config");
    }
}

void test_oversized_canonical_config_is_not_committed() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for oversized config transaction");
        return;
    }

    const auto path = temp.file("config.json");
    const std::string original{"original-config"};
    expect_true(!dmxwb::write_persistence_text_file_atomic(path, original),
                "oversized config transaction original target written");

    auto proposed = dmxwb::make_default_config();
    const std::string name(dmxwb::kEntityNameMaxBytes, 'x');
    constexpr std::uint64_t kGroupCount = 17000;
    proposed.groups.reserve(kGroupCount);
    for (std::uint64_t id = 1; id <= kGroupCount; ++id) {
        proposed.groups.push_back(dmxwb::GroupConfigRecord{id, name, {}});
    }
    proposed.id_counters.next_group_id = kGroupCount + 1U;
    expect_true(dmxwb::serialize_config_json(proposed).size() > dmxwb::kPersistenceMaxFileBytes,
                "oversized canonical config fixture exceeds persistence limit");

    const auto committed = dmxwb::commit_config_file_atomic(path, 0, 0, proposed);
    expect_true(!committed.ok() && committed.error.code == dmxwb::PersistenceFileErrorCode::too_large,
                "oversized canonical config rejected before commit");
    expect_true(read_plain_text(path) == original,
                "oversized canonical config leaves previous target unchanged");
}

void test_corrupt_config_uses_defaults_without_overwrite() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for corrupt config");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const std::string corrupt = "{not-json";
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(config_path, corrupt),
        "corrupt config fixture written");

    const auto loaded = dmxwb::load_persistence_files(config_path, state_path);
    expect_true(static_cast<bool>(loaded.config_error), "corrupt config error retained for diagnostics");
    expect_true(loaded.config == dmxwb::make_default_config(), "corrupt config falls back to safe defaults");
    expect_true(!loaded.state_writes_allowed,
                "corrupt config disables state writes for the fallback model");
    expect_true(read_plain_text(config_path) == corrupt, "corrupt config is not automatically overwritten");
}

void test_config_and_state_path_identity_is_rejected() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for path identity");
        return;
    }

    const auto shared = temp.file("shared.json");
    const auto distinct = temp.file("state.json");
    expect_true(!dmxwb::write_persistence_text_file_atomic(shared, "shared"),
                "shared path fixture written");
    expect_true(!dmxwb::write_persistence_text_file_atomic(distinct, "state"),
                "distinct path fixture written");

    expect_true(static_cast<bool>(dmxwb::validate_persistence_paths(shared, shared)),
                "identical literal config/state path rejected");
    expect_true(static_cast<bool>(dmxwb::validate_persistence_paths(
                    shared, temp.file("unused/../shared.json"))),
                "lexically equivalent config/state path rejected");

    const auto symlink = temp.file("shared-symlink.json");
    const auto hardlink = temp.file("shared-hardlink.json");
    std::error_code error;
    std::filesystem::create_symlink(shared, symlink, error);
    expect_true(!error && static_cast<bool>(dmxwb::validate_persistence_paths(shared, symlink)),
                "symlink config/state alias rejected");
    error.clear();
    std::filesystem::create_hard_link(shared, hardlink, error);
    expect_true(!error && static_cast<bool>(dmxwb::validate_persistence_paths(shared, hardlink)),
                "hard-link config/state alias rejected");
    expect_true(!dmxwb::validate_persistence_paths(shared, distinct),
                "distinct config/state paths accepted");
    expect_true(read_plain_text(shared) == "shared",
                "path validation never modifies the shared file");
}

void test_corrupt_state_keeps_valid_config_and_uses_default_state() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for corrupt state");
        return;
    }

    const auto config_path = temp.file("config.json");
    const auto state_path = temp.file("state.json");
    const auto config = dmxwb::make_default_config();
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(config_path, dmxwb::serialize_config_json(config)),
        "valid config fixture written");
    expect_true(
        !dmxwb::write_persistence_text_file_atomic(state_path, "[broken-state"),
        "corrupt state fixture written");

    const auto loaded = dmxwb::load_persistence_files(config_path, state_path);
    expect_true(!loaded.config_error, "valid config remains accepted when state is corrupt");
    expect_true(loaded.config == config, "valid config preserved with corrupt state");
    expect_true(static_cast<bool>(loaded.state_error), "corrupt state error retained for diagnostics");
    expect_true(
        loaded.state == dmxwb::make_default_state(config),
        "corrupt state falls back to safe fixture state");
}

void test_state_save_and_load_round_trip() {
    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for state round trip");
        return;
    }

    const auto path = temp.file("state.json");
    const auto config = dmxwb::make_default_config();
    auto state = dmxwb::make_default_state(config);
    state.source = dmxwb::PersistedSource::artnet;

    const auto error = dmxwb::save_state_file_atomic(path, state, config);
    expect_true(!error, "state atomic save succeeds");
    const auto loaded = dmxwb::load_state_file(path, config);
    expect_true(loaded.ok(), "saved state loads");
    if (loaded.ok()) {
        expect_true(*loaded.value == state, "state file round trip preserves data");
    }
}

void test_state_scheduler_debounce_and_max_interval() {
    using Manager = dmxwb::StatePersistenceManager;
    using namespace std::chrono_literals;

    Manager debounce_manager{"/tmp/unused-state.json"};
    const Manager::time_point t0{};
    debounce_manager.mark_dirty(t0);
    expect_true(debounce_manager.dirty(), "mark_dirty sets dirty flag");
    expect_true(
        debounce_manager.next_deadline() == std::optional<Manager::time_point>{t0 + 2s},
        "first dirty deadline is two seconds");
    expect_true(!debounce_manager.save_due(t0 + 1999ms), "state is not due before debounce deadline");

    debounce_manager.mark_dirty(t0 + 1500ms);
    expect_true(
        debounce_manager.next_deadline() == std::optional<Manager::time_point>{t0 + 3500ms},
        "new change restarts two-second debounce");

    Manager continuous_manager{"/tmp/unused-state.json"};
    continuous_manager.mark_dirty(t0);
    for (int second = 1; second <= 9; ++second) {
        continuous_manager.mark_dirty(t0 + std::chrono::seconds{second});
    }
    expect_true(
        continuous_manager.next_deadline() == std::optional<Manager::time_point>{t0 + 10s},
        "continuous changes are capped by ten-second dirty interval");
    expect_true(!continuous_manager.save_due(t0 + 9999ms), "max dirty save not due early");
    expect_true(continuous_manager.save_due(t0 + 10s), "max dirty save due at ten seconds");
}

void test_state_manager_due_save_and_forced_flush() {
    using Manager = dmxwb::StatePersistenceManager;
    using namespace std::chrono_literals;

    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for state manager");
        return;
    }

    const auto config = dmxwb::make_default_config();
    const auto state = dmxwb::make_default_state(config);
    const auto state_path = temp.file("state.json");
    Manager manager{state_path};
    const Manager::time_point t0{};

    manager.mark_dirty(t0);
    auto result = manager.save_if_due(state, config, t0 + 1s);
    expect_true(result.action == dmxwb::StateSaveAction::not_due, "save_if_due does not write before deadline");
    expect_true(manager.dirty(), "not-due state remains dirty");

    result = manager.save_if_due(state, config, t0 + 2s);
    expect_true(result.action == dmxwb::StateSaveAction::saved, "save_if_due writes at debounce deadline");
    expect_true(!manager.dirty(), "successful due save clears dirty flag");

    manager.mark_dirty(t0 + 3s);
    const std::filesystem::path blocking_temp{state_path + ".tmp"};
    std::error_code ec;
    expect_true(std::filesystem::create_directory(blocking_temp, ec), "state temp blocker created");
    result = manager.flush(state, config);
    expect_true(result.action == dmxwb::StateSaveAction::failed, "forced flush reports atomic write failure");
    expect_true(manager.dirty(), "failed forced flush preserves dirty flag");

    std::filesystem::remove(blocking_temp, ec);
    result = manager.flush(state, config);
    expect_true(result.action == dmxwb::StateSaveAction::saved, "forced flush saves dirty state after recovery");
    expect_true(!manager.dirty(), "successful forced flush clears dirty flag");
}

void test_state_save_failure_has_bounded_retry_and_recovers() {
    using Manager = dmxwb::StatePersistenceManager;
    using namespace std::chrono_literals;

    TempDirectory temp;
    if (!temp.valid()) {
        expect_true(false, "temporary directory created for state retry");
        return;
    }

    const auto config = dmxwb::make_default_config();
    const auto state = dmxwb::make_default_state(config);
    const auto state_path = temp.file("state.json");
    const std::filesystem::path blocking_temp{state_path + ".tmp"};
    std::error_code ec;
    expect_true(std::filesystem::create_directory(blocking_temp, ec),
                "state retry temp blocker created");

    Manager manager{state_path};
    const Manager::time_point t0{};
    manager.mark_dirty(t0);
    auto result = manager.save_if_due(state, config, t0 + 2s);
    expect_true(result.action == dmxwb::StateSaveAction::failed,
                "first due state write failure is reported");
    expect_true(manager.dirty() &&
                    manager.next_deadline() == std::optional<Manager::time_point>{t0 + 4s},
                "failed state write remains dirty with two-second retry deadline");

    result = manager.save_if_due(state, config, t0 + 2010ms);
    expect_true(result.action == dmxwb::StateSaveAction::not_due,
                "runtime tick immediately after failure does not retry write");
    result = manager.save_if_due(state, config, t0 + 3999ms);
    expect_true(result.action == dmxwb::StateSaveAction::not_due,
                "failed state write is not retried before bounded deadline");
    result = manager.save_if_due(state, config, t0 + 4s);
    expect_true(result.action == dmxwb::StateSaveAction::failed &&
                    manager.next_deadline() == std::optional<Manager::time_point>{t0 + 6s},
                "persistent failure schedules exactly one later retry window");

    std::filesystem::remove(blocking_temp, ec);
    result = manager.save_if_due(state, config, t0 + 6s);
    expect_true(result.action == dmxwb::StateSaveAction::saved,
                "pending state saves at retry deadline after storage recovery");
    expect_true(!manager.dirty() && !manager.next_deadline().has_value(),
                "successful retry clears dirty state and stops retries");
}

}  // namespace

int main() {
    test_atomic_write_success_and_failure_preserves_old_file();
    test_read_size_limit();
    test_config_transaction_and_revision_conflict();
    test_oversized_canonical_config_is_not_committed();
    test_corrupt_config_uses_defaults_without_overwrite();
    test_config_and_state_path_identity_is_rejected();
    test_corrupt_state_keeps_valid_config_and_uses_default_state();
    test_state_save_and_load_round_trip();
    test_state_scheduler_debounce_and_max_interval();
    test_state_manager_due_save_and_forced_flush();
    test_state_save_failure_has_bounded_retry_and_recovers();

    if (failures != 0) {
        std::cerr << failures << " DEV-006B persistence storage test(s) failed\n";
        return 1;
    }
    std::cout << "DEV-006B persistence storage tests PASS\n";
    return 0;
}
