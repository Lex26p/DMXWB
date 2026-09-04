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
    bool fallback_active{false};

    [[nodiscard]] bool ok() const noexcept {
        return !config_error && !state_error && !restore_error;
    }

    [[nodiscard]] std::string_view last_error() const noexcept {
        if (config_error) {
            return config_error.message;
        }
        if (state_error) {
            return state_error.message;
        }
        return restore_error.message;
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
    // Historical name kept for callers; the returned object now tracks current
    // persistence health and clears after a successful corrective write.
    [[nodiscard]] const PersistenceStartupStatus& startup_status() const noexcept;
    [[nodiscard]] const PersistenceStartupStatus& operational_status() const noexcept;

    [[nodiscard]] AppState capture_state() const;
    [[nodiscard]] const MqttRetainedCleanup& pending_mqtt_retained_cleanup() const noexcept;
    [[nodiscard]] const SceneCreateIdempotencyRecord* find_scene_create_idempotency(
        std::string_view request_id) const noexcept;
    void acknowledge_mqtt_retained_cleanup(
        const MqttRetainedCleanup& delivered,
        time_point now) noexcept;

    void set_source(PersistedSource source, time_point now) noexcept;
    void mark_fixture_state_changed(time_point now) noexcept;

    [[nodiscard]] bool state_dirty() const noexcept;
    [[nodiscard]] std::optional<time_point> next_state_save_deadline() const noexcept;
    [[nodiscard]] StateSaveResult save_state_if_due(time_point now);
    [[nodiscard]] StateSaveResult flush_state();

    [[nodiscard]] PersistenceFileResult<AppConfig> apply_config_transaction(
        std::uint64_t expected_revision,
        const AppConfig& proposed_config,
        time_point now,
        std::optional<SceneCreateIdempotencyRecord> scene_create_record = std::nullopt);

    [[nodiscard]] std::string_view config_path() const noexcept;
    [[nodiscard]] std::string_view state_path() const noexcept;

private:
    void restore_startup_model(LoadedPersistenceFiles loaded);
    void record_state_failure(PersistenceFileError error) noexcept;
    void record_state_success() noexcept;
    void record_config_failure(PersistenceFileError error) noexcept;
    void record_config_success() noexcept;

    std::string config_path_;
    std::string state_path_;
    AppConfig config_{};
    PersistedSource source_{PersistedSource::mqtt};
    FixtureCollection fixtures_{};
    MqttRetainedCleanup mqtt_retained_cleanup_{};
    std::vector<SceneCreateIdempotencyRecord> scene_create_idempotency_;
    PersistenceStartupStatus startup_status_{};
    StatePersistenceManager state_manager_;
    std::optional<std::uint64_t> pending_state_revision_;
    bool state_writes_allowed_{true};
};

}  // namespace dmxwb
