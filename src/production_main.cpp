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
    bool artnet_transport_open{false};
    bool artnet_ever_ready{false};
    dmxwb::ArtNetSourceState artnet_source_state{dmxwb::ArtNetSourceState::waiting};
    dmxwb::PersistedSource selected_source{dmxwb::PersistedSource::mqtt};
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
    const auto current = runtime.operational_state();
    state.initialized = true;
    state.mqtt_connected = current.mqtt.connected;
    state.mqtt_ever_connected = current.mqtt.connected;
    state.dmx_serial_open = current.dmx.serial_open;
    state.dmx_ever_ready = current.dmx.serial_open;
    state.dmx_error_active = !current.dmx.serial_open && !current.dmx.last_error.empty();
    state.artnet_transport_open = current.artnet.transport_open;
    state.artnet_ever_ready = current.artnet.transport_open;
    state.artnet_source_state = current.artnet.source_state;
    state.selected_source = current.selected_source;
    state.applied_dmx_port = current.applied_dmx_port;
    state.applied_artnet_universe = current.applied_artnet_port_address;

    if (state.mqtt_connected) {
        std::cout << "dmxwb event=mqtt_connected\n";
    }
    if (state.dmx_serial_open) {
        std::cout << "dmxwb event=dmx_ready port="
                  << std::quoted(state.applied_dmx_port) << '\n';
    } else if (state.dmx_error_active) {
        std::cerr << "dmxwb event=dmx_error port="
                  << std::quoted(state.applied_dmx_port)
                  << " error=" << std::quoted(current.dmx.last_error) << '\n';
    }
    if (state.artnet_transport_open) {
        std::cout << "dmxwb event=artnet_ready\n";
    } else {
        std::cerr << "dmxwb event=artnet_error state=reconnecting\n";
    }
    if (state.artnet_source_state != dmxwb::ArtNetSourceState::waiting) {
        auto& output =
            state.artnet_source_state == dmxwb::ArtNetSourceState::lost ||
            state.artnet_source_state == dmxwb::ArtNetSourceState::conflict
                ? std::cerr
                : std::cout;
        output << "dmxwb event=artnet_source state="
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

    const auto current = runtime.operational_state();

    if (current.mqtt.connected != state.mqtt_connected) {
        if (current.mqtt.connected) {
            std::cout << "dmxwb event="
                      << (state.mqtt_ever_connected ? "mqtt_recovered" : "mqtt_connected")
                      << '\n';
            state.mqtt_ever_connected = true;
        } else if (state.mqtt_connected) {
            std::cerr << "dmxwb event=mqtt_lost";
            if (!current.mqtt.last_error.empty()) {
                std::cerr << " error=" << std::quoted(current.mqtt.last_error);
            }
            std::cerr << '\n';
        }
        state.mqtt_connected = current.mqtt.connected;
    }

    if (current.dmx.serial_open != state.dmx_serial_open) {
        if (current.dmx.serial_open) {
            std::cout << "dmxwb event="
                      << (state.dmx_ever_ready ? "dmx_recovered" : "dmx_ready")
                      << " port=" << std::quoted(current.applied_dmx_port) << '\n';
            state.dmx_ever_ready = true;
            state.dmx_error_active = false;
        } else if (state.dmx_serial_open) {
            std::cerr << "dmxwb event=dmx_lost port="
                      << std::quoted(current.applied_dmx_port);
            if (!current.dmx.last_error.empty()) {
                std::cerr << " error=" << std::quoted(current.dmx.last_error);
            }
            std::cerr << '\n';
            state.dmx_error_active = true;
        }
        state.dmx_serial_open = current.dmx.serial_open;
    }
    if (!current.dmx.serial_open && !current.dmx.last_error.empty() &&
        !state.dmx_error_active) {
        std::cerr << "dmxwb event=dmx_error port="
                  << std::quoted(current.applied_dmx_port)
                  << " error=" << std::quoted(current.dmx.last_error) << '\n';
        state.dmx_error_active = true;
    }

    if (current.artnet.source_state != state.artnet_source_state) {
        const auto name = artnet_source_state_name(current.artnet.source_state);
        if (current.artnet.source_state == dmxwb::ArtNetSourceState::lost ||
            current.artnet.source_state == dmxwb::ArtNetSourceState::conflict) {
            std::cerr << "dmxwb event=artnet_source state=" << name << '\n';
        } else {
            std::cout << "dmxwb event=artnet_source state=" << name << '\n';
        }
        state.artnet_source_state = current.artnet.source_state;
    }

    if (current.artnet.transport_open != state.artnet_transport_open) {
        if (current.artnet.transport_open) {
            std::cout << "dmxwb event="
                      << (state.artnet_ever_ready ? "artnet_recovered" : "artnet_ready")
                      << '\n';
            state.artnet_ever_ready = true;
        } else {
            std::cerr << "dmxwb event=artnet_error state=reconnecting\n";
        }
        state.artnet_transport_open = current.artnet.transport_open;
    }

    if (current.selected_source != state.selected_source) {
        std::cout << "dmxwb event=source_selected source="
                  << dmxwb::persisted_source_name(current.selected_source) << '\n';
        state.selected_source = current.selected_source;
    }

    if (current.applied_dmx_port != state.applied_dmx_port ||
        current.applied_artnet_port_address != state.applied_artnet_universe) {
        std::cout << "dmxwb event=config_applied dmx_port="
                  << std::quoted(current.applied_dmx_port)
                  << " artnet_universe=" << current.applied_artnet_port_address << '\n';
        state.applied_dmx_port = current.applied_dmx_port;
        state.applied_artnet_universe = current.applied_artnet_port_address;
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

    const auto path_error = dmxwb::validate_persistence_paths(
        options->config_path,
        options->state_path);
    if (path_error) {
        std::cerr << "Invalid persistence paths: " << path_error.message << '\n';
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
        runtime_config.instrumentation_mode =
            dmxwb::InstrumentationMode::production;

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
