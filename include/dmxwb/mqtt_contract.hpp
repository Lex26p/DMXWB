#pragma once

#include "dmxwb/fixture.hpp"
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
// MQTT '+' wildcard must occupy a complete topic level. A partial filter such
// as "dmxwb_fixture_+" is invalid. Subscribe once to all WB device command
// topics and let parse_mqtt_command() keep only DMXWB system/Fixture commands.
inline constexpr std::string_view kMqttDeviceCommandSubscription =
    "/devices/+/controls/+/on";

inline constexpr std::string_view kMqttConfigTopic = "/dmxwb/config";
inline constexpr std::string_view kMqttConfigSetTopic = "/dmxwb/config/set";
inline constexpr std::string_view kMqttConfigResultTopic = "/dmxwb/config/result";
inline constexpr std::string_view kMqttStateTopic = "/dmxwb/state";
inline constexpr std::string_view kMqttStatusTopic = "/dmxwb/status";

// Типы команд намеренно не зависят от MQTT broker/transport. DEV-007B
// преобразует libmosquitto callbacks в эти команды и передаст их Controller.
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
};

struct MqttCommand final {
    MqttCommandType type{MqttCommandType::set_source};
    Fixture::Id fixture_id{0};
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

// MQTT callback помещает сюда только полностью разобранные команды. Controller
// извлекает их последовательно; callback остаётся коротким и не касается serial
// или файлов persistence.
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

// Internal/web snapshots уже сериализованы владельцами данных. Эта функция
// только сопоставляет канонические payload с retained MQTT topics.
[[nodiscard]] std::vector<MqttPublication> build_internal_snapshot_publications(
    std::string config_json,
    std::string state_json,
    std::string status_json);

}  // namespace dmxwb
