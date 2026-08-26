#pragma once

#include "dmxwb/persistence.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dmxwb {

inline constexpr std::string_view kDefaultConfigPath = "/etc/dmxwb/config.json";
inline constexpr std::string_view kDefaultStatePath = "/var/lib/dmxwb/state.json";
inline constexpr std::size_t kPersistenceMaxFileBytes = 4U * 1024U * 1024U;

enum class PersistenceFileErrorCode {
    none,
    not_found,
    io,
    too_large,
    parse,
    validation,
    revision_conflict,
};

struct PersistenceFileError final {
    PersistenceFileErrorCode code{PersistenceFileErrorCode::none};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != PersistenceFileErrorCode::none;
    }
};

template <typename T>
struct PersistenceFileResult final {
    std::optional<T> value;
    PersistenceFileError error;

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !error;
    }
};

struct LoadedPersistenceFiles final {
    AppConfig config;
    AppState state;
    PersistenceFileError config_error;
    PersistenceFileError state_error;
};

[[nodiscard]] PersistenceFileResult<std::string> read_persistence_text_file(
    std::string_view path,
    std::size_t max_bytes = kPersistenceMaxFileBytes);

[[nodiscard]] PersistenceFileError write_persistence_text_file_atomic(
    std::string_view path,
    std::string_view content);

[[nodiscard]] PersistenceFileResult<AppConfig> load_config_file(std::string_view path);
[[nodiscard]] PersistenceFileResult<AppState> load_state_file(
    std::string_view path,
    const AppConfig& config);

[[nodiscard]] LoadedPersistenceFiles load_persistence_files(
    std::string_view config_path = kDefaultConfigPath,
    std::string_view state_path = kDefaultStatePath);

[[nodiscard]] PersistenceFileResult<AppConfig> commit_config_file_atomic(
    std::string_view path,
    std::uint64_t current_revision,
    std::uint64_t expected_revision,
    const AppConfig& proposed_config);

[[nodiscard]] PersistenceFileError save_state_file_atomic(
    std::string_view path,
    const AppState& state,
    const AppConfig& config);

enum class StateSaveAction {
    not_dirty,
    not_due,
    saved,
    failed,
};

struct StateSaveResult final {
    StateSaveAction action{StateSaveAction::not_dirty};
    PersistenceFileError error;

    [[nodiscard]] bool ok() const noexcept {
        return action != StateSaveAction::failed;
    }
};

class StatePersistenceManager final {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    static constexpr auto kDebounceDelay = std::chrono::seconds{2};
    static constexpr auto kMaxDirtyInterval = std::chrono::seconds{10};

    explicit StatePersistenceManager(std::string state_path);

    void mark_dirty(time_point now) noexcept;

    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool save_due(time_point now) const noexcept;
    [[nodiscard]] std::optional<time_point> next_deadline() const noexcept;
    [[nodiscard]] std::string_view state_path() const noexcept;

    [[nodiscard]] StateSaveResult save_if_due(
        const AppState& state,
        const AppConfig& config,
        time_point now);

    [[nodiscard]] StateSaveResult flush(
        const AppState& state,
        const AppConfig& config);

private:
    void mark_saved() noexcept;

    std::string state_path_;
    bool dirty_{false};
    time_point first_dirty_at_{};
    time_point last_change_at_{};
};

}  // namespace dmxwb
