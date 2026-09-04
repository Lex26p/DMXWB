#include "dmxwb/integrated_runtime.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

std::atomic_bool g_stop_requested{false};

void handle_signal(int) noexcept {
    g_stop_requested.store(true, std::memory_order_release);
}

struct Options final {
    std::string config_path;
    std::string state_path;
    std::optional<std::uint16_t> development_oem_code;
    std::array<std::uint8_t, 6> mac{};
    bool mac_set{false};
    std::chrono::milliseconds status_interval{1000};
};

void print_usage(std::ostream& stream) {
    stream
        << "Usage:\n"
        << "  dmxwb-dev010-source-acceptance --config PATH --state PATH "
           "--development-oem-code HEX --mac XX:XX:XX:XX:XX:XX [options]\n\n"
        << "Options:\n"
        << "  --config PATH               DMXWB config.json\n"
        << "  --state PATH                DMXWB state.json\n"
        << "  --development-oem-code HEX  Explicit development-only Art-Net OEM placeholder\n"
        << "  --mac XX:XX:XX:XX:XX:XX    MAC advertised in ArtPollReply\n"
        << "  --status-interval-ms N      Periodic diagnostics interval (default 1000)\n"
        << "  --help                      Show this help\n\n"
        << "This is a DEV-010 engineering acceptance frontend over the same integrated\n"
        << "runtime used by production dmxwb. The OEM value supplied here is\n"
        << "development-only and must not be used as a production Art-Net assignment.\n";
}

[[nodiscard]] bool parse_unsigned_decimal(
    std::string_view text,
    std::uint64_t max_value,
    std::uint64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed,
        10);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > max_value) {
        return false;
    }

    value = parsed;
    return true;
}

[[nodiscard]] bool parse_hex_u16(
    std::string_view text,
    std::uint16_t& value) noexcept {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 4) {
        return false;
    }

    unsigned int parsed = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed,
        16);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > 0xffffU) {
        return false;
    }

    value = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_hex_byte(
    std::string_view text,
    std::uint8_t& value) noexcept {
    if (text.empty() || text.size() > 2) {
        return false;
    }

    unsigned int parsed = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed,
        16);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > 0xffU) {
        return false;
    }

    value = static_cast<std::uint8_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_mac(
    std::string_view text,
    std::array<std::uint8_t, 6>& mac) noexcept {
    std::size_t position = 0;
    for (std::size_t index = 0; index < mac.size(); ++index) {
        const auto separator = text.find(':', position);
        const bool last = index + 1 == mac.size();
        const auto end = last ? text.size() : separator;

        if ((!last && separator == std::string_view::npos) ||
            end == std::string_view::npos ||
            end <= position) {
            return false;
        }
        if (!parse_hex_byte(text.substr(position, end - position), mac[index])) {
            return false;
        }

        position = end + 1;
    }

    return position == text.size() + 1;
}

[[nodiscard]] std::optional<Options> parse_options(int argc, char** argv) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};

        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return std::nullopt;
        }

        const std::string_view value{argv[++index]};

        if (argument == "--config") {
            options.config_path = std::string{value};
        } else if (argument == "--state") {
            options.state_path = std::string{value};
        } else if (argument == "--development-oem-code") {
            std::uint16_t parsed = 0;
            if (!parse_hex_u16(value, parsed)) {
                std::cerr
                    << "Invalid --development-oem-code: "
                    << value << '\n';
                return std::nullopt;
            }
            options.development_oem_code = parsed;
        } else if (argument == "--mac") {
            if (!parse_mac(value, options.mac)) {
                std::cerr << "Invalid --mac: " << value << '\n';
                return std::nullopt;
            }
            options.mac_set = true;
        } else if (argument == "--status-interval-ms") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(value, 60000, parsed) ||
                parsed < 100) {
                std::cerr
                    << "Invalid --status-interval-ms: "
                    << value << '\n';
                return std::nullopt;
            }
            options.status_interval = std::chrono::milliseconds{parsed};
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return std::nullopt;
        }
    }

    if (options.config_path.empty() || options.state_path.empty()) {
        std::cerr << "--config and --state are required\n";
        return std::nullopt;
    }
    if (!options.development_oem_code.has_value()) {
        std::cerr
            << "--development-oem-code is required for DEV-010 discovery acceptance\n";
        return std::nullopt;
    }
    if (!options.mac_set) {
        std::cerr << "--mac is required for DEV-010 discovery acceptance\n";
        return std::nullopt;
    }

    return options;
}

[[nodiscard]] const char* source_state_name(
    dmxwb::ArtNetSourceState state) noexcept {
    switch (state) {
        case dmxwb::ArtNetSourceState::waiting: return "WAITING";
        case dmxwb::ArtNetSourceState::active: return "ACTIVE";
        case dmxwb::ArtNetSourceState::lost: return "LOST";
        case dmxwb::ArtNetSourceState::conflict: return "CONFLICT";
    }
    return "UNKNOWN";
}

[[nodiscard]] const char* sync_mode_name(
    dmxwb::ArtNetSyncMode mode) noexcept {
    switch (mode) {
        case dmxwb::ArtNetSyncMode::asynchronous: return "ASYNC";
        case dmxwb::ArtNetSyncMode::synchronous: return "SYNC";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view source_name(
    dmxwb::PersistedSource source) noexcept {
    return source == dmxwb::PersistedSource::mqtt ? "mqtt" : "artnet";
}

[[nodiscard]] std::string_view save_action_name(
    dmxwb::StateSaveAction action) noexcept {
    switch (action) {
        case dmxwb::StateSaveAction::not_dirty: return "not_dirty";
        case dmxwb::StateSaveAction::not_due: return "not_due";
        case dmxwb::StateSaveAction::saved: return "saved";
        case dmxwb::StateSaveAction::failed: return "failed";
    }
    return "unknown";
}

void print_status(const dmxwb::IntegratedRuntimeStatus& status) {
    std::cout
        << "status_selected_source: "
        << source_name(status.selected_source) << '\n'
        << "status_has_mqtt_snapshot: "
        << (status.has_mqtt_snapshot ? 1 : 0) << '\n'
        << "status_has_artnet_snapshot: "
        << (status.has_artnet_snapshot ? 1 : 0) << '\n'
        << "status_artnet_output_active: "
        << (status.artnet_output_active ? 1 : 0) << '\n'
        << "status_dmx_output_ever_started: "
        << (status.dmx_output_ever_started ? 1 : 0) << '\n'
        << "status_dmx_output_running: "
        << (status.dmx_output_running ? 1 : 0) << '\n'
        << "status_dmx_frames_sent: "
        << status.dmx_frames_sent << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        print_usage(std::cerr);
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        dmxwb::IntegratedRuntimeConfig runtime_config;
        runtime_config.config_path = options->config_path;
        runtime_config.state_path = options->state_path;
        runtime_config.artnet_oem_code = options->development_oem_code;
        runtime_config.artnet_mac = options->mac;
        runtime_config.artnet_port_name = "DMXWB DEV010B3";
        runtime_config.artnet_long_name =
            "DMXWB DEV-010B3 source switching acceptance";
        runtime_config.instrumentation_mode =
            dmxwb::InstrumentationMode::engineering;

        dmxwb::IntegratedRuntime runtime{std::move(runtime_config)};

        if (runtime.fixture_count() == 0) {
            std::cerr
                << "DEV-010 source acceptance requires at least one configured Fixture\n";
            return 2;
        }

        if (!runtime.start()) {
            std::cerr << runtime.last_error() << '\n';
            return 1;
        }

        std::cout
            << "dmxwb_dev010_source_acceptance: running\n"
            << "config_path: " << runtime.config_path() << '\n'
            << "state_path: " << runtime.state_path() << '\n'
            << "dmx_port: " << runtime.configured_dmx_port() << '\n'
            << "fixture_count: " << runtime.fixture_count() << '\n'
            << "artnet_port_address: "
            << runtime.configured_artnet_port_address() << '\n'
            << "initial_source: "
            << source_name(runtime.initial_source()) << '\n'
            << "development_oem_placeholder: 1\n"
            << "mqtt_runtime_connected: 1\n"
            << "artnet_runtime_connected: 1\n"
            << "source_router_connected: 1\n"
            << "physical_dmx_path_connected: 1\n"
            << "startup_artnet_without_snapshot_output_deferred: "
            << (runtime.startup_artnet_output_deferred() ? 1 : 0) << '\n'
            << "runtime_started: PASS\n";

        auto next_status = std::chrono::steady_clock::now();
        bool runtime_ok = true;

        while (!g_stop_requested.load(std::memory_order_acquire)) {
            const auto previous_dmx_port =
                std::string{runtime.applied_dmx_port()};
            const auto previous_artnet_port_address =
                runtime.applied_artnet_port_address();

            if (!runtime.step()) {
                std::cerr << runtime.last_error() << '\n';
                runtime_ok = false;
                break;
            }

            if (runtime.applied_dmx_port() != previous_dmx_port) {
                std::cout
                    << "runtime_dmx_port_applied: "
                    << runtime.applied_dmx_port() << '\n';
            }
            if (runtime.applied_artnet_port_address() !=
                previous_artnet_port_address) {
                std::cout
                    << "runtime_artnet_port_address_applied: "
                    << runtime.applied_artnet_port_address() << '\n';
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_status) {
                print_status(runtime.status());
                next_status = now + options->status_interval;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        const auto flush_result = runtime.shutdown();
        const auto diag = runtime.diagnostics();

        std::cout
            << "final_selected_source: "
            << source_name(diag.router.selected_source) << '\n'
            << "final_mqtt_successful_connections: "
            << diag.mqtt.successful_connections << '\n'
            << "final_mqtt_disconnects: "
            << diag.mqtt.disconnects << '\n'
            << "final_mqtt_callback_failures: "
            << diag.mqtt.callback_failures << '\n'
            << "final_mqtt_runtime_commands_processed: "
            << diag.mqtt_runtime.commands_processed << '\n'
            << "final_mqtt_runtime_dmx_publish_failures: "
            << diag.mqtt_runtime.dmx_publish_failures << '\n'
            << "final_mqtt_runtime_state_save_failures: "
            << diag.mqtt_runtime.state_save_failures << '\n'
            << "final_artnet_source_state: "
            << source_state_name(diag.artnet_source_state) << '\n'
            << "final_artnet_sync_mode: "
            << sync_mode_name(diag.artnet_sync_mode) << '\n'
            << "final_artnet_bind_attempts: "
            << diag.artnet.bind_attempts << '\n'
            << "final_artnet_bind_failures: "
            << diag.artnet.bind_failures << '\n'
            << "final_artnet_transport_recoveries: "
            << diag.artnet.transport_recoveries << '\n'
            << "final_artnet_datagrams_received: "
            << diag.artnet.datagrams_received << '\n'
            << "final_artnet_receive_errors: "
            << diag.artnet.receive_errors << '\n'
            << "final_artnet_send_errors: "
            << diag.artnet.send_errors << '\n'
            << "final_artnet_core_rejections: "
            << diag.artnet.core_rejections << '\n'
            << "final_artnet_conflicts: "
            << diag.artnet.conflicts << '\n'
            << "final_artnet_source_lost_events: "
            << diag.artnet.source_lost_events << '\n'
            << "final_artnet_snapshots_published: "
            << diag.artnet.snapshots_published << '\n'
            << "final_artnet_poll_replies_sent: "
            << diag.artnet.poll_replies_sent << '\n'
            << "final_artnet_route_failures: "
            << diag.artnet_source.route_failures << '\n'
            << "final_router_mqtt_snapshots_received: "
            << diag.router.mqtt_snapshots_received << '\n'
            << "final_router_artnet_snapshots_received: "
            << diag.router.artnet_snapshots_received << '\n'
            << "final_router_source_switches: "
            << diag.router.source_switches << '\n'
            << "final_router_source_switches_without_snapshot: "
            << diag.router.source_switches_without_snapshot << '\n'
            << "final_router_physical_snapshots_published: "
            << diag.router.physical_snapshots_published << '\n'
            << "final_router_physical_publish_failures: "
            << diag.router.physical_publish_failures << '\n'
            << "final_router_artnet_output_active: "
            << (diag.router.artnet_output_active ? 1 : 0) << '\n'
            << "final_dmx_sink_ever_started: "
            << (diag.dmx_sink_ever_started ? 1 : 0) << '\n'
            << "final_dmx_sink_start_failures: "
            << diag.dmx_sink_start_failures << '\n'
            << "final_dmx_sink_publish_failures: "
            << diag.dmx_sink_publish_failures << '\n'
            << "final_dmx_sink_unexpected_stops: "
            << diag.dmx_sink_unexpected_stops << '\n'
            << "final_dmx_port_reconfigurations: "
            << diag.dmx_port_reconfigurations << '\n'
            << "final_dmx_port_reconfigure_failures: "
            << diag.dmx_port_reconfigure_failures << '\n'
            << "final_artnet_universe_reconfigurations: "
            << diag.artnet_universe_reconfigurations << '\n'
            << "final_artnet_universe_reconfigure_failures: "
            << diag.artnet_universe_reconfigure_failures << '\n'
            << "final_applied_dmx_port: "
            << diag.applied_dmx_port << '\n'
            << "final_applied_artnet_port_address: "
            << diag.applied_artnet_port_address << '\n'
            << "final_dmx_frames_sent: "
            << diag.dmx.frames_sent << '\n'
            << "final_dmx_open_failures: "
            << diag.dmx.open_failures << '\n'
            << "final_dmx_send_failures: "
            << diag.dmx.send_failures << '\n'
            << "final_dmx_recoveries: "
            << diag.dmx.recoveries << '\n'
            << "final_dmx_missed_deadlines: "
            << diag.dmx.missed_deadlines << '\n'
            << "final_dmx_active_refresh_hz: "
            << diag.dmx.active_refresh_hz << '\n'
            << "final_dmx_serial_open_after_stop: "
            << (diag.dmx.serial_open ? 1 : 0) << '\n'
            << "final_artnet_transport_open_before_shutdown: "
            << (diag.artnet_transport_open_before_shutdown ? 1 : 0) << '\n'
            << "final_artnet_transport_open_after_shutdown: "
            << (diag.artnet_transport_open_after_shutdown ? 1 : 0) << '\n'
            << "state_flush_action: "
            << save_action_name(flush_result.action) << '\n';

        const bool dmx_ok =
            (!diag.dmx_sink_ever_started || diag.dmx.frames_sent > 0) &&
            diag.dmx_sink_start_failures == 0 &&
            diag.dmx_sink_publish_failures == 0 &&
            diag.dmx_sink_unexpected_stops == 0 &&
            diag.dmx_port_reconfigure_failures == 0 &&
            diag.artnet_universe_reconfigure_failures == 0 &&
            diag.dmx.missed_deadlines == 0 &&
            diag.dmx.active_refresh_hz == dmxwb::kDmxOutputRefreshHz &&
            !diag.dmx.serial_open;

        const bool pass =
            runtime_ok &&
            flush_result.ok() &&
            diag.mqtt.successful_connections >= 1 &&
            diag.mqtt.callback_failures == 0 &&
            diag.mqtt_runtime.dmx_publish_failures == 0 &&
            diag.mqtt_runtime.state_save_failures == 0 &&
            diag.artnet_source.route_failures == 0 &&
            diag.router.physical_publish_failures == 0 &&
            !diag.artnet_transport_open_after_shutdown &&
            dmx_ok;

        std::cout
            << "software_result: "
            << (pass ? "PASS" : "FAIL") << '\n';
        return pass ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr
            << "DEV-010 source acceptance startup failed: "
            << error.what() << '\n';
        return 1;
    }
}
