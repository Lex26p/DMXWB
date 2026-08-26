#include "dmxwb/mqtt_contract.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dmxwb {
namespace {

constexpr std::string_view kFixturePrefix = "/devices/dmxwb_fixture_";
constexpr std::string_view kControlsMarker = "/controls/";
constexpr std::string_view kCommandSuffix = "/on";

[[nodiscard]] MqttCommandParseResult ignored_result() {
    return {MqttCommandParseStatus::ignored, std::nullopt, {}};
}

[[nodiscard]] MqttCommandParseResult rejected_result(std::string message) {
    return {MqttCommandParseStatus::rejected, std::nullopt, std::move(message)};
}

[[nodiscard]] MqttCommandParseResult accepted_result(MqttCommand command) {
    return {MqttCommandParseStatus::accepted, std::move(command), {}};
}

[[nodiscard]] bool parse_u64_decimal(std::string_view text, std::uint64_t& output) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool parse_uint8_range(
    std::string_view text,
    std::uint8_t maximum,
    std::uint8_t& output) noexcept {
    std::uint64_t parsed = 0;
    if (!parse_u64_decimal(text, parsed) || parsed > maximum) {
        return false;
    }
    output = static_cast<std::uint8_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_color(std::string_view payload, RgbwValues& output) noexcept {
    const auto first = payload.find(';');
    if (first == std::string_view::npos) {
        return false;
    }
    const auto second = payload.find(';', first + 1U);
    if (second == std::string_view::npos || payload.find(';', second + 1U) != std::string_view::npos) {
        return false;
    }

    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    if (!parse_uint8_range(payload.substr(0, first), 255U, red) ||
        !parse_uint8_range(payload.substr(first + 1U, second - first - 1U), 255U, green) ||
        !parse_uint8_range(payload.substr(second + 1U), 255U, blue)) {
        return false;
    }
    output = RgbwValues{red, green, blue, 0};
    return true;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = bytes[index];
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            codepoint = static_cast<std::uint32_t>(first & 0x1FU);
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            codepoint = static_cast<std::uint32_t>(first & 0x0FU);
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            codepoint = static_cast<std::uint32_t>(first & 0x07U);
        } else {
            return false;
        }

        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = bytes[index + offset];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(next & 0x3FU);
        }

        if ((continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] std::string fixture_device_prefix(Fixture::Id id) {
    return std::string{"/devices/dmxwb_fixture_"} + std::to_string(id);
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[(byte >> 4U) & 0x0FU]);
                    output.push_back(hex[byte & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string make_device_meta(std::string_view title) {
    std::string output{"{\"driver\":\"dmxwb\",\"title\":{\"en\":"};
    append_json_string(output, title);
    output += ",\"ru\":";
    append_json_string(output, title);
    output += "}}";
    return output;
}

[[nodiscard]] std::string make_control_meta(
    std::string_view type,
    bool readonly,
    bool hidden,
    std::string_view english_title,
    std::string_view russian_title,
    std::optional<std::uint32_t> minimum = std::nullopt,
    std::optional<std::uint32_t> maximum = std::nullopt,
    std::string_view extra = {}) {
    std::string output{"{\"type\":"};
    append_json_string(output, type);
    output += ",\"readonly\":";
    output += readonly ? "true" : "false";
    output += ",\"hidden\":";
    output += hidden ? "true" : "false";
    if (minimum.has_value()) {
        output += ",\"min\":" + std::to_string(*minimum);
    }
    if (maximum.has_value()) {
        output += ",\"max\":" + std::to_string(*maximum);
    }
    output += ",\"title\":{\"en\":";
    append_json_string(output, english_title);
    output += ",\"ru\":";
    append_json_string(output, russian_title);
    output += '}';
    if (!extra.empty()) {
        output += ',';
        output += extra;
    }
    output += '}';
    return output;
}

void append_publication(
    std::vector<MqttPublication>& output,
    std::string topic,
    std::string payload,
    bool retained = true) {
    output.push_back(MqttPublication{std::move(topic), std::move(payload), retained});
}

[[nodiscard]] std::string fixture_control_topic(Fixture::Id id, std::string_view control) {
    auto topic = fixture_device_prefix(id);
    topic += "/controls/";
    topic += control;
    return topic;
}

}  // namespace

MqttCommandParseResult parse_mqtt_command(
    std::string_view topic,
    std::string_view payload,
    bool retained) {
    if (topic == kMqttSystemSourceCommandTopic) {
        if (retained) {
            return ignored_result();
        }
        MqttCommand command;
        command.type = MqttCommandType::set_source;
        if (payload == "mqtt") {
            command.source = PersistedSource::mqtt;
        } else if (payload == "artnet") {
            command.source = PersistedSource::artnet;
        } else {
            return rejected_result("source command must be 'mqtt' or 'artnet'");
        }
        return accepted_result(std::move(command));
    }

    if (!topic.starts_with(kFixturePrefix) || !topic.ends_with(kCommandSuffix)) {
        return ignored_result();
    }
    if (retained) {
        return ignored_result();
    }

    const auto marker = topic.find(kControlsMarker, kFixturePrefix.size());
    if (marker == std::string_view::npos) {
        return ignored_result();
    }
    const auto id_text = topic.substr(kFixturePrefix.size(), marker - kFixturePrefix.size());
    std::uint64_t fixture_id = 0;
    if (!parse_u64_decimal(id_text, fixture_id) || fixture_id == 0) {
        return rejected_result("fixture topic contains invalid stable ID");
    }

    const auto control_begin = marker + kControlsMarker.size();
    const auto control_size = topic.size() - control_begin - kCommandSuffix.size();
    if (control_size == 0) {
        return ignored_result();
    }
    const auto control = topic.substr(control_begin, control_size);

    MqttCommand command;
    command.fixture_id = fixture_id;

    if (control == "name") {
        if (!is_valid_utf8(payload)) {
            return rejected_result("fixture name must be valid UTF-8");
        }
        command.type = MqttCommandType::fixture_name;
        command.text = std::string{payload};
        return accepted_result(std::move(command));
    }
    if (control == "power") {
        command.type = MqttCommandType::fixture_power;
        if (payload == "0") {
            command.boolean_value = false;
        } else if (payload == "1") {
            command.boolean_value = true;
        } else {
            return rejected_result("power command must be 0 or 1");
        }
        return accepted_result(std::move(command));
    }
    if (control == "color") {
        command.type = MqttCommandType::fixture_color;
        if (!parse_color(payload, command.color)) {
            return rejected_result("color command must be R;G;B with each component in 0..255");
        }
        return accepted_result(std::move(command));
    }
    if (control == "reset") {
        if (payload != "1") {
            return rejected_result("reset command must be 1");
        }
        command.type = MqttCommandType::fixture_reset;
        return accepted_result(std::move(command));
    }

    std::uint8_t maximum = 255;
    if (control == "red") {
        command.type = MqttCommandType::fixture_red;
    } else if (control == "green") {
        command.type = MqttCommandType::fixture_green;
    } else if (control == "blue") {
        command.type = MqttCommandType::fixture_blue;
    } else if (control == "brightness") {
        command.type = MqttCommandType::fixture_brightness;
        maximum = 100;
    } else if (control == "temperature") {
        command.type = MqttCommandType::fixture_temperature;
        maximum = 100;
    } else {
        return ignored_result();
    }

    if (!parse_uint8_range(payload, maximum, command.value)) {
        return rejected_result("numeric fixture command is outside the allowed range");
    }
    return accepted_result(std::move(command));
}

void MqttCommandQueue::push(MqttCommand command) {
    std::lock_guard lock{mutex_};
    commands_.push_back(std::move(command));
}

std::optional<MqttCommand> MqttCommandQueue::try_pop() {
    std::lock_guard lock{mutex_};
    if (commands_.empty()) {
        return std::nullopt;
    }
    auto command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

std::size_t MqttCommandQueue::size() const {
    std::lock_guard lock{mutex_};
    return commands_.size();
}

std::string_view mqtt_application_status_name(MqttApplicationStatus status) noexcept {
    switch (status) {
        case MqttApplicationStatus::running:
            return "running";
        case MqttApplicationStatus::error:
            return "error";
        case MqttApplicationStatus::off:
            return "off";
    }
    return "error";
}

std::vector<MqttPublication> build_system_metadata_publications() {
    std::vector<MqttPublication> output;
    output.reserve(3);
    append_publication(output, "/devices/dmxwb/meta", make_device_meta("DMXWB"));
    append_publication(
        output,
        "/devices/dmxwb/controls/status/meta",
        make_control_meta(
            "text",
            true,
            false,
            "Status",
            "Статус",
            std::nullopt,
            std::nullopt,
            "\"enum\":{\"running\":{\"en\":\"Running\",\"ru\":\"Работает\"},"
            "\"error\":{\"en\":\"Error\",\"ru\":\"Ошибка\"},"
            "\"off\":{\"en\":\"Off\",\"ru\":\"Выключено\"}}"));
    append_publication(
        output,
        "/devices/dmxwb/controls/source/meta",
        make_control_meta(
            "text",
            false,
            false,
            "Source",
            "Источник",
            std::nullopt,
            std::nullopt,
            "\"enum\":{\"mqtt\":{\"en\":\"WB MQTT\",\"ru\":\"WB MQTT\"},"
            "\"artnet\":{\"en\":\"ART-NET\",\"ru\":\"ART-NET\"}}"));
    return output;
}

std::vector<MqttPublication> build_system_state_publications(
    MqttApplicationStatus status,
    PersistedSource source) {
    std::vector<MqttPublication> output;
    output.reserve(2);
    append_publication(
        output,
        "/devices/dmxwb/controls/status",
        std::string{mqtt_application_status_name(status)});
    append_publication(
        output,
        "/devices/dmxwb/controls/source",
        std::string{persisted_source_name(source)});
    return output;
}

std::vector<MqttPublication> build_fixture_metadata_publications(const Fixture& fixture) {
    const auto prefix = fixture_device_prefix(fixture.id());
    std::vector<MqttPublication> output;
    output.reserve(10);
    append_publication(output, prefix + "/meta", make_device_meta(fixture.name()));

    const auto add = [&](std::string_view control, std::string payload) {
        append_publication(output, prefix + "/controls/" + std::string{control} + "/meta", std::move(payload));
    };

    add("name", make_control_meta("text", false, true, "Name", "Имя"));
    add("power", make_control_meta("switch", false, true, "Power", "Питание"));
    add("red", make_control_meta("range", false, true, "Red", "Красный", 0U, 255U));
    add("green", make_control_meta("range", false, true, "Green", "Зелёный", 0U, 255U));
    add("blue", make_control_meta("range", false, true, "Blue", "Синий", 0U, 255U));
    add("color", make_control_meta("rgb", false, true, "Color", "Цвет"));
    add("brightness", make_control_meta("range", false, true, "Brightness", "Яркость", 0U, 100U));
    add("temperature", make_control_meta("range", false, true, "Temperature", "Температура", 0U, 100U));
    add("reset", make_control_meta("pushbutton", false, true, "Reset", "Сброс"));
    return output;
}

std::vector<MqttPublication> build_fixture_state_publications(const Fixture& fixture) {
    const auto actual = fixture.actual_rgbw();
    std::vector<MqttPublication> output;
    output.reserve(8);

    append_publication(output, fixture_control_topic(fixture.id(), "name"), std::string{fixture.name()});
    append_publication(
        output,
        fixture_control_topic(fixture.id(), "power"),
        fixture.actual_power() ? "1" : "0");
    append_publication(output, fixture_control_topic(fixture.id(), "red"), std::to_string(actual.red));
    append_publication(output, fixture_control_topic(fixture.id(), "green"), std::to_string(actual.green));
    append_publication(output, fixture_control_topic(fixture.id(), "blue"), std::to_string(actual.blue));
    append_publication(
        output,
        fixture_control_topic(fixture.id(), "color"),
        std::to_string(actual.red) + ";" + std::to_string(actual.green) + ";" + std::to_string(actual.blue));
    append_publication(
        output,
        fixture_control_topic(fixture.id(), "brightness"),
        std::to_string(fixture.brightness()));
    append_publication(
        output,
        fixture_control_topic(fixture.id(), "temperature"),
        std::to_string(fixture.temperature()));
    return output;
}

std::vector<MqttPublication> build_internal_snapshot_publications(
    std::string config_json,
    std::string state_json,
    std::string status_json) {
    std::vector<MqttPublication> output;
    output.reserve(3);
    append_publication(output, std::string{kMqttConfigTopic}, std::move(config_json));
    append_publication(output, std::string{kMqttStateTopic}, std::move(state_json));
    append_publication(output, std::string{kMqttStatusTopic}, std::move(status_json));
    return output;
}

}  // namespace dmxwb
