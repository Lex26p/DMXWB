#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_output.hpp"
#include "dmxwb/dmx_test_pattern.hpp"
#include "dmxwb/dmx_transport.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

struct DmxContinuousTestOptions final {
    dmxwb::DmxTestPattern pattern{dmxwb::DmxTestPattern::all_off};
    std::string port{dmxwb::kDefaultDmxPort};
    std::size_t start_channel{1};
    std::uint32_t refresh_hz{dmxwb::kDmxDefaultRefreshHz};
    std::size_t seconds{10};
};

void print_help() {
    std::cout
        << "Usage:\n"
        << "  dmxwb --help\n"
        << "  dmxwb --version\n"
        << "  dmxwb --dmx-test PATTERN [--port PATH] [--start-channel N] [--frames N]\n"
        << "  dmxwb --dmx-continuous-test PATTERN [--port PATH] [--start-channel N] [--refresh HZ] [--seconds N]\n"
        << "\n"
        << "PATTERN: all-off | red | green | blue | white | all-on\n"
        << "\n"
        << "DEV-003 one-shot hardware diagnostic defaults:\n"
        << "  --port /dev/ttyRS485-1 --start-channel 1 --frames 120\n"
        << "\n"
        << "DEV-004 continuous-output diagnostic defaults:\n"
        << "  --port /dev/ttyRS485-1 --start-channel 1 --refresh 30 --seconds 10\n"
        << "  refresh range: 10..44 Hz, additionally limited by physical frame length\n";
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

[[nodiscard]] std::optional<DmxContinuousTestOptions> parse_dmx_continuous_test_options(int argc, char* argv[]) {
    if (argc < 3 || std::string_view{argv[1]} != "--dmx-continuous-test") {
        return std::nullopt;
    }

    const auto pattern = dmxwb::parse_dmx_test_pattern(argv[2]);
    if (!pattern.has_value()) {
        std::cerr << "Invalid DMX test pattern: " << argv[2] << '\n';
        return std::nullopt;
    }

    DmxContinuousTestOptions options{};
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
        } else if (argument == "--refresh") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < dmxwb::kDmxMinRefreshHz || *parsed > dmxwb::kDmxMaxRefreshHz) {
                std::cerr << "--refresh must be in range 10..44 Hz\n";
                return std::nullopt;
            }
            options.refresh_hz = static_cast<std::uint32_t>(*parsed);
        } else if (argument == "--seconds") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 3600) {
                std::cerr << "--seconds must be in range 1..3600\n";
                return std::nullopt;
            }
            options.seconds = *parsed;
        } else {
            std::cerr << "Unknown DMX continuous test option: " << argument << '\n';
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

int run_dmx_continuous_test(const DmxContinuousTestOptions& options) {
    const auto snapshot = dmxwb::make_dmx_test_snapshot(options.pattern, options.start_channel);
    if (!snapshot) {
        std::cerr << "Cannot create DMX test snapshot for start channel " << options.start_channel << '\n';
        return 2;
    }

    const auto refresh_check = dmxwb::check_dmx_refresh_rate(snapshot->slot_count(), options.refresh_hz);
    if (!refresh_check.valid) {
        std::cerr << "Requested refresh " << options.refresh_hz
                  << " Hz is not physically valid for " << snapshot->slot_count()
                  << " slots; maximum without measured transport overhead is "
                  << refresh_check.max_supported_hz << " Hz\n";
        return 2;
    }

    dmxwb::DmxOutput output{dmxwb::DmxOutputConfig{
        options.port,
        options.refresh_hz,
        std::chrono::milliseconds{250}}};

    if (!output.publish_snapshot(*snapshot)) {
        std::cerr << "Cannot publish initial DMX snapshot for requested refresh\n";
        return 2;
    }
    if (!output.start()) {
        std::cerr << "Cannot start continuous DmxOutput worker\n";
        return 1;
    }

    std::cout << "DEV-004 continuous DMX diagnostic\n"
              << "  port: " << options.port << '\n'
              << "  pattern: " << dmxwb::dmx_test_pattern_name(options.pattern) << '\n'
              << "  RGBW start channel: " << options.start_channel << '\n'
              << "  physical slots: " << snapshot->slot_count() << '\n'
              << "  refresh: " << options.refresh_hz << " Hz\n"
              << "  duration: " << options.seconds << " s\n";

    std::this_thread::sleep_for(
        std::chrono::seconds{static_cast<std::chrono::seconds::rep>(options.seconds)});
    output.stop();

    const auto diagnostics = output.diagnostics();
    std::cout << "DmxOutput diagnostics\n"
              << "  frames_sent: " << diagnostics.frames_sent << '\n'
              << "  open_failures: " << diagnostics.open_failures << '\n'
              << "  send_failures: " << diagnostics.send_failures << '\n'
              << "  recoveries: " << diagnostics.recoveries << '\n'
              << "  missed_deadlines: " << diagnostics.missed_deadlines << '\n'
              << "  active_generation: " << diagnostics.active_generation << '\n'
              << "  active_refresh_hz: " << diagnostics.active_refresh_hz << '\n'
              << "  max_send_us: "
              << std::chrono::duration_cast<std::chrono::microseconds>(diagnostics.max_send_duration).count() << '\n'
              << "  max_transport_overhead_us: "
              << std::chrono::duration_cast<std::chrono::microseconds>(diagnostics.max_transport_overhead).count() << '\n';

    if (!diagnostics.last_error.empty()) {
        std::cout << "  last_error: " << diagnostics.last_error << '\n';
    }

    if (diagnostics.frames_sent == 0 || diagnostics.open_failures != 0 || diagnostics.send_failures != 0) {
        std::cerr << "DEV-004 continuous DMX diagnostic failed\n";
        return 1;
    }

    std::cout << "DEV-004 continuous DMX diagnostic completed; serial port closed cleanly.\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << dmxwb::application_name() << " " << dmxwb::application_version()
                  << " (DEV-004 continuous DMX engine available; application layers not implemented yet)\n";
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

    if (argc >= 2 && std::string_view{argv[1]} == "--dmx-continuous-test") {
        const auto options = parse_dmx_continuous_test_options(argc, argv);
        if (!options.has_value()) {
            std::cerr << "Use --help for supported DEV-004 diagnostic options.\n";
            return 2;
        }
        return run_dmx_continuous_test(*options);
    }

    std::cerr << "Unknown arguments. Use --help for supported options.\n";
    return 2;
}
