#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_test_pattern.hpp"
#include "dmxwb/dmx_transport.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

struct DmxTestOptions final {
    dmxwb::DmxTestPattern pattern{dmxwb::DmxTestPattern::all_off};
    std::string port{dmxwb::kDefaultDmxPort};
    std::size_t start_channel{1};
    std::size_t frames{120};
};

void print_help() {
    std::cout
        << "Usage:\n"
        << "  dmxwb --help\n"
        << "  dmxwb --version\n"
        << "  dmxwb --dmx-test PATTERN [--port PATH] [--start-channel N] [--frames N]\n"
        << "\n"
        << "DEV-003 hardware diagnostic mode (Linux/Wiren Board only).\n"
        << "PATTERN: all-off | red | green | blue | white | all-on\n"
        << "Defaults: --port /dev/ttyRS485-1 --start-channel 1 --frames 120\n"
        << "The diagnostic repeats one fixed RGBW frame with an approximate 25 ms pause.\n"
        << "It is not the production continuous/timed DMX engine from DEV-004.\n";
}

[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view value) noexcept {
    std::size_t parsed = 0;
    const char* const begin = value.data();
    const char* const end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<DmxTestOptions> parse_dmx_test_options(int argc, char* argv[]) {
    if (argc < 3 || std::string_view{argv[1]} != "--dmx-test") {
        return std::nullopt;
    }

    const auto pattern = dmxwb::parse_dmx_test_pattern(argv[2]);
    if (!pattern.has_value()) {
        std::cerr << "Invalid DMX test pattern: " << argv[2] << '\n';
        return std::nullopt;
    }

    DmxTestOptions options{};
    options.pattern = *pattern;

    int index = 3;
    while (index < argc) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            std::cerr << "Missing value after " << argument << '\n';
            return std::nullopt;
        }
        const std::string_view value{argv[index + 1]};

        if (argument == "--port") {
            if (value.empty()) {
                std::cerr << "--port must not be empty\n";
                return std::nullopt;
            }
            options.port = std::string{value};
        } else if (argument == "--start-channel") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 509) {
                std::cerr << "--start-channel must be in range 1..509 for one RGBW fixture\n";
                return std::nullopt;
            }
            options.start_channel = *parsed;
        } else if (argument == "--frames") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 100000) {
                std::cerr << "--frames must be in range 1..100000\n";
                return std::nullopt;
            }
            options.frames = *parsed;
        } else {
            std::cerr << "Unknown DMX test option: " << argument << '\n';
            return std::nullopt;
        }
        index += 2;
    }

    return options;
}

int run_dmx_test(const DmxTestOptions& options) {
    const auto snapshot = dmxwb::make_dmx_test_snapshot(options.pattern, options.start_channel);
    if (!snapshot) {
        std::cerr << "Cannot create DMX test snapshot for start channel " << options.start_channel << '\n';
        return 2;
    }

    dmxwb::DmxTransport transport{options.port};
    if (!transport.open()) {
        std::cerr << "DMX transport open failed: " << transport.last_error() << '\n';
        return 1;
    }

    std::cout << "DEV-003 DMX hardware test\n"
              << "  port: " << transport.port() << '\n'
              << "  pattern: " << dmxwb::dmx_test_pattern_name(options.pattern) << '\n'
              << "  RGBW start channel: " << options.start_channel << '\n'
              << "  physical slots: " << snapshot->slot_count() << '\n'
              << "  frames: " << options.frames << '\n';

    const auto frame = dmxwb::make_frame_view(*snapshot);
    constexpr auto kDiagnosticPause = std::chrono::milliseconds{25};

    for (std::size_t frame_number = 0; frame_number < options.frames; ++frame_number) {
        if (!transport.send_frame(frame)) {
            std::cerr << "DMX frame " << (frame_number + 1)
                      << " failed: " << transport.last_error() << '\n';
            transport.close();
            return 1;
        }
        if (frame_number + 1 < options.frames) {
            std::this_thread::sleep_for(kDiagnosticPause);
        }
    }

    transport.close();
    std::cout << "DMX test completed; serial port closed cleanly.\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << dmxwb::application_name() << " " << dmxwb::application_version()
                  << " (DEV-003 physical DMX transport diagnostic available; no production runtime yet)\n";
        return 0;
    }

    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument == "--version") {
            std::cout << dmxwb::application_name() << " " << dmxwb::application_version() << '\n';
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            print_help();
            return 0;
        }
    }

    if (argc >= 2 && std::string_view{argv[1]} == "--dmx-test") {
        const auto options = parse_dmx_test_options(argc, argv);
        if (!options.has_value()) {
            std::cerr << "Use --help for supported DEV-003 diagnostic options.\n";
            return 2;
        }
        return run_dmx_test(*options);
    }

    std::cerr << "Unknown arguments. Use --help for supported options.\n";
    return 2;
}
