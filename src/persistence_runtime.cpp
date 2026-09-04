#include "dmxwb/persistence_runtime.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dmxwb {
namespace {

[[nodiscard]] PersistenceFileError make_runtime_file_error(
    PersistenceFileErrorCode code,
    std::string message) {
    return PersistenceFileError{code, std::move(message)};
}

[[nodiscard]] PersistenceFileError map_runtime_model_error(const PersistenceError& error) {
    if (!error) {
        return {};
    }
    if (error.code == PersistenceErrorCode::revision_conflict) {
        return make_runtime_file_error(PersistenceFileErrorCode::revision_conflict, error.message);
    }
    return make_runtime_file_error(PersistenceFileErrorCode::validation, error.message);
}

[[nodiscard]] AppConfig normalize_removed_fixture_memberships(
    const AppConfig& current,
    const AppConfig& proposed) {
    AppConfig normalized = proposed;

    std::unordered_set<Fixture::Id> proposed_ids;
    proposed_ids.reserve(proposed.fixtures.size());
    for (const auto& fixture : proposed.fixtures) {
        proposed_ids.insert(fixture.id);
    }

    std::unordered_set<Fixture::Id> removed_ids;
    removed_ids.reserve(current.fixtures.size());
    for (const auto& fixture : current.fixtures) {
        if (proposed_ids.find(fixture.id) == proposed_ids.end()) {
            removed_ids.insert(fixture.id);
        }
    }
    if (removed_ids.empty()) {
        return normalized;
    }

    for (auto& group : normalized.groups) {
        std::erase_if(group.members, [&removed_ids](Fixture::Id member_id) {
            return removed_ids.find(member_id) != removed_ids.end();
        });
    }
    return normalized;
}

template <typename Record>
[[nodiscard]] std::vector<std::uint64_t> removed_ids(
    const std::vector<Record>& current,
    const std::vector<Record>& proposed) {
    std::unordered_set<std::uint64_t> proposed_ids;
    proposed_ids.reserve(proposed.size());
    for (const auto& record : proposed) proposed_ids.insert(record.id);

    std::vector<std::uint64_t> removed;
    removed.reserve(current.size());
    for (const auto& record : current) {
        if (!proposed_ids.contains(record.id)) removed.push_back(record.id);
    }
    return removed;
}

void merge_cleanup_ids(
    std::vector<std::uint64_t>& pending,
    const std::vector<std::uint64_t>& added) {
    pending.insert(pending.end(), added.begin(), added.end());
    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
}

void remove_delivered_ids(
    std::vector<std::uint64_t>& pending,
    const std::vector<std::uint64_t>& delivered) {
    if (pending.empty() || delivered.empty()) return;
    const std::unordered_set<std::uint64_t> delivered_set{
        delivered.begin(), delivered.end()};
    std::erase_if(pending, [&delivered_set](std::uint64_t id) {
        return delivered_set.contains(id);
    });
}

}  // namespace

PersistenceRuntime::PersistenceRuntime(std::string config_path, std::string state_path)
    : config_path_(std::move(config_path)),
      state_path_(std::move(state_path)),
      state_manager_(state_path_) {
    const auto path_error = validate_persistence_paths(config_path_, state_path_);
    if (path_error) {
        throw std::invalid_argument{path_error.message};
    }
    restore_startup_model(load_persistence_files(config_path_, state_path_));
}

const AppConfig& PersistenceRuntime::config() const noexcept {
    return config_;
}

PersistedSource PersistenceRuntime::source() const noexcept {
    return source_;
}

const FixtureCollection& PersistenceRuntime::fixtures() const noexcept {
    return fixtures_;
}

Fixture* PersistenceRuntime::fixture_at(std::size_t zero_based_index) noexcept {
    return fixtures_.fixture_at(zero_based_index);
}

const Fixture* PersistenceRuntime::fixture_at(std::size_t zero_based_index) const noexcept {
    return fixtures_.fixture_at(zero_based_index);
}

const PersistenceStartupStatus& PersistenceRuntime::startup_status() const noexcept {
    return startup_status_;
}

const PersistenceStartupStatus& PersistenceRuntime::operational_status() const noexcept {
    return startup_status_;
}

AppState PersistenceRuntime::capture_state() const {
    AppState state;
    state.source = source_;
    state.mqtt_retained_cleanup = mqtt_retained_cleanup_;
    state.scene_create_idempotency = scene_create_idempotency_;
    state.fixtures.reserve(fixtures_.fixture_count());

    for (std::size_t index = 0; index < fixtures_.fixture_count(); ++index) {
        const auto* fixture = fixtures_.fixture_at(index);
        if (fixture == nullptr) {
            continue;
        }

        FixtureRuntimeState runtime;
        runtime.id = fixture->id();
        runtime.requested_power = fixture->requested_power();
        runtime.rgbw = fixture->saved_rgbw();
        runtime.brightness = fixture->brightness();
        runtime.temperature = fixture->temperature();
        state.fixtures.push_back(runtime);
    }
    return state;
}

const MqttRetainedCleanup& PersistenceRuntime::pending_mqtt_retained_cleanup() const noexcept {
    return mqtt_retained_cleanup_;
}

const SceneCreateIdempotencyRecord* PersistenceRuntime::find_scene_create_idempotency(
    std::string_view request_id) const noexcept {
    const auto found = std::find_if(
        scene_create_idempotency_.begin(),
        scene_create_idempotency_.end(),
        [request_id](const SceneCreateIdempotencyRecord& record) {
            return record.request_id == request_id;
        });
    return found == scene_create_idempotency_.end() ? nullptr : &*found;
}

void PersistenceRuntime::acknowledge_mqtt_retained_cleanup(
    const MqttRetainedCleanup& delivered,
    time_point now) noexcept {
    const auto previous = mqtt_retained_cleanup_;
    remove_delivered_ids(
        mqtt_retained_cleanup_.fixture_ids,
        delivered.fixture_ids);
    remove_delivered_ids(
        mqtt_retained_cleanup_.group_ids,
        delivered.group_ids);
    remove_delivered_ids(
        mqtt_retained_cleanup_.scene_ids,
        delivered.scene_ids);
    if (state_writes_allowed_ && mqtt_retained_cleanup_ != previous) {
        state_manager_.mark_dirty(now);
    }
}

void PersistenceRuntime::set_source(PersistedSource source, time_point now) noexcept {
    if (source_ == source) {
        return;
    }
    source_ = source;
    if (state_writes_allowed_) {
        state_manager_.mark_dirty(now);
    }
}

void PersistenceRuntime::mark_fixture_state_changed(time_point now) noexcept {
    if (state_writes_allowed_) {
        state_manager_.mark_dirty(now);
    }
}

bool PersistenceRuntime::state_dirty() const noexcept {
    return state_writes_allowed_ && state_manager_.dirty();
}

std::optional<PersistenceRuntime::time_point> PersistenceRuntime::next_state_save_deadline() const noexcept {
    if (!state_writes_allowed_) {
        return std::nullopt;
    }
    return state_manager_.next_deadline();
}

StateSaveResult PersistenceRuntime::save_state_if_due(time_point now) {
    if (!state_writes_allowed_) {
        return {StateSaveAction::not_dirty, {}};
    }
    if (!state_manager_.dirty()) {
        return {StateSaveAction::not_dirty, {}};
    }
    if (!state_manager_.save_due(now)) {
        return {StateSaveAction::not_due, {}};
    }

    const auto state = capture_state();
    if (pending_state_revision_.has_value()) {
        const auto prepare_error = prepare_config_state_file(state_path_, state, config_);
        if (prepare_error) {
            state_manager_.defer_after_failure(now);
            record_state_failure(prepare_error);
            return {StateSaveAction::failed, prepare_error};
        }
    }

    auto result = state_manager_.save_if_due(state, config_, now);
    if (result.action == StateSaveAction::failed) {
        record_state_failure(result.error);
        return result;
    }
    if (result.action == StateSaveAction::saved && pending_state_revision_.has_value()) {
        auto discard_error = discard_config_state_file(
            state_path_,
            *pending_state_revision_);
        if (discard_error) {
            state_manager_.mark_dirty(now);
            state_manager_.defer_after_failure(now);
            record_state_failure(discard_error);
            return {StateSaveAction::failed, std::move(discard_error)};
        }
        pending_state_revision_.reset();
    }
    if (result.action == StateSaveAction::saved) {
        record_state_success();
    }
    return result;
}

StateSaveResult PersistenceRuntime::flush_state() {
    if (!state_writes_allowed_) {
        return {StateSaveAction::not_dirty, {}};
    }
    if (!state_manager_.dirty()) {
        return {StateSaveAction::not_dirty, {}};
    }

    const auto state = capture_state();
    if (pending_state_revision_.has_value()) {
        const auto prepare_error = prepare_config_state_file(state_path_, state, config_);
        if (prepare_error) {
            record_state_failure(prepare_error);
            return {StateSaveAction::failed, prepare_error};
        }
    }

    auto result = state_manager_.flush(state, config_);
    if (result.action == StateSaveAction::failed) {
        record_state_failure(result.error);
        return result;
    }
    if (result.action == StateSaveAction::saved && pending_state_revision_.has_value()) {
        auto discard_error = discard_config_state_file(
            state_path_,
            *pending_state_revision_);
        if (discard_error) {
            state_manager_.mark_dirty(StatePersistenceManager::clock::now());
            record_state_failure(discard_error);
            return {StateSaveAction::failed, std::move(discard_error)};
        }
        pending_state_revision_.reset();
    }
    if (result.action == StateSaveAction::saved) {
        record_state_success();
    }
    return result;
}

PersistenceFileResult<AppConfig> PersistenceRuntime::apply_config_transaction(
    std::uint64_t expected_revision,
    const AppConfig& proposed_config,
    time_point now,
    std::optional<SceneCreateIdempotencyRecord> scene_create_record) {
    const auto revision_error = validate_expected_revision(config_.revision, expected_revision);
    if (revision_error) {
        return {{}, map_runtime_model_error(revision_error)};
    }

    const AppConfig normalized = normalize_removed_fixture_memberships(config_, proposed_config);
    const auto config_error = validate_config_transition(config_, normalized);
    if (config_error) {
        return {{}, map_runtime_model_error(config_error)};
    }

    if (config_.revision == std::numeric_limits<std::uint64_t>::max()) {
        return {{}, make_runtime_file_error(
            PersistenceFileErrorCode::validation,
            "configuration revision counter exhausted")};
    }

    auto current_state = capture_state();
    if (!state_writes_allowed_) {
        auto stored = read_persistence_text_file(state_path_);
        if (stored.ok()) {
            auto parsed = parse_state_json(*stored.value);
            if (!parsed.ok()) {
                return {{}, map_runtime_model_error(parsed.error)};
            }
            current_state = std::move(*parsed.value);
        } else if (stored.error.code != PersistenceFileErrorCode::not_found) {
            return {{}, std::move(stored.error)};
        }
    }

    auto reconciled = dmxwb::reconcile_state_for_config(current_state, normalized);
    if (!reconciled.ok()) {
        return {{}, map_runtime_model_error(reconciled.error)};
    }
    AppState next_state = std::move(*reconciled.value);
    merge_cleanup_ids(
        next_state.mqtt_retained_cleanup.fixture_ids,
        removed_ids(config_.fixtures, normalized.fixtures));
    merge_cleanup_ids(
        next_state.mqtt_retained_cleanup.group_ids,
        removed_ids(config_.groups, normalized.groups));
    merge_cleanup_ids(
        next_state.mqtt_retained_cleanup.scene_ids,
        removed_ids(config_.scenes, normalized.scenes));

    AppConfig committed_config = normalized;
    committed_config.revision = config_.revision + 1U;
    const auto committed_config_error = validate_config(committed_config);
    if (committed_config_error) {
        return {{}, map_runtime_model_error(committed_config_error)};
    }
    if (serialize_config_json(committed_config).size() > kPersistenceMaxFileBytes) {
        return {{}, make_runtime_file_error(
            PersistenceFileErrorCode::too_large,
            "canonical configuration exceeds persistence file size limit")};
    }

    if (scene_create_record.has_value()) {
        if (scene_create_record->revision != committed_config.revision) {
            return {{}, make_runtime_file_error(
                PersistenceFileErrorCode::validation,
                "Scene Create idempotency revision does not match config transaction")};
        }
        next_state.scene_create_idempotency.push_back(
            std::move(*scene_create_record));
        if (next_state.scene_create_idempotency.size() >
            kSceneCreateIdempotencyCapacity) {
            const auto excess = next_state.scene_create_idempotency.size() -
                kSceneCreateIdempotencyCapacity;
            next_state.scene_create_idempotency.erase(
                next_state.scene_create_idempotency.begin(),
                next_state.scene_create_idempotency.begin() +
                    static_cast<std::ptrdiff_t>(excess));
        }
    }

    const auto next_state_error = validate_state(next_state, committed_config);
    if (next_state_error) {
        return {{}, map_runtime_model_error(next_state_error)};
    }
    FixtureCollection next_fixtures;
    const auto restore_error = restore_fixture_collection(
        committed_config,
        next_state,
        next_fixtures);
    if (restore_error) {
        return {{}, map_runtime_model_error(restore_error)};
    }

    const auto prepare_error = prepare_config_state_file(
        state_path_,
        next_state,
        committed_config);
    if (prepare_error) {
        record_state_failure(prepare_error);
        return {{}, prepare_error};
    }

    auto committed = commit_config_file_atomic(
        config_path_,
        config_.revision,
        expected_revision,
        normalized);
    if (!committed.ok()) {
        static_cast<void>(discard_config_state_file(
            state_path_,
            committed_config.revision));
        if (committed.error.code == PersistenceFileErrorCode::io) {
            record_config_failure(committed.error);
        }
        return committed;
    }

    // The exact matching state is durable before the atomic config rename commit
    // point. A crash after that point restarts from the revision-qualified pending
    // state, never from an incompatible config/state pair.
    const auto finalize_error = finalize_config_state_file(
        state_path_,
        committed.value->revision);
    if (finalize_error) {
        pending_state_revision_ = committed.value->revision;
        state_manager_.mark_dirty(now);
        record_config_success();
        record_state_failure(finalize_error);
    } else {
        pending_state_revision_.reset();
        record_config_success();
    }

    config_ = *committed.value;
    fixtures_ = std::move(next_fixtures);
    mqtt_retained_cleanup_ = std::move(next_state.mqtt_retained_cleanup);
    scene_create_idempotency_ = std::move(next_state.scene_create_idempotency);
    state_writes_allowed_ = true;
    return committed;
}

std::string_view PersistenceRuntime::config_path() const noexcept {
    return config_path_;
}

std::string_view PersistenceRuntime::state_path() const noexcept {
    return state_path_;
}

void PersistenceRuntime::restore_startup_model(LoadedPersistenceFiles loaded) {
    state_writes_allowed_ = loaded.state_writes_allowed;
    startup_status_.config_error = std::move(loaded.config_error);
    startup_status_.state_error = std::move(loaded.state_error);
    startup_status_.fallback_active =
        static_cast<bool>(startup_status_.config_error) ||
        static_cast<bool>(startup_status_.state_error);

    FixtureCollection restored;
    auto restore_error = restore_fixture_collection(loaded.config, loaded.state, restored);
    if (!restore_error) {
        config_ = std::move(loaded.config);
        source_ = loaded.state.source;
        fixtures_ = std::move(restored);
        mqtt_retained_cleanup_ = std::move(loaded.state.mqtt_retained_cleanup);
        scene_create_idempotency_ = std::move(loaded.state.scene_create_idempotency);
        if (state_writes_allowed_ && (loaded.state_reconciled || startup_status_.state_error)) {
            state_manager_.mark_dirty(StatePersistenceManager::clock::now());
        }
        pending_state_revision_ = loaded.pending_state_revision;
        return;
    }

    startup_status_.restore_error = restore_error;
    startup_status_.fallback_active = true;
    auto fallback_state = make_default_state(loaded.config);
    restore_error = restore_fixture_collection(loaded.config, fallback_state, restored);
    if (!restore_error) {
        config_ = std::move(loaded.config);
        source_ = fallback_state.source;
        fixtures_ = std::move(restored);
        mqtt_retained_cleanup_ = std::move(fallback_state.mqtt_retained_cleanup);
        scene_create_idempotency_ = std::move(fallback_state.scene_create_idempotency);
        if (state_writes_allowed_) {
            state_manager_.mark_dirty(StatePersistenceManager::clock::now());
        }
        return;
    }

    config_ = make_default_config();
    auto default_state = make_default_state(config_);
    FixtureCollection defaults;
    (void)restore_fixture_collection(config_, default_state, defaults);
    source_ = default_state.source;
    fixtures_ = std::move(defaults);
    mqtt_retained_cleanup_ = std::move(default_state.mqtt_retained_cleanup);
    scene_create_idempotency_ = std::move(default_state.scene_create_idempotency);
}

void PersistenceRuntime::record_state_failure(PersistenceFileError error) noexcept {
    startup_status_.state_error = std::move(error);
}

void PersistenceRuntime::record_state_success() noexcept {
    startup_status_.state_error = {};
    startup_status_.restore_error = {};
    if (!startup_status_.config_error) {
        startup_status_.fallback_active = false;
    }
}

void PersistenceRuntime::record_config_failure(PersistenceFileError error) noexcept {
    startup_status_.config_error = std::move(error);
}

void PersistenceRuntime::record_config_success() noexcept {
    startup_status_.config_error = {};
    startup_status_.state_error = {};
    startup_status_.restore_error = {};
    startup_status_.fallback_active = false;
}

}  // namespace dmxwb
