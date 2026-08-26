#include "dmxwb/persistence_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
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

LoadedPersistenceFiles load_persistence_files(
    std::string_view config_path,
    std::string_view state_path) {
    LoadedPersistenceFiles loaded;

    auto config_result = load_config_file(config_path);
    if (config_result.ok()) {
        loaded.config = std::move(*config_result.value);
    } else {
        loaded.config = make_default_config();
        loaded.config_error = std::move(config_result.error);
    }

    auto state_result = load_state_file(state_path, loaded.config);
    if (state_result.ok()) {
        loaded.state = std::move(*state_result.value);
    } else {
        loaded.state = make_default_state(loaded.config);
        loaded.state_error = std::move(state_result.error);
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

    const auto write_error = write_persistence_text_file_atomic(path, serialize_config_json(committed));
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
    return std::min(last_change_at_ + kDebounceDelay, first_dirty_at_ + kMaxDirtyInterval);
}

std::string_view StatePersistenceManager::state_path() const noexcept {
    return state_path_;
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
}

}  // namespace dmxwb
