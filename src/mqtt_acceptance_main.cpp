#include "dmxwb/dmx_output.hpp"
#include "dmxwb/mqtt_client.hpp"
#include "dmxwb/mqtt_runtime.hpp"
#include "dmxwb/persistence_runtime.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) {
    g_stop_requested.store(true, std::memory_order_release);
}

struct Options final {
    std::string config_path;
    std::string state_path;
};

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  dmxwb-mqtt-acceptance --config PATH --state PATH\n";
}

[[nodiscard]] std::optional<Options> parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--config" && index + 1 < argc) {
            options.config_path = argv[++index];
        } else if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (options.config_path.empty() || options.state_path.empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] std::string_view save_action_name(dmxwb::StateSaveAction action) noexcept {
    switch (action) {
        case dmxwb::StateSaveAction::not_dirty: return "not_dirty";
        case dmxwb::StateSaveAction::not_due: return "not_due";
        case dmxwb::StateSaveAction::saved: return "saved";
        case dmxwb::StateSaveAction::failed: return "failed";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        print_usage();
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    dmxwb::PersistenceRuntime persistence{options->config_path, options->state_path};
    if (persistence.config().fixture_count == 0) {
        std::cerr << "DEV-007 acceptance requires at least one configured Fixture\n";
        return 2;
    }

    dmxwb::MqttCommandQueue command_queue;
    dmxwb::MqttController controller{persistence};
    dmxwb::MqttClient mqtt{command_queue};
    dmxwb::DmxOutput output{dmxwb::DmxOutputConfig{
        persistence.config().dmx_port,
        std::chrono::milliseconds{250}}};
    dmxwb::DmxSourceRouter dmx_router{
        persistence.source(),
        [&output](const dmxwb::DmxSnapshot& snapshot) {
            return output.publish_snapshot(snapshot);
        }};
    dmxwb::MqttRuntimeCoordinator runtime{
        persistence,
        command_queue,
        controller,
        mqtt,
        dmx_router};

    if (!runtime.publish_initial_snapshot()) {
        std::cerr << "Cannot route initial MQTT DMX snapshot\n";
        return 1;
    }
    if (!output.start()) {
        std::cerr << "Cannot start DmxOutput\n";
        return 1;
    }
    if (!mqtt.start()) {
        output.stop();
        std::cerr << "Cannot start MQTT client\n";
        return 1;
    }

    std::cout << "DEV-007 MQTT acceptance runtime\n"
              << "  config_path: " << options->config_path << '\n'
              << "  state_path: " << options->state_path << '\n'
              << "  dmx_port: " << persistence.config().dmx_port << '\n'
              << "  fixture_count: " << persistence.config().fixture_count << '\n'
              << "  source: " << dmxwb::persisted_source_name(persistence.source()) << '\n'
              << "runtime_started: PASS\n";
    std::cout.flush();

    while (!g_stop_requested.load(std::memory_order_acquire)) {
        runtime.step(dmxwb::StatePersistenceManager::clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    const auto flush_result = runtime.flush_state();
    mqtt.stop();
    output.stop();

    const auto mqtt_diag = mqtt.diagnostics();
    const auto dmx_diag = output.diagnostics();
    const auto& runtime_diag = runtime.diagnostics();
    const auto router_diag = dmx_router.diagnostics();

    std::cout
        << "mqtt_successful_connections: " << mqtt_diag.successful_connections << '\n'
        << "mqtt_disconnects: " << mqtt_diag.disconnects << '\n'
        << "mqtt_commands_accepted: " << mqtt_diag.commands_accepted << '\n'
        << "mqtt_commands_ignored: " << mqtt_diag.commands_ignored << '\n'
        << "mqtt_commands_rejected: " << mqtt_diag.commands_rejected << '\n'
        << "mqtt_publish_failures: " << mqtt_diag.publish_failures << '\n'
        << "mqtt_callback_failures: " << mqtt_diag.callback_failures << '\n'
        << "runtime_commands_processed: " << runtime_diag.commands_processed << '\n'
        << "runtime_commands_rejected: " << runtime_diag.commands_rejected << '\n'
        << "runtime_dmx_snapshots_published: " << runtime_diag.dmx_snapshots_published << '\n'
        << "runtime_dmx_publish_failures: " << runtime_diag.dmx_publish_failures << '\n'
        << "runtime_full_republishes: " << runtime_diag.full_republishes << '\n'
        << "runtime_mqtt_publish_failures: " << runtime_diag.mqtt_publish_failures << '\n'
        << "runtime_state_save_failures: " << runtime_diag.state_save_failures << '\n'
        << "source_router_mqtt_snapshots_received: " << router_diag.mqtt_snapshots_received << '\n'
        << "source_router_artnet_snapshots_received: " << router_diag.artnet_snapshots_received << '\n'
        << "source_router_source_switches: " << router_diag.source_switches << '\n'
        << "source_router_source_switches_without_snapshot: "
        << router_diag.source_switches_without_snapshot << '\n'
        << "source_router_physical_snapshots_published: "
        << router_diag.physical_snapshots_published << '\n'
        << "source_router_physical_publish_failures: "
        << router_diag.physical_publish_failures << '\n'
        << "source_router_artnet_output_active: "
        << (router_diag.artnet_output_active ? 1 : 0) << '\n'
        << "state_flush_action: " << save_action_name(flush_result.action) << '\n'
        << "dmx_frames_sent: " << dmx_diag.frames_sent << '\n'
        << "dmx_open_failures: " << dmx_diag.open_failures << '\n'
        << "dmx_send_failures: " << dmx_diag.send_failures << '\n'
        << "dmx_recoveries: " << dmx_diag.recoveries << '\n'
        << "dmx_missed_deadlines: " << dmx_diag.missed_deadlines << '\n'
        << "dmx_active_refresh_hz: " << dmx_diag.active_refresh_hz << '\n'
        << "dmx_serial_open_after_stop: " << (dmx_diag.serial_open ? 1 : 0) << '\n';

    const bool pass =
        flush_result.ok() &&
        mqtt_diag.successful_connections >= 1 &&
        mqtt_diag.callback_failures == 0 &&
        runtime_diag.dmx_publish_failures == 0 &&
        runtime_diag.state_save_failures == 0 &&
        router_diag.physical_publish_failures == 0 &&
        dmx_diag.frames_sent > 0 &&
        dmx_diag.open_failures == 0 &&
        dmx_diag.send_failures == 0 &&
        dmx_diag.recoveries == 0 &&
        dmx_diag.missed_deadlines == 0 &&
        dmx_diag.active_refresh_hz == dmxwb::kDmxOutputRefreshHz &&
        !dmx_diag.serial_open;

    std::cout << "software_result: " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
