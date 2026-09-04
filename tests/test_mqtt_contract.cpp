#include "dmxwb/mqtt_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool condition, std::string_view name) {
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failures;
        std::cerr << "[FAIL] " << name << '\n';
    }
}

const dmxwb::MqttPublication* find_publication(
    const std::vector<dmxwb::MqttPublication>& publications,
    std::string_view topic) {
    const auto found = std::find_if(
        publications.begin(),
        publications.end(),
        [topic](const auto& publication) { return publication.topic == topic; });
    return found == publications.end() ? nullptr : &*found;
}

void test_source_command_parser() {
    auto parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on",
        "artnet",
        false);
    expect_true(parsed.accepted(), "source /on command accepted");
    expect_true(
        parsed.command.has_value() && parsed.command->type == dmxwb::MqttCommandType::set_source,
        "source command type parsed");
    expect_true(
        parsed.command.has_value() && parsed.command->source == dmxwb::PersistedSource::artnet,
        "source artnet payload parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on",
        "mqtt",
        true);
    expect_true(
        parsed.status == dmxwb::MqttCommandParseStatus::ignored && !parsed.command.has_value(),
        "retained source command ignored");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb/controls/source/on",
        "invalid",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "invalid source rejected");
}

void test_fixture_command_parser() {
    auto parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/power/on",
        "1",
        false);
    expect_true(parsed.accepted(), "fixture power command accepted");
    expect_true(parsed.command.has_value() && parsed.command->fixture_id == 42, "fixture stable ID parsed");
    expect_true(parsed.command.has_value() && parsed.command->boolean_value, "fixture power value parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/red/on",
        "255",
        false);
    expect_true(
        parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::fixture_red && parsed.command->value == 255,
        "fixture RGB numeric command parsed");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/brightness/on",
        "100",
        false);
    expect_true(parsed.accepted() && parsed.command->value == 100, "brightness upper bound accepted");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/brightness/on",
        "101",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "brightness above 100 rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/red/on",
        " 7",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "numeric payload with whitespace rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_0/controls/red/on",
        "7",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "fixture stable ID zero rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_42/controls/white/on",
        "7",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::ignored, "unsupported white control ignored");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/other/controls/red/on",
        "7",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::ignored, "unrelated MQTT topic ignored");
}

void test_color_name_reset_and_retained() {
    auto parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/color/on",
        "12;34;255",
        false);
    expect_true(parsed.accepted(), "RGB color command accepted");
    expect_true(
        parsed.command.has_value() && parsed.command->color == dmxwb::RgbwValues{12, 34, 255, 0},
        "RGB color components parsed and W stays zero");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/color/on",
        "12;34;256",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "RGB color component above 255 rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/reset/on",
        "1",
        false);
    expect_true(
        parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::fixture_reset,
        "reset pushbutton command accepted");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/reset/on",
        "0",
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "reset payload other than 1 rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/name/on",
        "Сцена справа",
        false);
    expect_true(parsed.accepted() && parsed.command->text == "Сцена справа", "UTF-8 Fixture name accepted");

    const std::string invalid_utf8{"\xC3\x28", 2};
    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/name/on",
        invalid_utf8,
        false);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::rejected, "invalid UTF-8 Fixture name rejected");

    parsed = dmxwb::parse_mqtt_command(
        "/devices/dmxwb_fixture_7/controls/color/on",
        "1;2;3",
        true);
    expect_true(
        parsed.status == dmxwb::MqttCommandParseStatus::ignored,
        "retained Fixture /on command ignored before execution");
}

void test_config_set_command_contract() {
    const std::string payload{"{\"request_id\":\"r1\",\"expected_revision\":1,\"config\":{}}"};
    auto parsed = dmxwb::parse_mqtt_command(dmxwb::kMqttConfigSetTopic, payload, false);
    expect_true(
        parsed.accepted() && parsed.command->type == dmxwb::MqttCommandType::set_config,
        "non-retained /dmxwb/config/set is queued as Controller command");
    expect_true(parsed.accepted() && parsed.command->text == payload,
        "config/set callback preserves raw payload for Controller parsing");

    parsed = dmxwb::parse_mqtt_command(dmxwb::kMqttConfigSetTopic, payload, true);
    expect_true(parsed.status == dmxwb::MqttCommandParseStatus::ignored,
        "retained /dmxwb/config/set is ignored before Controller");
}

void test_fixture_retained_cleanup() {
    const auto publications = dmxwb::build_fixture_retained_cleanup_publications(42);
    expect_true(publications.size() == 18, "Fixture removal clears device/control metadata and retained states");
    bool all_retained_empty = true;
    for (const auto& publication : publications) {
        all_retained_empty = all_retained_empty && publication.retained && publication.payload.empty();
    }
    expect_true(all_retained_empty, "Fixture cleanup uses empty retained MQTT publications");
    expect_true(find_publication(publications, "/devices/dmxwb_fixture_42/meta") != nullptr,
        "Fixture cleanup removes device metadata");
    expect_true(find_publication(publications, "/devices/dmxwb_fixture_42/controls/reset/meta") != nullptr,
        "Fixture cleanup removes stateless Reset metadata");
}

void test_command_queue_fifo() {
    dmxwb::MqttCommandQueue queue;
    dmxwb::MqttCommand first;
    first.type = dmxwb::MqttCommandType::fixture_red;
    first.fixture_id = 1;
    first.value = 10;
    dmxwb::MqttCommand second;
    second.type = dmxwb::MqttCommandType::fixture_blue;
    second.fixture_id = 2;
    second.value = 20;

    queue.push(first);
    queue.push(second);
    expect_true(queue.size() == 2, "Controller command queue reports queued commands");
    const auto popped_first = queue.try_pop();
    const auto popped_second = queue.try_pop();
    expect_true(popped_first.has_value() && *popped_first == first, "Controller queue preserves first command");
    expect_true(popped_second.has_value() && *popped_second == second, "Controller queue preserves FIFO order");
    expect_true(!queue.try_pop().has_value(), "Controller queue empty after pops");
}

void test_system_publications() {
    const auto metadata = dmxwb::build_system_metadata_publications();
    expect_true(metadata.size() == 3, "system metadata contains device/status/source records");
    const auto* source_meta = find_publication(metadata, "/devices/dmxwb/controls/source/meta");
    expect_true(source_meta != nullptr && source_meta->retained, "system Source metadata is retained");
    expect_true(
        source_meta != nullptr && source_meta->payload.find("\"mqtt\"") != std::string::npos &&
            source_meta->payload.find("\"artnet\"") != std::string::npos,
        "system Source metadata exposes mqtt/artnet enum");

    const auto state = dmxwb::build_system_source_publications(dmxwb::PersistedSource::mqtt);
    const auto* source = find_publication(state, "/devices/dmxwb/controls/source");
    expect_true(source != nullptr && source->payload == "mqtt" && source->retained, "system Source retained state built");
    expect_true(find_publication(state, "/devices/dmxwb/controls/status") == nullptr,
        "Controller-facing system state helper does not publish operational status");
}

void test_fixture_metadata_visible() {
    dmxwb::Fixture fixture{42, "Правая \"линия\""};
    const auto metadata = dmxwb::build_fixture_metadata_publications(fixture);
    expect_true(metadata.size() == 10, "Fixture metadata includes device plus nine controls");

    const auto* device_meta = find_publication(metadata, "/devices/dmxwb_fixture_42/meta");
    expect_true(
        device_meta != nullptr && device_meta->payload.find("Правая \\\"линия\\\"") != std::string::npos,
        "Fixture device metadata JSON-escapes Name");

    const std::vector<std::string_view> controls{
        "name", "power", "red", "green", "blue", "color", "brightness", "temperature", "reset"};
    bool all_visible = true;
    for (const auto control : controls) {
        const auto topic = std::string{"/devices/dmxwb_fixture_42/controls/"} + std::string{control} + "/meta";
        const auto* publication = find_publication(metadata, topic);
        all_visible = all_visible && publication != nullptr && publication->retained &&
            publication->payload.find("\"hidden\":false") != std::string::npos &&
            publication->payload.find("\"hidden\":true") == std::string::npos;
    }
    expect_true(all_visible, "all Fixture control metadata is retained and visible in standard WB web");
}

void test_fixture_factual_state_publications() {
    dmxwb::Fixture fixture{42, "Fixture 42"};
    fixture.set_power(true);
    fixture.set_color(200, 100, 50);
    expect_true(fixture.set_brightness(50), "fixture brightness setup accepted");

    auto state = dmxwb::build_fixture_state_publications(fixture);
    const auto prefix = std::string{"/devices/dmxwb_fixture_42/controls/"};
    expect_true(find_publication(state, prefix + "power")->payload == "1", "Fixture factual power state is ON");
    expect_true(find_publication(state, prefix + "red")->payload == "100", "Fixture red state is actual post-Brightness value");
    expect_true(find_publication(state, prefix + "green")->payload == "50", "Fixture green state is actual post-Brightness value");
    expect_true(find_publication(state, prefix + "blue")->payload == "25", "Fixture blue state is actual post-Brightness value");
    expect_true(find_publication(state, prefix + "color")->payload == "100;50;25", "Fixture Color state uses factual RGB values");
    expect_true(find_publication(state, prefix + "brightness")->payload == "50", "Fixture Brightness state remains current setting");
    expect_true(find_publication(state, prefix + "temperature")->payload == "100", "Fixture Temperature state remains last setting");
    expect_true(find_publication(state, prefix + "reset") == nullptr, "stateless Reset has no retained state topic");

    fixture.set_power(false);
    state = dmxwb::build_fixture_state_publications(fixture);
    expect_true(find_publication(state, prefix + "power")->payload == "0", "Fixture factual power OFF after Power command");
    expect_true(find_publication(state, prefix + "red")->payload == "0", "Fixture red factual state zero while OFF");
    expect_true(find_publication(state, prefix + "color")->payload == "0;0;0", "Fixture Color factual state zero while OFF");
    expect_true(find_publication(state, prefix + "brightness")->payload == "50", "Fixture saved Brightness survives OFF");
}

void test_internal_snapshot_publications() {
    const auto publications = dmxwb::build_internal_model_publications(
        "{\"revision\":7}",
        "{\"source\":\"mqtt\"}");
    expect_true(publications.size() == 2, "internal config/state model publications built");
    const auto* config = find_publication(publications, "/dmxwb/config");
    const auto* state = find_publication(publications, "/dmxwb/state");
    expect_true(config != nullptr && config->payload == "{\"revision\":7}" && config->retained, "canonical config payload preserved exactly");
    expect_true(state != nullptr && state->payload == "{\"source\":\"mqtt\"}" && state->retained, "canonical state payload preserved exactly");
    expect_true(find_publication(publications, "/dmxwb/status") == nullptr,
        "internal model helper cannot publish operational status");
}

}  // namespace

int main() {
    test_source_command_parser();
    test_fixture_command_parser();
    test_color_name_reset_and_retained();
    test_config_set_command_contract();
    test_fixture_retained_cleanup();
    test_command_queue_fifo();
    test_system_publications();
    test_fixture_metadata_visible();
    test_fixture_factual_state_publications();
    test_internal_snapshot_publications();

    if (failures != 0) {
        std::cerr << failures << " MQTT contract test(s) failed\n";
        return 1;
    }
    std::cout << "All DEV-007A MQTT contract tests passed\n";
    return 0;
}
