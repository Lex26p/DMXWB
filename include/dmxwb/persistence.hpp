#pragma once

#include "dmxwb/fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmxwb {

inline constexpr std::uint32_t kPersistenceVersion = 1;
inline constexpr std::uint16_t kArtNetUniverseMax = 32767;

using GroupId = std::uint64_t;
using SceneId = std::uint64_t;

enum class PersistedSource {
    mqtt,
    artnet,
};

struct FixtureConfigRecord final {
    Fixture::Id id{0};
    std::string name;
    [[nodiscard]] friend bool operator==(const FixtureConfigRecord&, const FixtureConfigRecord&) = default;
};

struct GroupConfigRecord final {
    GroupId id{0};
    std::string name;
    std::vector<Fixture::Id> members;
    [[nodiscard]] friend bool operator==(const GroupConfigRecord&, const GroupConfigRecord&) = default;
};

struct SceneFixtureRecord final {
    Fixture::Id fixture_id{0};
    RgbwValues rgbw{};
    std::uint8_t brightness{100};
    bool requested_power{false};
    [[nodiscard]] friend bool operator==(const SceneFixtureRecord&, const SceneFixtureRecord&) = default;
};

struct SceneConfigRecord final {
    SceneId id{0};
    std::string name;
    std::vector<SceneFixtureRecord> fixtures;
    [[nodiscard]] friend bool operator==(const SceneConfigRecord&, const SceneConfigRecord&) = default;
};

struct IdCounters final {
    Fixture::Id next_fixture_id{1};
    GroupId next_group_id{1};
    SceneId next_scene_id{1};
    [[nodiscard]] friend bool operator==(const IdCounters&, const IdCounters&) = default;
};

struct AppConfig final {
    std::uint32_t version{kPersistenceVersion};
    std::uint64_t revision{0};
    std::string dmx_port{"/dev/ttyRS485-1"};
    std::uint16_t artnet_universe{0};
    std::size_t fixture_count{0};
    std::size_t start_address{1};
    std::vector<FixtureConfigRecord> fixtures;
    std::vector<GroupConfigRecord> groups;
    std::vector<SceneConfigRecord> scenes;
    IdCounters id_counters{};
    [[nodiscard]] friend bool operator==(const AppConfig&, const AppConfig&) = default;
};

struct FixtureRuntimeState final {
    Fixture::Id id{0};
    bool requested_power{false};
    RgbwValues rgbw{
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue};
    std::uint8_t brightness{kFixtureDefaultBrightness};
    std::uint8_t temperature{kFixtureDefaultTemperature};
    [[nodiscard]] friend bool operator==(const FixtureRuntimeState&, const FixtureRuntimeState&) = default;
};

struct AppState final {
    std::uint32_t version{kPersistenceVersion};
    PersistedSource source{PersistedSource::mqtt};
    std::vector<FixtureRuntimeState> fixtures;
    [[nodiscard]] friend bool operator==(const AppState&, const AppState&) = default;
};

enum class PersistenceErrorCode {
    none,
    json_syntax,
    schema,
    version,
    validation,
    revision_conflict,
};

struct PersistenceError final {
    PersistenceErrorCode code{PersistenceErrorCode::none};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != PersistenceErrorCode::none;
    }
};

template <typename T>
struct PersistenceResult final {
    std::optional<T> value;
    PersistenceError error;

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !error;
    }
};

[[nodiscard]] AppConfig make_default_config();
[[nodiscard]] AppState make_default_state(const AppConfig& config);

[[nodiscard]] PersistenceError validate_config(const AppConfig& config);
[[nodiscard]] PersistenceError validate_state(const AppState& state, const AppConfig& config);
[[nodiscard]] PersistenceError validate_expected_revision(
    std::uint64_t current_revision,
    std::uint64_t expected_revision);

[[nodiscard]] std::string serialize_config_json(const AppConfig& config);
[[nodiscard]] std::string serialize_state_json(const AppState& state);

[[nodiscard]] PersistenceResult<AppConfig> parse_config_json(std::string_view json);
[[nodiscard]] PersistenceResult<AppState> parse_state_json(std::string_view json);

[[nodiscard]] PersistenceError restore_fixture_collection(
    const AppConfig& config,
    const AppState& state,
    FixtureCollection& collection);

[[nodiscard]] std::string_view persisted_source_name(PersistedSource source) noexcept;

}  // namespace dmxwb
