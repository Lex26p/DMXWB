#pragma once

#include "dmxwb/fixture.hpp"
#include "dmxwb/group_scene.hpp"
#include "dmxwb/persistence.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmxwb {

inline constexpr std::string_view kMqttBrokerHost = "127.0.0.1";
inline constexpr std::uint16_t kMqttBrokerPort = 1883;
inline constexpr std::string_view kMqttSystemDeviceId = "dmxwb";
inline constexpr std::string_view kMqttSystemSourceCommandTopic =
    "/devices/dmxwb/controls/source/on";
// MQTT '+' wildcard occupies one complete topic level. All DMXWB Fixture,
// Group and Scene device commands fit this one subscription; the parser then
// accepts only the supported DMXWB device prefixes/controls.
inline constexpr std::string_view kMqttDeviceCommandSubscription =
    "/devices/+/controls/+/on";

inline constexpr std::string_view kMqttConfigTopic = "/dmxwb/config";
inline constexpr std::string_view kMqttConfigSetTopic = "/dmxwb/config/set";
inline constexpr std::string_view kMqttConfigResultTopic = "/dmxwb/config/result";
inline constexpr std::string_view kMqttStateTopic = "/dmxwb/state";
inline constexpr std::string_view kMqttStatusTopic = "/dmxwb/status";

enum class MqttCommandType {
    set_source,
    set_config,
    fixture_name,
    fixture_power,
    fixture_red,
    fixture_green,
    fixture_blue,
    fixture_color,
    fixture_brightness,
    fixture_temperature,
    fixture_reset,
    group_name,
    group_power,
    group_red,
    group_green,
    group_blue,
    group_color,
    group_brightness,
    group_temperature,
    group_reset,
    scene_name,
    scene_apply,
};

struct MqttCommand final {
    MqttCommandType type{MqttCommandType::set_source};
    Fixture::Id fixture_id{0};
    GroupId group_id{0};
    SceneId scene_id{0};
    PersistedSource source{PersistedSource::mqtt};
    std::string text;
    std::uint8_t value{0};
    bool boolean_value{false};
    RgbwValues color{};

    [[nodiscard]] friend bool operator==(const MqttCommand&, const MqttCommand&) = default;
};

enum class MqttCommandParseStatus {
    ignored,
    accepted,
    rejected,
};

struct MqttCommandParseResult final {
    MqttCommandParseStatus status{MqttCommandParseStatus::ignored};
    std::optional<MqttCommand> command;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept {
        return status == MqttCommandParseStatus::accepted && command.has_value();
    }
};

[[nodiscard]] MqttCommandParseResult parse_mqtt_command(
    std::string_view topic,
    std::string_view payload,
    bool retained);

// MQTT callback помещает сюда только полностью разобранные live commands.
// Controller извлекает их последовательно; callback не касается serial/files.
class MqttCommandQueue final {
public:
    void push(MqttCommand command);
    [[nodiscard]] std::optional<MqttCommand> try_pop();
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<MqttCommand> commands_;
};

struct MqttPublication final {
    std::string topic;
    std::string payload;
    bool retained{true};

    [[nodiscard]] friend bool operator==(const MqttPublication&, const MqttPublication&) = default;
};

enum class MqttApplicationStatus {
    running,
    error,
    off,
};

[[nodiscard]] std::string_view mqtt_application_status_name(MqttApplicationStatus status) noexcept;

[[nodiscard]] std::vector<MqttPublication> build_system_metadata_publications();
[[nodiscard]] std::vector<MqttPublication> build_system_state_publications(
    MqttApplicationStatus status,
    PersistedSource source);

[[nodiscard]] std::vector<MqttPublication> build_fixture_metadata_publications(const Fixture& fixture);
[[nodiscard]] std::vector<MqttPublication> build_fixture_state_publications(const Fixture& fixture);
[[nodiscard]] std::vector<MqttPublication> build_fixture_retained_cleanup_publications(Fixture::Id fixture_id);

[[nodiscard]] std::vector<MqttPublication> build_group_metadata_publications(const GroupConfigRecord& group);
[[nodiscard]] std::vector<MqttPublication> build_group_state_publications(
    const GroupConfigRecord& group,
    const GroupControlState& state);
[[nodiscard]] std::vector<MqttPublication> build_group_retained_cleanup_publications(GroupId group_id);

[[nodiscard]] std::vector<MqttPublication> build_scene_metadata_publications(const SceneConfigRecord& scene);
[[nodiscard]] std::vector<MqttPublication> build_scene_state_publications(const SceneConfigRecord& scene);
[[nodiscard]] std::vector<MqttPublication> build_scene_retained_cleanup_publications(SceneId scene_id);

// Internal/web snapshots уже сериализованы владельцами данных. Эта функция
// только сопоставляет канонические payload с retained MQTT topics.
[[nodiscard]] std::vector<MqttPublication> build_internal_snapshot_publications(
    std::string config_json,
    std::string state_json,
    std::string status_json);

}  // namespace dmxwb
