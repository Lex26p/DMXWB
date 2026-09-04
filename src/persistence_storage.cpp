#include "dmxwb/persistence_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace dmxwb {
namespace {

[[nodiscard]] PersistenceFileError make_file_error(
    PersistenceFileErrorCode code,
    std::string message) {
    return PersistenceFileError{code, std::move(message)};
}

[[nodiscard]] std::string errno_message(std::string_view operation, std::string_view path, int error_number) {
    std::string message{operation};
    message += " '";
    message += path;
    message += "': ";
    message += std::strerror(error_number);
    return message;
}

[[nodiscard]] PersistenceFileError map_model_error(const PersistenceError& error) {
    if (!error) {
        return {};
    }

    switch (error.code) {
        case PersistenceErrorCode::revision_conflict:
            return make_file_error(PersistenceFileErrorCode::revision_conflict, error.message);
        case PersistenceErrorCode::json_syntax:
            return make_file_error(PersistenceFileErrorCode::parse, error.message);
        case PersistenceErrorCode::schema:
        case PersistenceErrorCode::version:
        case PersistenceErrorCode::validation:
            return make_file_error(PersistenceFileErrorCode::validation, error.message);
        case PersistenceErrorCode::none:
            break;
    }
    return make_file_error(PersistenceFileErrorCode::validation, error.message);
}

[[nodiscard]] bool close_without_retry(int file_descriptor) noexcept {
    return ::close(file_descriptor) == 0;
}

[[nodiscard]] bool write_all(int file_descriptor, std::string_view content, int& error_number) noexcept {
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto remaining = content.size() - offset;
        const auto max_chunk = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        const auto chunk = std::min(remaining, max_chunk);
        const auto written = ::write(file_descriptor, content.data() + offset, chunk);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_number = errno;
            return false;
        }
        if (written == 0) {
            error_number = EIO;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] std::string make_temp_path(std::string_view path) {
    std::string temp{path};
    temp += ".tmp";
    return temp;
}

[[nodiscard]] std::string make_config_state_path(
    std::string_view state_path,
    std::uint64_t revision) {
    std::string path{state_path};
    path += ".config-";
    path += std::to_string(revision);
    path += ".pending";
    return path;
}

void remove_temp_file(std::string_view path) noexcept {
    const std::string owned_path{path};
    (void)::unlink(owned_path.c_str());
}

}  // namespace

PersistenceFileResult<std::string> read_persistence_text_file(
    std::string_view path,
    std::size_t max_bytes) {
    if (path.empty()) {
        return {{}, make_file_error(PersistenceFileErrorCode::io, "persistence path is empty")};
    }

    const std::string owned_path{path};
    const int file_descriptor = ::open(owned_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_descriptor < 0) {
        const int error_number = errno;
        const auto code = error_number == ENOENT
            ? PersistenceFileErrorCode::not_found
            : PersistenceFileErrorCode::io;
        return {{}, make_file_error(code, errno_message("open", path, error_number))};
    }

    struct stat info {};
    if (::fstat(file_descriptor, &info) != 0) {
        const int error_number = errno;
        (void)close_without_retry(file_descriptor);
        return {{}, make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("fstat", path, error_number))};
    }

    if (S_ISREG(info.st_mode) != 0 && info.st_size > 0) {
        const auto unsigned_size = static_cast<std::uintmax_t>(info.st_size);
        if (unsigned_size > static_cast<std::uintmax_t>(max_bytes)) {
            (void)close_without_retry(file_descriptor);
            return {{}, make_file_error(PersistenceFileErrorCode::too_large, "persistence file exceeds size limit")};
        }
    }

    std::string content;
    if (S_ISREG(info.st_mode) != 0 && info.st_size > 0) {
        content.reserve(static_cast<std::size_t>(info.st_size));
    }

    char buffer[8192];
    while (true) {
        const auto read_count = ::read(file_descriptor, buffer, sizeof(buffer));
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error_number = errno;
            (void)close_without_retry(file_descriptor);
            return {{}, make_file_error(
                PersistenceFileErrorCode::io,
                errno_message("read", path, error_number))};
        }
        if (read_count == 0) {
            break;
        }

        const auto count = static_cast<std::size_t>(read_count);
        if (count > max_bytes - std::min(content.size(), max_bytes)) {
            (void)close_without_retry(file_descriptor);
            return {{}, make_file_error(PersistenceFileErrorCode::too_large, "persistence file exceeds size limit")};
        }
        content.append(buffer, count);
    }

    if (!close_without_retry(file_descriptor)) {
        const int error_number = errno;
        return {{}, make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("close", path, error_number))};
    }

    return {std::move(content), {}};
}

PersistenceFileError write_persistence_text_file_atomic(
    std::string_view path,
    std::string_view content) {
    if (path.empty()) {
        return make_file_error(PersistenceFileErrorCode::io, "persistence path is empty");
    }

    const std::string target_path{path};
    const std::string temp_path = make_temp_path(path);
    const int file_descriptor = ::open(
        temp_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        static_cast<mode_t>(0644));
    if (file_descriptor < 0) {
        const int error_number = errno;
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("open temporary file", temp_path, error_number));
    }

    int write_error = 0;
    if (!write_all(file_descriptor, content, write_error)) {
        (void)close_without_retry(file_descriptor);
        remove_temp_file(temp_path);
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("write temporary file", temp_path, write_error));
    }

    if (::fsync(file_descriptor) != 0) {
        const int error_number = errno;
        (void)close_without_retry(file_descriptor);
        remove_temp_file(temp_path);
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("fsync temporary file", temp_path, error_number));
    }

    if (!close_without_retry(file_descriptor)) {
        const int error_number = errno;
        remove_temp_file(temp_path);
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("close temporary file", temp_path, error_number));
    }

    if (::rename(temp_path.c_str(), target_path.c_str()) != 0) {
        const int error_number = errno;
        remove_temp_file(temp_path);
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("rename temporary file", path, error_number));
    }

    return {};
}

PersistenceFileResult<AppConfig> load_config_file(std::string_view path) {
    auto file = read_persistence_text_file(path);
    if (!file.ok()) {
        return {{}, std::move(file.error)};
    }

    auto parsed = parse_config_json(*file.value);
    if (!parsed.ok()) {
        return {{}, map_model_error(parsed.error)};
    }

    const auto validation = validate_config(*parsed.value);
    if (validation) {
        return {{}, map_model_error(validation)};
    }
    return {std::move(parsed.value), {}};
}

PersistenceFileResult<AppState> load_state_file(
    std::string_view path,
    const AppConfig& config) {
    auto file = read_persistence_text_file(path);
    if (!file.ok()) {
        return {{}, std::move(file.error)};
    }

    auto parsed = parse_state_json(*file.value);
    if (!parsed.ok()) {
        return {{}, map_model_error(parsed.error)};
    }

    const auto validation = validate_state(*parsed.value, config);
    if (validation) {
        return {{}, map_model_error(validation)};
    }
    return {std::move(parsed.value), {}};
}

PersistenceFileError validate_persistence_paths(
    std::string_view config_path,
    std::string_view state_path) {
    if (config_path.empty() || state_path.empty()) {
        return make_file_error(
            PersistenceFileErrorCode::validation,
            "config and state paths must not be empty");
    }

    std::error_code config_error;
    std::error_code state_error;
    const auto normalized_config = std::filesystem::absolute(
        std::filesystem::path{std::string{config_path}}, config_error).lexically_normal();
    const auto normalized_state = std::filesystem::absolute(
        std::filesystem::path{std::string{state_path}}, state_error).lexically_normal();
    if (!config_error && !state_error && normalized_config == normalized_state) {
        return make_file_error(
            PersistenceFileErrorCode::validation,
            "config and state paths must refer to different files");
    }

    config_error.clear();
    state_error.clear();
    const auto canonical_config = std::filesystem::weakly_canonical(
        std::filesystem::path{std::string{config_path}}, config_error);
    const auto canonical_state = std::filesystem::weakly_canonical(
        std::filesystem::path{std::string{state_path}}, state_error);
    if (!config_error && !state_error && canonical_config == canonical_state) {
        return make_file_error(
            PersistenceFileErrorCode::validation,
            "config and state paths must not alias the same file");
    }

    struct stat config_stat {};
    struct stat state_stat {};
    if (::stat(std::string{config_path}.c_str(), &config_stat) == 0 &&
        ::stat(std::string{state_path}.c_str(), &state_stat) == 0 &&
        config_stat.st_dev == state_stat.st_dev &&
        config_stat.st_ino == state_stat.st_ino) {
        return make_file_error(
            PersistenceFileErrorCode::validation,
            "config and state paths must not be hard-link aliases");
    }
    return {};
}

LoadedPersistenceFiles load_persistence_files(
    std::string_view config_path,
    std::string_view state_path) {
    LoadedPersistenceFiles loaded;

    const auto path_error = validate_persistence_paths(config_path, state_path);
    if (path_error) {
        loaded.config = make_default_config();
        loaded.state = make_default_state(loaded.config);
        loaded.config_error = path_error;
        loaded.state_writes_allowed = false;
        return loaded;
    }

    auto config_result = load_config_file(config_path);
    if (config_result.ok()) {
        loaded.config = std::move(*config_result.value);
    } else {
        loaded.config = make_default_config();
        loaded.config_error = std::move(config_result.error);
        loaded.state = make_default_state(loaded.config);
        loaded.state_writes_allowed = false;
        return loaded;
    }

    const auto pending_path = make_config_state_path(state_path, loaded.config.revision);
    auto pending_state = load_state_file(pending_path, loaded.config);
    if (pending_state.ok()) {
        loaded.state = std::move(*pending_state.value);
        const auto finalize_error = finalize_config_state_file(
            state_path,
            loaded.config.revision);
        if (finalize_error) {
            loaded.state_reconciled = true;
            loaded.pending_state_revision = loaded.config.revision;
        }
        return loaded;
    }
    if (pending_state.error.code != PersistenceFileErrorCode::not_found) {
        loaded.state_error = std::move(pending_state.error);
    }

    auto state_file = read_persistence_text_file(state_path);
    if (!state_file.ok()) {
        loaded.state = make_default_state(loaded.config);
        if (!loaded.state_error) {
            loaded.state_error = std::move(state_file.error);
        }
        return loaded;
    }

    auto parsed_state = parse_state_json(*state_file.value);
    if (!parsed_state.ok()) {
        loaded.state = make_default_state(loaded.config);
        if (!loaded.state_error) {
            loaded.state_error = map_model_error(parsed_state.error);
        }
        return loaded;
    }

    const auto exact_state_error = validate_state(*parsed_state.value, loaded.config);
    if (!exact_state_error) {
        loaded.state = std::move(*parsed_state.value);
        return loaded;
    }

    auto reconciled = reconcile_state_for_config(*parsed_state.value, loaded.config);
    if (reconciled.ok()) {
        loaded.state = std::move(*reconciled.value);
        loaded.state_reconciled = true;
        return loaded;
    }

    loaded.state = make_default_state(loaded.config);
    if (!loaded.state_error) {
        loaded.state_error = map_model_error(reconciled.error);
    }
    return loaded;
}

PersistenceFileResult<AppConfig> commit_config_file_atomic(
    std::string_view path,
    std::uint64_t current_revision,
    std::uint64_t expected_revision,
    const AppConfig& proposed_config) {
    const auto revision_error = validate_expected_revision(current_revision, expected_revision);
    if (revision_error) {
        return {{}, map_model_error(revision_error)};
    }

    const auto proposed_error = validate_config(proposed_config);
    if (proposed_error) {
        return {{}, map_model_error(proposed_error)};
    }

    if (current_revision == std::numeric_limits<std::uint64_t>::max()) {
        return {{}, make_file_error(
            PersistenceFileErrorCode::validation,
            "configuration revision counter exhausted")};
    }

    AppConfig committed = proposed_config;
    committed.revision = current_revision + 1U;
    const auto committed_error = validate_config(committed);
    if (committed_error) {
        return {{}, map_model_error(committed_error)};
    }

    const auto serialized = serialize_config_json(committed);
    if (serialized.size() > kPersistenceMaxFileBytes) {
        return {{}, make_file_error(
            PersistenceFileErrorCode::too_large,
            "canonical configuration exceeds persistence file size limit")};
    }

    const auto write_error = write_persistence_text_file_atomic(path, serialized);
    if (write_error) {
        return {{}, write_error};
    }

    return {std::move(committed), {}};
}

PersistenceFileError save_state_file_atomic(
    std::string_view path,
    const AppState& state,
    const AppConfig& config) {
    const auto validation = validate_state(state, config);
    if (validation) {
        return map_model_error(validation);
    }
    return write_persistence_text_file_atomic(path, serialize_state_json(state));
}

PersistenceFileError prepare_config_state_file(
    std::string_view state_path,
    const AppState& state,
    const AppConfig& committed_config) {
    const auto validation = validate_state(state, committed_config);
    if (validation) {
        return map_model_error(validation);
    }
    return write_persistence_text_file_atomic(
        make_config_state_path(state_path, committed_config.revision),
        serialize_state_json(state));
}

PersistenceFileError finalize_config_state_file(
    std::string_view state_path,
    std::uint64_t committed_revision) {
    const auto pending_path = make_config_state_path(state_path, committed_revision);
    const std::string target_path{state_path};
    if (::rename(pending_path.c_str(), target_path.c_str()) != 0) {
        const int error_number = errno;
        return make_file_error(
            error_number == ENOENT
                ? PersistenceFileErrorCode::not_found
                : PersistenceFileErrorCode::io,
            errno_message("finalize prepared state", pending_path, error_number));
    }
    return {};
}

PersistenceFileError discard_config_state_file(
    std::string_view state_path,
    std::uint64_t revision) {
    const auto pending_path = make_config_state_path(state_path, revision);
    if (::unlink(pending_path.c_str()) != 0 && errno != ENOENT) {
        const int error_number = errno;
        return make_file_error(
            PersistenceFileErrorCode::io,
            errno_message("discard prepared state", pending_path, error_number));
    }
    return {};
}

StatePersistenceManager::StatePersistenceManager(std::string state_path)
    : state_path_(std::move(state_path)) {}

void StatePersistenceManager::mark_dirty(time_point now) noexcept {
    if (!dirty_) {
        dirty_ = true;
        first_dirty_at_ = now;
    }
    last_change_at_ = now;
}

bool StatePersistenceManager::dirty() const noexcept {
    return dirty_;
}

bool StatePersistenceManager::save_due(time_point now) const noexcept {
    const auto deadline = next_deadline();
    return deadline.has_value() && now >= *deadline;
}

std::optional<StatePersistenceManager::time_point> StatePersistenceManager::next_deadline() const noexcept {
    if (!dirty_) {
        return std::nullopt;
    }
    auto deadline = std::min(last_change_at_ + kDebounceDelay, first_dirty_at_ + kMaxDirtyInterval);
    if (retry_not_before_.has_value()) {
        deadline = std::max(deadline, *retry_not_before_);
    }
    return deadline;
}

std::string_view StatePersistenceManager::state_path() const noexcept {
    return state_path_;
}

void StatePersistenceManager::defer_after_failure(time_point now) noexcept {
    if (dirty_) {
        retry_not_before_ = now + kRetryDelay;
    }
}

StateSaveResult StatePersistenceManager::save_if_due(
    const AppState& state,
    const AppConfig& config,
    time_point now) {
    if (!dirty_) {
        return {StateSaveAction::not_dirty, {}};
    }
    if (!save_due(now)) {
        return {StateSaveAction::not_due, {}};
    }

    auto error = save_state_file_atomic(state_path_, state, config);
    if (error) {
        defer_after_failure(now);
        return {StateSaveAction::failed, std::move(error)};
    }
    mark_saved();
    return {StateSaveAction::saved, {}};
}

StateSaveResult StatePersistenceManager::flush(
    const AppState& state,
    const AppConfig& config) {
    if (!dirty_) {
        return {StateSaveAction::not_dirty, {}};
    }

    auto error = save_state_file_atomic(state_path_, state, config);
    if (error) {
        return {StateSaveAction::failed, std::move(error)};
    }
    mark_saved();
    return {StateSaveAction::saved, {}};
}

void StatePersistenceManager::mark_saved() noexcept {
    dirty_ = false;
    first_dirty_at_ = {};
    last_change_at_ = {};
    retry_not_before_.reset();
}

}  // namespace dmxwb
