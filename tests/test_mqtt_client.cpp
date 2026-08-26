#include "dmxwb/mqtt_client.hpp"

#include <mosquitto.h>

#include <iostream>
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
}  // namespace

int main() {
    dmxwb::MqttCommandQueue queue;
    dmxwb::MqttClient client{queue};

    expect_true(
        mosquitto_sub_topic_check(dmxwb::kMqttDeviceCommandSubscription.data()) == MOSQ_ERR_SUCCESS,
        "DMXWB command subscription is valid MQTT syntax");
    expect_true(
        mosquitto_sub_topic_check("/devices/dmxwb_fixture_+/controls/+/on") != MOSQ_ERR_SUCCESS,
        "partial-level wildcard regression is rejected by libmosquitto");
    expect_true(
        mosquitto_sub_topic_check(dmxwb::kMqttConfigSetTopic.data()) == MOSQ_ERR_SUCCESS,
        "config/set exact subscription is valid MQTT syntax");

    expect_true(!client.running(), "MQTT client initially stopped");
    expect_true(!client.connected(), "MQTT client initially disconnected");
    expect_true(!client.take_full_republish_request(), "no reconnect republish before connection");

    const dmxwb::MqttPublication publication{"/test", "value", true};
    expect_true(!client.publish(publication), "publish is rejected while disconnected");
    const auto diagnostics = client.diagnostics();
    expect_true(diagnostics.publish_failures == 1, "disconnected publish is counted");

    if (failures != 0) {
        std::cerr << failures << " MQTT client smoke test(s) failed\n";
        return 1;
    }
    std::cout << "DEV-007B libmosquitto client smoke tests passed\n";
    return 0;
}
