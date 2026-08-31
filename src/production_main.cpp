#include "dmxwb/app_info.hpp"
#include "dmxwb/integrated_runtime.hpp"
#include "dmxwb/persistence_storage.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
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

}  // namespace

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;

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
        // DEV-012D is the explicit checkpoint for the registered production
        // Art-Net OEM identity. Until that gate, ArtDmx input is fully active
        // while ArtPollReply is intentionally not advertised by production.
        runtime_config.artnet_oem_code.reset();
        runtime_config.artnet_port_name = "DMXWB";
        runtime_config.artnet_long_name = "DMXWB Art-Net input";

        dmxwb::IntegratedRuntime runtime{std::move(runtime_config)};
        if (!runtime.start()) {
            std::cerr << runtime.last_error() << '\n';
            return 1;
        }

        std::cout
            << "dmxwb: running\n"
            << "config_path: " << runtime.config_path() << '\n'
            << "state_path: " << runtime.state_path() << '\n';

        bool runtime_ok = true;
        while (!g_stop_requested.load(std::memory_order_acquire)) {
            if (!runtime.step()) {
                std::cerr << runtime.last_error() << '\n';
                runtime_ok = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        const auto flush_result = runtime.shutdown();
        if (!flush_result.ok()) {
            std::cerr << "Cannot flush persistent state during shutdown\n";
            runtime_ok = false;
        }

        std::cout << "dmxwb: stopped\n";
        return runtime_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "dmxwb startup failed: " << error.what() << '\n';
        return 1;
    }
}
