#include "dmxwb/integrated_runtime.hpp"

#include <chrono>
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

[[nodiscard]] bool contains(std::string_view text, std::string_view token) {
    return text.find(token) != std::string_view::npos;
}

void expect_no_cumulative_telemetry(std::string_view json) {
    constexpr std::string_view forbidden[] = {
        "frames_sent",
        "deadlines_missed",
        "packets_received",
        "datagrams_received",
        "commands_processed",
        "commands_rejected",
        "publications",
        "republishes",
        "snapshots_published",
        "snapshots_routed",
        "snapshots_superseded",
        "source_switches",
        "successful_connections",
        "disconnects",
        "open_failures",
        "send_failures",
        "publish_failures",
        "recoveries",
    };

    for (const auto token : forbidden) {
        expect_true(!contains(json, token),
            std::string{"production status excludes cumulative field "}.append(token));
    }
}

[[nodiscard]] dmxwb::IntegratedRuntimeOperationalState make_running_state() {
    dmxwb::IntegratedRuntimeOperationalState state;
    state.started = true;
    state.selected_source = dmxwb::PersistedSource::mqtt;
    state.has_mqtt_snapshot = true;
    state.dmx_output_ever_started = true;
    state.dmx.running = true;
    state.dmx.serial_open = true;
    state.dmx.slot_count = 300;
    state.dmx.active_generation = 42;
    state.dmx.refresh_hz = 44;
    state.mqtt.running = true;
    state.mqtt.connected = true;
    state.artnet.transport_open = true;
    state.artnet.source_state = dmxwb::ArtNetSourceState::waiting;
    state.configuration_ok = true;
    state.configuration_revision = 7;
    state.applied_dmx_port = "/dev/ttyRS485-1";
    state.applied_artnet_port_address = 17;
    state.config_path = "/etc/dmxwb/config.json";
    state.state_path = "/var/lib/dmxwb/state.json";
    return state;
}

void test_running_mqtt_status_is_factual() {
    const auto payload = dmxwb::build_operational_status_payload(make_running_state(), false);

    expect_true(payload.application == dmxwb::MqttApplicationStatus::running,
        "healthy MQTT runtime reports running application");
    expect_true(contains(payload.json, R"("application":"running")"),
        "status has compatible application field");
    expect_true(contains(payload.json, R"("selected_source":"mqtt")"),
        "status reports current selected source");
    expect_true(contains(payload.json, R"("state":"running","port":"/dev/ttyRS485-1")"),
        "status reports current DMX state and port");
    expect_true(contains(payload.json, R"("slot_count":300,"refresh_hz":44,"physical_slot_limit":300)"),
        "status reports factual physical DMX contract");
    expect_true(contains(payload.json, R"("state":"connected","connected":true)"),
        "status reports current MQTT connection");
    expect_true(contains(payload.json, R"("revision":7,"dmx_port":"/dev/ttyRS485-1","artnet_universe":17)"),
        "status reports applied configuration and revision");
    expect_no_cumulative_telemetry(payload.json);
}

void test_artnet_conflict_status_identifies_sources() {
    auto state = make_running_state();
    state.selected_source = dmxwb::PersistedSource::artnet;
    state.artnet_output_active = false;
    state.artnet.source_state = dmxwb::ArtNetSourceState::conflict;
    state.artnet.sync_mode = dmxwb::ArtNetSyncMode::synchronous;
    state.artnet.active_source = dmxwb::ArtNetSource{{{192, 168, 1, 20}}, 1};
    state.artnet.conflicting_source = dmxwb::ArtNetSource{{{192, 168, 1, 21}}, 2};
    state.artnet.last_sequence = 91;
    state.artnet.last_packet_age = std::chrono::milliseconds{125};
    state.artnet.last_sync_age = std::chrono::milliseconds{30};
    state.artnet.committed_revision = 12;

    const auto payload = dmxwb::build_operational_status_payload(state, false);

    expect_true(payload.application == dmxwb::MqttApplicationStatus::error,
        "selected Art-Net conflict reports application error");
    expect_true(contains(payload.json, R"("active_source_ip":"192.168.1.20")"),
        "status identifies active Art-Net source");
    expect_true(contains(payload.json, R"("conflicting_source_ip":"192.168.1.21")"),
        "status identifies conflicting Art-Net source");
    expect_true(contains(payload.json, R"("last_packet_age_ms":125,"last_sequence":91,"sync_mode":"SYNC")"),
        "status reports current Art-Net timing and protocol state");
    expect_true(contains(payload.json, R"("output_mode":"hold_last")"),
        "inactive conflicting Art-Net input reports Hold Last");
    expect_true(contains(payload.json, R"("last_error":"Art-Net source conflict")"),
        "status reports current Art-Net conflict error");
    expect_no_cumulative_telemetry(payload.json);
}

void test_artnet_live_requires_operational_physical_output() {
    auto state = make_running_state();
    state.selected_source = dmxwb::PersistedSource::artnet;
    state.artnet_output_active = true;
    state.artnet.source_state = dmxwb::ArtNetSourceState::active;
    state.dmx.serial_open = false;
    state.dmx.last_error = "serial unavailable";

    auto payload = dmxwb::build_operational_status_payload(state, false);
    expect_true(payload.application == dmxwb::MqttApplicationStatus::error,
        "closed physical serial reports application error for selected Art-Net");
    expect_true(contains(payload.json, R"("output_mode":"hold_last")"),
        "accepted Art-Net snapshot is not live while physical serial is closed");

    state.dmx.serial_open = true;
    state.dmx.last_error.clear();
    payload = dmxwb::build_operational_status_payload(state, false);
    expect_true(payload.application == dmxwb::MqttApplicationStatus::running,
        "physical serial recovery restores running Art-Net application");
    expect_true(contains(payload.json, R"("output_mode":"live")"),
        "Art-Net becomes live only after physical DMX is operational");
}

void test_stopping_status_is_off() {
    const auto payload = dmxwb::build_operational_status_payload(make_running_state(), true);

    expect_true(payload.application == dmxwb::MqttApplicationStatus::off,
        "stopping runtime reports application off");
    expect_true(contains(payload.json, R"("application":"off","dmx":"off","mqtt":"offline","artnet":"off")"),
        "stopping status reports all top-level runtime surfaces off");
    expect_true(contains(payload.json, R"("last_error":"")"),
        "clean stopping status has no top-level error");
    expect_no_cumulative_telemetry(payload.json);
}

void test_persistence_error_and_recovery_status_are_factual() {
    auto state = make_running_state();
    state.configuration_ok = false;
    state.persistence_last_error = "state write failed";
    auto payload = dmxwb::build_operational_status_payload(state, false);

    expect_true(payload.application == dmxwb::MqttApplicationStatus::error,
        "current persistence failure reports application error");
    expect_true(contains(payload.json, R"("configuration":"error")"),
        "current persistence failure is distinguished from startup fallback");
    expect_true(contains(payload.json, R"("last_error":"state write failed")"),
        "status exposes current persistence error");
    expect_true(contains(payload.json, R"("recovery_state":"error")"),
        "configuration diagnostics report persistence recovery state");

    state.configuration_ok = true;
    state.persistence_last_error.clear();
    payload = dmxwb::build_operational_status_payload(state, false);
    expect_true(payload.application == dmxwb::MqttApplicationStatus::running,
        "successful persistence recovery returns application to running");
    expect_true(contains(payload.json, R"("configuration":"ok")") &&
                    contains(payload.json, R"("recovery_state":"ok")"),
        "successful persistence recovery returns status to factual ok");
}

}  // namespace

int main() {
    test_running_mqtt_status_is_factual();
    test_artnet_conflict_status_identifies_sources();
    test_artnet_live_requires_operational_physical_output();
    test_stopping_status_is_off();
    test_persistence_error_and_recovery_status_are_factual();

    if (failures != 0) {
        std::cerr << failures << " operational status test(s) failed\n";
        return 1;
    }

    std::cout << "All operational status tests passed\n";
    return 0;
}
