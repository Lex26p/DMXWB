#pragma once

#include "dmxwb/persistence_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dmxwb {

struct PersistenceStartupStatus final {
    PersistenceFileError config_error;
    PersistenceFileError state_error;
    PersistenceError restore_error;

    [[nodiscard]] bool ok() const noexcept {
        return !config_error && !state_error && !restore_error;
    }
};

class PersistenceRuntime final {
public:
    using time_point = StatePersistenceManager::time_point;

    explicit PersistenceRuntime(
        std::string config_path = std::string{kDefaultConfigPath},
        std::string state_path = std::string{kDefaultStatePath});

    [[nodiscard]] const AppConfig& config() const noexcept;
    [[nodiscard]] PersistedSource source() const noexcept;
    [[nodiscard]] const FixtureCollection& fixtures() const noexcept;
    [[nodiscard]] Fixture* fixture_at(std::size_t zero_based_index) noexcept;
    [[nodiscard]] const Fixture* fixture_at(std::size_t zero_based_index) const noexcept;
    [[nodiscard]] const PersistenceStartupStatus& startup_status() const noexcept;

    [[nodiscard]] AppState capture_state() const;

    void set_source(PersistedSource source, time_point now) noexcept;
    void mark_fixture_state_changed(time_point now) noexcept;

    [[nodiscard]] bool state_dirty() const noexcept;
    [[nodiscard]] std::optional<time_point> next_state_save_deadline() const noexcept;
    [[nodiscard]] StateSaveResult save_state_if_due(time_point now);
    [[nodiscard]] StateSaveResult flush_state();

    [[nodiscard]] PersistenceFileResult<AppConfig> apply_config_transaction(
        std::uint64_t expected_revision,
        const AppConfig& proposed_config,
        time_point now);

    [[nodiscard]] std::string_view config_path() const noexcept;
    [[nodiscard]] std::string_view state_path() const noexcept;

private:
    [[nodiscard]] AppState reconcile_state_for_config(const AppConfig& proposed_config) const;
    void restore_startup_model(LoadedPersistenceFiles loaded);

    std::string config_path_;
    std::string state_path_;
    AppConfig config_{};
    PersistedSource source_{PersistedSource::mqtt};
    FixtureCollection fixtures_{};
    PersistenceStartupStatus startup_status_{};
    StatePersistenceManager state_manager_;
};

}  // namespace dmxwb
