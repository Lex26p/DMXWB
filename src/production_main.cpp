#include "dmxwb/app_info.hpp"
#include "dmxwb/integrated_runtime.hpp"
#include "dmxwb/persistence_storage.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

std::atomic_bool g_stop_requested{false};

void handle_signal(int) noexcept {
    g_stop_requested.store(true, std::memory_order_release);
}

struct Options final {
    std::string config_path{dmxwb::kDefaultConfigPath};
    std::string state_path{dmxwb::kDefaultStatePath};
};

struct RuntimeLogState final {
    bool initialized{false};
    bool mqtt_connected{false};
    bool mqtt_ever_connected{false};
    bool dmx_serial_open{false};
    bool dmx_ever_ready{false};
    bool dmx_error_active{false};
    bool artnet_error_active{false};
    dmxwb::ArtNetSourceState artnet_source_state{dmxwb::ArtNetSourceState::waiting};
    std::uint64_t dmx_failures{0};
    std::uint64_t artnet_errors{0};
    std::uint64_t artnet_transport_recoveries{0};
    std::string applied_dmx_port;
    std::uint16_t applied_artnet_universe{0};
};

void print_help() {
    std::cout
        << "Usage:\n"
        << "  dmxwb [--config PATH] [--state PATH]\n"
        << "  dmxwb --help\n"
        << "  dmxwb --version\n\n"
        << "Production foreground daemon.\n"
        << "Defaults:\n"
        << "  --config " << dmxwb::kDefaultConfigPath << '\n'
        << "  --state  " << dmxwb::kDefaultStatePath << '\n';
}

[[nodiscard]] std::optional<Options> parse_options(int argc, char* argv[]) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};

        if (argument == "--help" || argument == "-h") {
            print_help();
            std::exit(0);
        }
        if (argument == "--version") {
            std::cout
                << dmxwb::application_name() << ' '
                << dmxwb::application_version() << '\n';
            std::exit(0);
        }

        if (argument != "--config" && argument != "--state") {
            std::cerr << "Unknown option: " << argument << '\n';
            return std::nullopt;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return std::nullopt;
        }

        const std::string_view value{argv[++index]};
        if (value.empty()) {
            std::cerr << argument << " must not be empty\n";
            return std::nullopt;
        }

        if (argument == "--config") {
            options.config_path = std::string{value};
        } else {
            options.state_path = std::string{value};
        }
    }

    return options;
}

[[nodiscard]] std::string_view artnet_source_state_name(dmxwb::ArtNetSourceState state) noexcept {
    switch (state) {
        case dmxwb::ArtNetSourceState::waiting: return "WAITING";
        case dmxwb::ArtNetSourceState::active: return "ACTIVE";
        case dmxwb::ArtNetSourceState::lost: return "LOST";
        case dmxwb::ArtNetSourceState::conflict: return "CONFLICT";
    }
    return "WAITING";
}

void initialize_runtime_log_state(
    dmxwb::IntegratedRuntime& runtime,
    RuntimeLogState& state) {
    const auto diag = runtime.diagnostics();
    state.initialized = true;
    state.mqtt_connected = diag.mqtt.connected;
    state.mqtt_ever_connected = diag.mqtt.connected;
    state.dmx_serial_open = diag.dmx.serial_open;
    state.dmx_ever_ready = diag.dmx.serial_open;
    state.artnet_source_state = diag.artnet_source_state;
    state.dmx_failures = diag.dmx.open_failures + diag.dmx.send_failures;
    state.artnet_errors =
        diag.artnet.bind_failures + diag.artnet.receive_errors + diag.artnet.send_errors;
    state.artnet_transport_recoveries = diag.artnet.transport_recoveries;
    state.applied_dmx_port = std::string{runtime.applied_dmx_port()};
    state.applied_artnet_universe = runtime.applied_artnet_port_address();

    if (state.mqtt_connected) {
        std::cout << "dmxwb event=mqtt_connected\n";
    }
    if (state.dmx_serial_open) {
        std::cout << "dmxwb event=dmx_ready port="
                  << std::quoted(state.applied_dmx_port) << '\n';
    }
    if (state.artnet_source_state != dmxwb::ArtNetSourceState::waiting) {
        std::cout << "dmxwb event=artnet_source state="
                  << artnet_source_state_name(state.artnet_source_state) << '\n';
    }
}

void log_runtime_events(
    dmxwb::IntegratedRuntime& runtime,
    RuntimeLogState& state) {
    if (!state.initialized) {
        initialize_runtime_log_state(runtime, state);
        return;
    }

    const auto diag = runtime.diagnostics();

    if (diag.mqtt.connected != state.mqtt_connected) {
        if (diag.mqtt.connected) {
            std::cout << "dmxwb event="
                      << (state.mqtt_ever_connected ? "mqtt_recovered" : "mqtt_connected")
                      << " connections=" << diag.mqtt.successful_connections << '\n';
            state.mqtt_ever_connected = true;
        } else if (state.mqtt_connected) {
            std::cerr << "dmxwb event=mqtt_lost disconnects="
                      << diag.mqtt.disconnects;
            if (!diag.mqtt.last_error.empty()) {
                std::cerr << " error=" << std::quoted(diag.mqtt.last_error);
            }
            std::cerr << '\n';
        }
        state.mqtt_connected = diag.mqtt.connected;
    }

    const auto dmx_failures = diag.dmx.open_failures + diag.dmx.send_failures;
    if (dmx_failures > state.dmx_failures && !state.dmx_error_active) {
        std::cerr << "dmxwb event=dmx_error open_failures=" << diag.dmx.open_failures
                  << " send_failures=" << diag.dmx.send_failures;
        if (!diag.dmx.last_error.empty()) {
            std::cerr << " error=" << std::quoted(diag.dmx.last_error);
        }
        std::cerr << '\n';
        state.dmx_error_active = true;
    }
    state.dmx_failures = dmx_failures;

    if (diag.dmx.serial_open != state.dmx_serial_open) {
        if (diag.dmx.serial_open) {
            std::cout << "dmxwb event="
                      << (state.dmx_ever_ready ? "dmx_recovered" : "dmx_ready")
                      << " port=" << std::quoted(std::string{runtime.applied_dmx_port()})
                      << " recoveries=" << diag.dmx.recoveries << '\n';
            state.dmx_ever_ready = true;
            state.dmx_error_active = false;
        } else if (state.dmx_serial_open) {
            std::cerr << "dmxwb event=dmx_lost port="
                      << std::quoted(std::string{runtime.applied_dmx_port()});
            if (!diag.dmx.last_error.empty()) {
                std::cerr << " error=" << std::quoted(diag.dmx.last_error);
            }
            std::cerr << '\n';
            state.dmx_error_active = true;
        }
        state.dmx_serial_open = diag.dmx.serial_open;
    }

    if (diag.artnet_source_state != state.artnet_source_state) {
        const auto name = artnet_source_state_name(diag.artnet_source_state);
        if (diag.artnet_source_state == dmxwb::ArtNetSourceState::lost ||
            diag.artnet_source_state == dmxwb::ArtNetSourceState::conflict) {
            std::cerr << "dmxwb event=artnet_source state=" << name << '\n';
        } else {
            std::cout << "dmxwb event=artnet_source state=" << name << '\n';
        }
        state.artnet_source_state = diag.artnet_source_state;
    }

    const auto artnet_errors =
        diag.artnet.bind_failures + diag.artnet.receive_errors + diag.artnet.send_errors;
    if (artnet_errors > state.artnet_errors && !state.artnet_error_active) {
        std::cerr << "dmxwb event=artnet_error bind_failures=" << diag.artnet.bind_failures
                  << " receive_errors=" << diag.artnet.receive_errors
                  << " send_errors=" << diag.artnet.send_errors << '\n';
        state.artnet_error_active = true;
    }
    state.artnet_errors = artnet_errors;

    if (diag.artnet.transport_recoveries > state.artnet_transport_recoveries) {
        std::cout << "dmxwb event=artnet_recovered recoveries="
                  << diag.artnet.transport_recoveries << '\n';
        state.artnet_error_active = false;
    }
    state.artnet_transport_recoveries = diag.artnet.transport_recoveries;

    const std::string current_dmx_port{runtime.applied_dmx_port()};
    const auto current_artnet_universe = runtime.applied_artnet_port_address();
    if (current_dmx_port != state.applied_dmx_port ||
        current_artnet_universe != state.applied_artnet_universe) {
        std::cout << "dmxwb event=config_applied dmx_port="
                  << std::quoted(current_dmx_port)
                  << " artnet_universe=" << current_artnet_universe << '\n';
        state.applied_dmx_port = current_dmx_port;
        state.applied_artnet_universe = current_artnet_universe;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        print_help();
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        dmxwb::IntegratedRuntimeConfig runtime_config;
        runtime_config.config_path = options->config_path;
        runtime_config.state_path = options->state_path;
        // A registered production Art-Net OEM identity is a release identity
        // input. Until it is assigned, ArtDmx input remains active while
        // ArtPollReply advertisement stays disabled rather than inventing a code.
        runtime_config.artnet_oem_code.reset();
        runtime_config.artnet_port_name = "DMXWB";
        runtime_config.artnet_long_name = "DMXWB Art-Net input";

        dmxwb::IntegratedRuntime runtime{std::move(runtime_config)};
        if (!runtime.start()) {
            std::cerr << "dmxwb event=startup_failed error="
                      << std::quoted(std::string{runtime.last_error()}) << '\n';
            return 1;
        }

        std::cout
            << "dmxwb event=startup version=" << dmxwb::application_version()
            << " config=" << std::quoted(std::string{runtime.config_path()})
            << " state=" << std::quoted(std::string{runtime.state_path()})
            << " source=" << dmxwb::persisted_source_name(runtime.initial_source())
            << " dmx_port=" << std::quoted(std::string{runtime.applied_dmx_port()})
            << " artnet_universe=" << runtime.applied_artnet_port_address() << '\n'
            << "dmxwb: running\n"
            << "config_path: " << runtime.config_path() << '\n'
            << "state_path: " << runtime.state_path() << '\n';

        RuntimeLogState log_state;
        initialize_runtime_log_state(runtime, log_state);

        bool runtime_ok = true;
        while (!g_stop_requested.load(std::memory_order_acquire)) {
            if (!runtime.step()) {
                std::cerr << "dmxwb event=fatal error="
                          << std::quoted(std::string{runtime.last_error()}) << '\n';
                runtime_ok = false;
                break;
            }
            log_runtime_events(runtime, log_state);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        if (g_stop_requested.load(std::memory_order_acquire)) {
            std::cout << "dmxwb event=shutdown reason=signal\n";
        }

        const auto flush_result = runtime.shutdown();
        if (!flush_result.ok()) {
            std::cerr << "dmxwb event=state_flush_failed error="
                      << std::quoted(std::string{flush_result.error.message}) << '\n';
            runtime_ok = false;
        } else {
            std::cout << "dmxwb event=state_flushed\n";
        }

        std::cout << "dmxwb: stopped\n"
                  << "dmxwb event=stopped result="
                  << (runtime_ok ? "ok" : "error") << '\n';
        return runtime_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "dmxwb event=startup_failed error="
                  << std::quoted(std::string{error.what()}) << '\n';
        return 1;
    }
}
