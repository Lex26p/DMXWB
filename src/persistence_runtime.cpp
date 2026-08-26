#include "dmxwb/persistence_runtime.hpp"

#include <limits>
#include <unordered_map>
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

}  // namespace

PersistenceRuntime::PersistenceRuntime(std::string config_path, std::string state_path)
    : config_path_(std::move(config_path)),
      state_path_(std::move(state_path)),
      state_manager_(state_path_) {
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

AppState PersistenceRuntime::capture_state() const {
    AppState state;
    state.source = source_;
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

void PersistenceRuntime::set_source(PersistedSource source, time_point now) noexcept {
    if (source_ == source) {
        return;
    }
    source_ = source;
    state_manager_.mark_dirty(now);
}

void PersistenceRuntime::mark_fixture_state_changed(time_point now) noexcept {
    state_manager_.mark_dirty(now);
}

bool PersistenceRuntime::state_dirty() const noexcept {
    return state_manager_.dirty();
}

std::optional<PersistenceRuntime::time_point> PersistenceRuntime::next_state_save_deadline() const noexcept {
    return state_manager_.next_deadline();
}

StateSaveResult PersistenceRuntime::save_state_if_due(time_point now) {
    return state_manager_.save_if_due(capture_state(), config_, now);
}

StateSaveResult PersistenceRuntime::flush_state() {
    return state_manager_.flush(capture_state(), config_);
}

PersistenceFileResult<AppConfig> PersistenceRuntime::apply_config_transaction(
    std::uint64_t expected_revision,
    const AppConfig& proposed_config,
    time_point now) {
    const auto revision_error = validate_expected_revision(config_.revision, expected_revision);
    if (revision_error) {
        return {{}, map_runtime_model_error(revision_error)};
    }

    const auto config_error = validate_config(proposed_config);
    if (config_error) {
        return {{}, map_runtime_model_error(config_error)};
    }

    if (config_.revision == std::numeric_limits<std::uint64_t>::max()) {
        return {{}, make_runtime_file_error(
            PersistenceFileErrorCode::validation,
            "configuration revision counter exhausted")};
    }

    AppState next_state = reconcile_state_for_config(proposed_config);
    FixtureCollection next_fixtures;
    const auto restore_error = restore_fixture_collection(proposed_config, next_state, next_fixtures);
    if (restore_error) {
        return {{}, map_runtime_model_error(restore_error)};
    }

    auto committed = commit_config_file_atomic(
        config_path_,
        config_.revision,
        expected_revision,
        proposed_config);
    if (!committed.ok()) {
        return committed;
    }

    config_ = *committed.value;
    fixtures_ = std::move(next_fixtures);
    state_manager_.mark_dirty(now);
    return committed;
}

std::string_view PersistenceRuntime::config_path() const noexcept {
    return config_path_;
}

std::string_view PersistenceRuntime::state_path() const noexcept {
    return state_path_;
}

AppState PersistenceRuntime::reconcile_state_for_config(const AppConfig& proposed_config) const {
    const auto current_state = capture_state();
    std::unordered_map<Fixture::Id, const FixtureRuntimeState*> current_by_id;
    current_by_id.reserve(current_state.fixtures.size());
    for (const auto& runtime : current_state.fixtures) {
        current_by_id.emplace(runtime.id, &runtime);
    }

    AppState next_state;
    next_state.source = source_;
    next_state.fixtures.reserve(proposed_config.fixtures.size());
    for (const auto& configured : proposed_config.fixtures) {
        const auto found = current_by_id.find(configured.id);
        if (found != current_by_id.end()) {
            next_state.fixtures.push_back(*found->second);
            continue;
        }

        FixtureRuntimeState fresh;
        fresh.id = configured.id;
        next_state.fixtures.push_back(fresh);
    }
    return next_state;
}

void PersistenceRuntime::restore_startup_model(LoadedPersistenceFiles loaded) {
    startup_status_.config_error = std::move(loaded.config_error);
    startup_status_.state_error = std::move(loaded.state_error);

    FixtureCollection restored;
    auto restore_error = restore_fixture_collection(loaded.config, loaded.state, restored);
    if (!restore_error) {
        config_ = std::move(loaded.config);
        source_ = loaded.state.source;
        fixtures_ = std::move(restored);
        return;
    }

    startup_status_.restore_error = restore_error;
    auto fallback_state = make_default_state(loaded.config);
    restore_error = restore_fixture_collection(loaded.config, fallback_state, restored);
    if (!restore_error) {
        config_ = std::move(loaded.config);
        source_ = fallback_state.source;
        fixtures_ = std::move(restored);
        return;
    }

    config_ = make_default_config();
    auto default_state = make_default_state(config_);
    FixtureCollection defaults;
    (void)restore_fixture_collection(config_, default_state, defaults);
    source_ = default_state.source;
    fixtures_ = std::move(defaults);
}

}  // namespace dmxwb
