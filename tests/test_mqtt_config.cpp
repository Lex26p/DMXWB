#include "dmxwb/mqtt_config.hpp"

#include <iostream>
#include <string>
#include <string_view>

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

[[nodiscard]] dmxwb::AppConfig make_config() {
    auto config = dmxwb::make_default_config();
    config.revision = 7;
    config.fixture_count = 1;
    config.start_address = 5;
    config.fixtures = {dmxwb::FixtureConfigRecord{10, "Fixture 10"}};
    config.id_counters.next_fixture_id = 11;
    return config;
}

[[nodiscard]] std::string make_request(
    std::string_view request_id_json,
    std::string_view expected_revision_json,
    const dmxwb::AppConfig& config) {
    std::string output{"{\"request_id\":"};
    output += request_id_json;
    output += ",\"expected_revision\":";
    output += expected_revision_json;
    output += ",\"config\":";
    output += dmxwb::serialize_config_json(config);
    output += '}';
    return output;
}

void test_valid_request() {
    const auto config = make_config();
    const auto parsed = dmxwb::parse_mqtt_config_set_request(
        make_request("\"req-42\"", "7", config));
    expect_true(parsed.ok(), "valid config/set envelope parses");
    if (!parsed.ok()) return;
    expect_true(parsed.request->request_id == "req-42", "request_id preserved");
    expect_true(parsed.request->expected_revision == 7, "expected_revision preserved exactly");
    expect_true(parsed.request->proposed_config == config, "nested canonical config parsed without semantic loss");
}

void test_escaped_request_id_and_field_order() {
    const auto config = make_config();
    std::string payload{"{\"config\":"};
    payload += dmxwb::serialize_config_json(config);
    payload += ",\"request_id\":\"web-\\u0031-\\\"x\\\"\",\"expected_revision\":7}";
    const auto parsed = dmxwb::parse_mqtt_config_set_request(payload);
    expect_true(parsed.ok(), "config/set field order is irrelevant");
    expect_true(parsed.ok() && parsed.request->request_id == "web-1-\"x\"",
        "request_id JSON escapes decode correctly");
}

void test_invalid_envelopes() {
    const auto config = make_config();

    auto parsed = dmxwb::parse_mqtt_config_set_request("{}");
    expect_true(!parsed.ok() && parsed.error_code == "invalid_request", "empty envelope rejected");

    auto extra = make_request("\"req-extra\"", "7", config);
    extra.pop_back();
    extra += ",\"unknown\":1}";
    parsed = dmxwb::parse_mqtt_config_set_request(extra);
    expect_true(!parsed.ok() && parsed.error_code == "invalid_request", "unknown envelope field rejected");
    expect_true(parsed.request_id == "req-extra", "envelope schema error preserves parsed request_id when available");

    parsed = dmxwb::parse_mqtt_config_set_request(make_request("\"req-fraction\"", "7.0", config));
    expect_true(!parsed.ok() && parsed.request_id == "req-fraction", "fractional expected_revision rejected with request_id retained");

    parsed = dmxwb::parse_mqtt_config_set_request(make_request("\"req-revision-body\"", "6", config));
    expect_true(!parsed.ok() && parsed.error_code == "revision_conflict",
        "nested config revision must match expected_revision");

    parsed = dmxwb::parse_mqtt_config_set_request(make_request("\"\"", "7", config));
    expect_true(!parsed.ok() && parsed.error_code == "invalid_request", "empty request_id rejected");

    std::string invalid_utf8_request{"{\"request_id\":\""};
    invalid_utf8_request.push_back(static_cast<char>(0xC3));
    invalid_utf8_request.push_back('(');
    invalid_utf8_request += "\",\"expected_revision\":7,\"config\":";
    invalid_utf8_request += dmxwb::serialize_config_json(config);
    invalid_utf8_request += '}';
    parsed = dmxwb::parse_mqtt_config_set_request(invalid_utf8_request);
    expect_true(!parsed.ok() && parsed.error_code == "invalid_request", "invalid UTF-8 request_id rejected");
}

void test_invalid_nested_config() {
    auto config = make_config();
    config.start_address = 300;
    const auto parsed = dmxwb::parse_mqtt_config_set_request(
        make_request("\"req-invalid-config\"", "7", config));
    expect_true(!parsed.ok(), "invalid nested config rejected before transaction");
    expect_true(parsed.request_id == "req-invalid-config", "nested config error keeps request_id for result correlation");
    expect_true(parsed.error_code == "validation", "nested config validation error has stable error_code");
}

void test_result_publication() {
    const auto publication = dmxwb::build_mqtt_config_result_publication(
        "req-\"7\"",
        false,
        9,
        "revision_conflict",
        "expected revision mismatch");
    expect_true(publication.topic == dmxwb::kMqttConfigResultTopic, "config result uses canonical topic");
    expect_true(!publication.retained, "config result is non-retained");
    expect_true(publication.payload.find("\"request_id\":\"req-\\\"7\\\"\"") != std::string::npos,
        "config result JSON-escapes request_id");
    expect_true(publication.payload.find("\"ok\":false") != std::string::npos,
        "config result includes failure flag");
    expect_true(publication.payload.find("\"revision\":9") != std::string::npos,
        "config result includes current revision");
    expect_true(publication.payload.find("\"error_code\":\"revision_conflict\"") != std::string::npos,
        "config result includes stable error code");
}

}  // namespace

int main() {
    test_valid_request();
    test_escaped_request_id_and_field_order();
    test_invalid_envelopes();
    test_invalid_nested_config();
    test_result_publication();

    if (failures != 0) {
        std::cerr << failures << " MQTT config API test(s) failed\n";
        return 1;
    }
    std::cout << "All MQTT config API tests passed\n";
    return 0;
}
