#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_output.hpp"
#include "dmxwb/dmx_test_pattern.hpp"
#include "dmxwb/dmx_transport.hpp"
#include "dmxwb/fixture.hpp"

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


enum class FixtureHardwareState {
    red,
    green,
    blue,
    temperature_0,
    temperature_50,
    temperature_100,
    brightness_50,
    power_off,
    power_on_restore,
    reset,
    all_off,
};

struct FixtureHardwareTestOptions final {
    FixtureHardwareState state{FixtureHardwareState::all_off};
    std::string port{dmxwb::kDefaultDmxPort};
    std::size_t start_address{1};
    std::size_t seconds{3};
};

[[nodiscard]] std::optional<FixtureHardwareState> parse_fixture_hardware_state(std::string_view value) noexcept {
    if (value == "red") {
        return FixtureHardwareState::red;
    }
    if (value == "green") {
        return FixtureHardwareState::green;
    }
    if (value == "blue") {
        return FixtureHardwareState::blue;
    }
    if (value == "temperature-0") {
        return FixtureHardwareState::temperature_0;
    }
    if (value == "temperature-50") {
        return FixtureHardwareState::temperature_50;
    }
    if (value == "temperature-100") {
        return FixtureHardwareState::temperature_100;
    }
    if (value == "brightness-50") {
        return FixtureHardwareState::brightness_50;
    }
    if (value == "power-off") {
        return FixtureHardwareState::power_off;
    }
    if (value == "power-on-restore") {
        return FixtureHardwareState::power_on_restore;
    }
    if (value == "reset") {
        return FixtureHardwareState::reset;
    }
    if (value == "all-off") {
        return FixtureHardwareState::all_off;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view fixture_hardware_state_name(FixtureHardwareState state) noexcept {
    switch (state) {
        case FixtureHardwareState::red:
            return "red";
        case FixtureHardwareState::green:
            return "green";
        case FixtureHardwareState::blue:
            return "blue";
        case FixtureHardwareState::temperature_0:
            return "temperature-0";
        case FixtureHardwareState::temperature_50:
            return "temperature-50";
        case FixtureHardwareState::temperature_100:
            return "temperature-100";
        case FixtureHardwareState::brightness_50:
            return "brightness-50";
        case FixtureHardwareState::power_off:
            return "power-off";
        case FixtureHardwareState::power_on_restore:
            return "power-on-restore";
        case FixtureHardwareState::reset:
            return "reset";
        case FixtureHardwareState::all_off:
            return "all-off";
    }
    return "unknown";
}

[[nodiscard]] bool apply_fixture_hardware_state(dmxwb::Fixture& fixture, FixtureHardwareState state) noexcept {
    switch (state) {
        case FixtureHardwareState::red:
            fixture.set_power(true);
            fixture.set_color(255, 0, 0);
            return true;
        case FixtureHardwareState::green:
            fixture.set_power(true);
            fixture.set_color(0, 255, 0);
            return true;
        case FixtureHardwareState::blue:
            fixture.set_power(true);
            fixture.set_color(0, 0, 255);
            return true;
        case FixtureHardwareState::temperature_0:
            fixture.set_power(true);
            return fixture.set_temperature(0);
        case FixtureHardwareState::temperature_50:
            fixture.set_power(true);
            return fixture.set_temperature(50);
        case FixtureHardwareState::temperature_100:
            fixture.set_power(true);
            return fixture.set_temperature(100);
        case FixtureHardwareState::brightness_50:
            fixture.set_power(true);
            return fixture.set_temperature(100) && fixture.set_brightness(50);
        case FixtureHardwareState::power_off:
            fixture.set_power(true);
            if (!fixture.set_temperature(100) || !fixture.set_brightness(50)) {
                return false;
            }
            fixture.set_power(false);
            return true;
        case FixtureHardwareState::power_on_restore:
            fixture.set_power(true);
            if (!fixture.set_temperature(100) || !fixture.set_brightness(50)) {
                return false;
            }
            fixture.set_power(false);
            fixture.set_power(true);
            return true;
        case FixtureHardwareState::reset:
            fixture.set_power(true);
            fixture.set_color(1, 2, 3);
            if (!fixture.set_brightness(17) || !fixture.set_temperature(23)) {
                return false;
            }
            fixture.reset();
            return true;
        case FixtureHardwareState::all_off:
            fixture.set_power(false);
            return true;
    }
    return false;
}

struct DmxTestOptions final {
    dmxwb::DmxTestPattern pattern{dmxwb::DmxTestPattern::all_off};
    std::string port{dmxwb::kDefaultDmxPort};
    std::size_t start_channel{1};
    std::size_t frames{120};
    std::size_t slots{0};
};

struct DmxContinuousTestOptions final {
    dmxwb::DmxTestPattern pattern{dmxwb::DmxTestPattern::all_off};
    std::string port{dmxwb::kDefaultDmxPort};
    std::size_t start_channel{1};
    std::uint32_t refresh_hz{dmxwb::kDmxOutputRefreshHz};
    std::size_t seconds{10};
    std::size_t slots{0};
};

void print_help() {
    std::cout
        << "Usage:\n"
        << "  dmxwb --help\n"
        << "  dmxwb --version\n"
        << "  dmxwb --dmx-test PATTERN [--port PATH] [--start-channel N] [--frames N] [--slots N]\n"
        << "  dmxwb --dmx-continuous-test PATTERN [--port PATH] [--start-channel N] [--refresh HZ] [--seconds N] [--slots N]\n"
        << "  dmxwb --fixture-hardware-test STATE [--port PATH] [--start-address N] [--seconds N]\n"
        << "\n"
        << "PATTERN: all-off | red | green | blue | white | all-on\n"
        << "STATE: red | green | blue | temperature-0 | temperature-50 | temperature-100 | brightness-50 | power-off | power-on-restore | reset | all-off\n"
        << "\n"
        << "DEV-003 one-shot hardware diagnostic defaults:\n"
        << "  --port /dev/ttyRS485-1 --start-channel 1 --frames 120\n"
        << "\n"
        << "DEV-004 continuous-output diagnostic defaults:\n"
        << "  --port /dev/ttyRS485-1 --start-channel 1 --refresh 44 --seconds 10\n"
        << "  --slots N pads the diagnostic frame with zero channels up to N (1..300)\n"
        << "  production DMX profile is fixed at 44 Hz; --refresh accepts only 44\n"
        << "\n"
        << "DEV-005 Fixture hardware diagnostic defaults:\n"
        << "  --port /dev/ttyRS485-1 --start-address 1 --seconds 3\n"
        << "  snapshot path is FixtureCollection -> DmxSnapshot -> fixed 44 Hz DmxOutput\n";
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
        } else if (argument == "--slots") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > dmxwb::kDmxPhysicalMaxSlots) {
                std::cerr << "--slots must be in range 1..300\n";
                return std::nullopt;
            }
            options.slots = *parsed;
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
            if (!parsed.has_value() || *parsed != dmxwb::kDmxOutputRefreshHz) {
                std::cerr << "--refresh is fixed at 44 Hz for the production DMX profile\n";
                return std::nullopt;
            }
            options.refresh_hz = dmxwb::kDmxOutputRefreshHz;
        } else if (argument == "--seconds") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 3600) {
                std::cerr << "--seconds must be in range 1..3600\n";
                return std::nullopt;
            }
            options.seconds = *parsed;
        } else if (argument == "--slots") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > dmxwb::kDmxPhysicalMaxSlots) {
                std::cerr << "--slots must be in range 1..300\n";
                return std::nullopt;
            }
            options.slots = *parsed;
        } else {
            std::cerr << "Unknown DMX continuous test option: " << argument << '\n';
            return std::nullopt;
        }
        index += 2;
    }

    return options;
}

[[nodiscard]] std::optional<FixtureHardwareTestOptions> parse_fixture_hardware_test_options(int argc, char* argv[]) {
    if (argc < 3 || std::string_view{argv[1]} != "--fixture-hardware-test") {
        return std::nullopt;
    }

    const auto state = parse_fixture_hardware_state(argv[2]);
    if (!state.has_value()) {
        std::cerr << "Invalid Fixture hardware state: " << argv[2] << '\n';
        return std::nullopt;
    }

    FixtureHardwareTestOptions options{};
    options.state = *state;

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
        } else if (argument == "--start-address") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 297) {
                std::cerr << "--start-address must be in range 1..297 for one RGBW Fixture within physical slot 300\n";
                return std::nullopt;
            }
            options.start_address = *parsed;
        } else if (argument == "--seconds") {
            const auto parsed = parse_size(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 60) {
                std::cerr << "--seconds must be in range 1..60 for DEV-005 hardware diagnostic\n";
                return std::nullopt;
            }
            options.seconds = *parsed;
        } else {
            std::cerr << "Unknown Fixture hardware test option: " << argument << '\n';
            return std::nullopt;
        }
        index += 2;
    }

    return options;
}

[[nodiscard]] std::shared_ptr<const dmxwb::DmxSnapshot> make_padded_test_snapshot(
    dmxwb::DmxTestPattern pattern,
    std::size_t start_channel,
    std::size_t requested_slots) {
    const auto base = dmxwb::make_dmx_test_snapshot(pattern, start_channel);
    if (!base) {
        return {};
    }
    if (requested_slots == 0 || requested_slots == base->slot_count()) {
        return base;
    }
    if (requested_slots < base->slot_count() || requested_slots > dmxwb::kDmxPhysicalMaxSlots) {
        return {};
    }

    const auto maybe_builder = dmxwb::DmxSnapshotBuilder::create(requested_slots);
    if (!maybe_builder.has_value()) {
        return {};
    }
    auto builder = *maybe_builder;
    for (std::size_t channel = 1; channel <= base->slot_count(); ++channel) {
        const auto value = base->channel(channel);
        if (!value.has_value() || !builder.set_channel(channel, *value)) {
            return {};
        }
    }
    return builder.build(base->generation());
}

int run_dmx_test(const DmxTestOptions& options) {
    const auto snapshot = make_padded_test_snapshot(options.pattern, options.start_channel, options.slots);
    if (!snapshot) {
        std::cerr << "Cannot create DMX test snapshot for start channel " << options.start_channel
                  << " and requested slots " << options.slots << '\n';
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
    const auto snapshot = make_padded_test_snapshot(options.pattern, options.start_channel, options.slots);
    if (!snapshot) {
        std::cerr << "Cannot create DMX test snapshot for start channel " << options.start_channel
                  << " and requested slots " << options.slots << '\n';
        return 2;
    }

    dmxwb::DmxOutput output{dmxwb::DmxOutputConfig{
        options.port,
        std::chrono::milliseconds{250}}};

    if (!output.publish_snapshot(*snapshot)) {
        std::cerr << "Cannot publish initial DMX snapshot within the 300-slot physical limit\n";
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

    if (diagnostics.frames_sent == 0 ||
        diagnostics.open_failures != 0 ||
        diagnostics.send_failures != 0 ||
        diagnostics.missed_deadlines != 0 ||
        diagnostics.active_refresh_hz != dmxwb::kDmxOutputRefreshHz) {
        std::cerr << "DEV-004 continuous DMX diagnostic failed\n";
        return 1;
    }

    std::cout << "DEV-004 continuous DMX diagnostic completed; serial port closed cleanly.\n";
    return 0;
}

[[nodiscard]] bool fixture_snapshot_matches(
    const dmxwb::DmxSnapshot& snapshot,
    std::size_t start_address,
    const dmxwb::RgbwValues& expected) noexcept {
    return snapshot.channel(start_address) == std::optional<std::uint8_t>{expected.red} &&
           snapshot.channel(start_address + 1) == std::optional<std::uint8_t>{expected.green} &&
           snapshot.channel(start_address + 2) == std::optional<std::uint8_t>{expected.blue} &&
           snapshot.channel(start_address + 3) == std::optional<std::uint8_t>{expected.white};
}

void print_rgbw(std::string_view label, const dmxwb::RgbwValues& values) {
    std::cout << "  " << label << ": "
              << static_cast<unsigned int>(values.red) << '/'
              << static_cast<unsigned int>(values.green) << '/'
              << static_cast<unsigned int>(values.blue) << '/'
              << static_cast<unsigned int>(values.white) << '\n';
}

int run_fixture_hardware_test(const FixtureHardwareTestOptions& options) {
    dmxwb::FixtureCollection fixtures;
    if (!fixtures.reconfigure(1, options.start_address)) {
        std::cerr << "Cannot configure one RGBW Fixture at start address " << options.start_address << '\n';
        return 2;
    }

    auto* fixture = fixtures.fixture_at(0);
    if (fixture == nullptr || !apply_fixture_hardware_state(*fixture, options.state)) {
        std::cerr << "Cannot apply Fixture hardware state " << fixture_hardware_state_name(options.state) << '\n';
        return 2;
    }

    constexpr dmxwb::DmxSnapshot::Generation kFixtureDiagnosticGeneration = 5005;
    const auto snapshot = fixtures.build_snapshot(kFixtureDiagnosticGeneration);
    const auto actual = fixture->actual_rgbw();
    if (!snapshot) {
        std::cerr << "FixtureCollection failed to build a DMX snapshot\n";
        return 2;
    }

    const bool snapshot_check = fixture_snapshot_matches(*snapshot, options.start_address, actual);

    std::cout << "DEV-005 Fixture RGBW hardware diagnostic\n"
              << "  port: " << options.port << '\n'
              << "  state: " << fixture_hardware_state_name(options.state) << '\n'
              << "  fixture_id: " << fixture->id() << '\n'
              << "  fixture_start_address: " << options.start_address << '\n'
              << "  requested_power: " << (fixture->requested_power() ? "ON" : "OFF") << '\n';
    print_rgbw("saved_rgbw", fixture->saved_rgbw());
    std::cout << "  brightness: " << static_cast<unsigned int>(fixture->brightness()) << '\n'
              << "  temperature: " << static_cast<unsigned int>(fixture->temperature()) << '\n';
    print_rgbw("actual_rgbw", actual);
    std::cout << "  physical_slots: " << snapshot->slot_count() << '\n'
              << "  refresh: " << dmxwb::kDmxOutputRefreshHz << " Hz\n"
              << "  duration: " << options.seconds << " s\n"
              << "  snapshot_check: " << (snapshot_check ? "PASS" : "FAIL") << '\n';

    if (!snapshot_check) {
        std::cerr << "Fixture snapshot bytes do not match Fixture::actual_rgbw()\n";
        return 1;
    }

    dmxwb::DmxOutput output{dmxwb::DmxOutputConfig{
        options.port,
        std::chrono::milliseconds{250}}};

    if (!output.publish_snapshot(*snapshot)) {
        std::cerr << "Cannot publish Fixture snapshot to DmxOutput\n";
        return 2;
    }
    if (!output.start()) {
        std::cerr << "Cannot start DmxOutput for Fixture hardware diagnostic\n";
        return 1;
    }

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
              << std::chrono::duration_cast<std::chrono::microseconds>(diagnostics.max_transport_overhead).count() << '\n'
              << "  serial_open_after_stop: " << (diagnostics.serial_open ? 1 : 0) << '\n';

    if (!diagnostics.last_error.empty()) {
        std::cout << "  last_error: " << diagnostics.last_error << '\n';
    }

    const bool software_pass =
        diagnostics.frames_sent != 0 &&
        diagnostics.open_failures == 0 &&
        diagnostics.send_failures == 0 &&
        diagnostics.recoveries == 0 &&
        diagnostics.missed_deadlines == 0 &&
        diagnostics.active_generation == kFixtureDiagnosticGeneration &&
        diagnostics.active_refresh_hz == dmxwb::kDmxOutputRefreshHz &&
        !diagnostics.serial_open;

    std::cout << "software_result: " << (software_pass ? "PASS" : "FAIL") << '\n';
    if (!software_pass) {
        std::cerr << "DEV-005 Fixture RGBW hardware diagnostic failed\n";
        return 1;
    }

    std::cout << "DEV-005 Fixture RGBW hardware diagnostic completed; serial port closed cleanly.\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << dmxwb::application_name() << " " << dmxwb::application_version()
                  << " (DEV-005 Fixture model available; persistence/MQTT/Art-Net runtime not implemented yet)\n";
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

    if (argc >= 2 && std::string_view{argv[1]} == "--fixture-hardware-test") {
        const auto options = parse_fixture_hardware_test_options(argc, argv);
        if (!options.has_value()) {
            std::cerr << "Use --help for supported DEV-005 Fixture diagnostic options.\n";
            return 2;
        }
        return run_fixture_hardware_test(*options);
    }

    std::cerr << "Unknown arguments. Use --help for supported options.\n";
    return 2;
}
