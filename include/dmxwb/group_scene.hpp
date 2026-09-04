#pragma once

#include "dmxwb/persistence_runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dmxwb {

enum class GroupControlType {
    power,
    red,
    green,
    blue,
    color,
    brightness,
    temperature,
    reset,
};

struct GroupControlCommand final {
    GroupId group_id{0};
    GroupControlType type{GroupControlType::power};
    bool boolean_value{false};
    std::uint8_t value{0};
    RgbwValues color{};
};

struct GroupControlState final {
    GroupId id{0};
    bool actual_power{false};
    std::uint8_t red{kFixtureDefaultChannelValue};
    std::uint8_t green{kFixtureDefaultChannelValue};
    std::uint8_t blue{kFixtureDefaultChannelValue};
    std::uint8_t brightness{kFixtureDefaultBrightness};
    std::uint8_t temperature{kFixtureDefaultTemperature};

    [[nodiscard]] friend bool operator==(const GroupControlState&, const GroupControlState&) = default;
};

struct GroupControlResult final {
    bool applied{false};
    bool fixture_state_changed{false};
    GroupControlState state{};
    std::string error;
};

struct SceneOperationResult final {
    bool ok{false};
    bool config_changed{false};
    bool fixture_state_changed{false};
    SceneId scene_id{0};
    std::uint64_t revision{0};
    std::string error;
};

// Runtime logic shared by MQTT and the future Web-facing scene lifecycle.
// Structural data remains in AppConfig/PersistenceRuntime. Group control state
// is intentionally ephemeral: only Fixture logical state is canonical state.json.
class GroupSceneManager final {
public:
    using time_point = PersistenceRuntime::time_point;

    explicit GroupSceneManager(PersistenceRuntime& runtime);

    [[nodiscard]] std::optional<GroupControlState> group_state(GroupId group_id);
    [[nodiscard]] GroupControlResult apply_group_command(
        const GroupControlCommand& command,
        time_point now);

    [[nodiscard]] SceneOperationResult create_scene(
        std::string name,
        time_point now,
        std::optional<std::string> idempotency_request_id = std::nullopt);
    [[nodiscard]] SceneOperationResult overwrite_scene(SceneId scene_id, time_point now);
    [[nodiscard]] SceneOperationResult rename_scene(SceneId scene_id, std::string name, time_point now);
    [[nodiscard]] SceneOperationResult delete_scene(SceneId scene_id, time_point now);
    [[nodiscard]] SceneOperationResult apply_scene(SceneId scene_id, time_point now);

    // Reconcile ephemeral Group controls after an external config transaction.
    // Existing stable IDs keep their last control values; new IDs get defaults.
    void synchronize_config();

private:
    struct StoredGroupState final {
        GroupId id{0};
        std::uint8_t red{kFixtureDefaultChannelValue};
        std::uint8_t green{kFixtureDefaultChannelValue};
        std::uint8_t blue{kFixtureDefaultChannelValue};
        std::uint8_t brightness{kFixtureDefaultBrightness};
        std::uint8_t temperature{kFixtureDefaultTemperature};
    };

    [[nodiscard]] const GroupConfigRecord* find_group(GroupId group_id) const noexcept;
    [[nodiscard]] const SceneConfigRecord* find_scene(SceneId scene_id) const noexcept;
    [[nodiscard]] Fixture* find_fixture(Fixture::Id fixture_id) noexcept;
    [[nodiscard]] const Fixture* find_fixture(Fixture::Id fixture_id) const noexcept;
    [[nodiscard]] StoredGroupState* find_group_state(GroupId group_id) noexcept;
    [[nodiscard]] bool group_actual_power(const GroupConfigRecord& group) const noexcept;
    [[nodiscard]] GroupControlState build_group_state(
        const GroupConfigRecord& group,
        const StoredGroupState& stored) const noexcept;
    [[nodiscard]] std::vector<SceneFixtureRecord> capture_scene_snapshot() const;
    [[nodiscard]] SceneOperationResult commit_scene_config(
        AppConfig proposed,
        SceneId scene_id,
        time_point now,
        std::optional<SceneCreateIdempotencyRecord> idempotency_record = std::nullopt);

    PersistenceRuntime& runtime_;
    std::vector<StoredGroupState> group_states_;
    std::uint64_t synchronized_revision_{0};
};

}  // namespace dmxwb
