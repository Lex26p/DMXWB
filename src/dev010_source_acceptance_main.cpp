#include "dmxwb/artnet_source_coordinator.hpp"
#include "dmxwb/artnet_transport_linux.hpp"
#include "dmxwb/dmx_output.hpp"
#include "dmxwb/mqtt_client.hpp"
#include "dmxwb/mqtt_runtime.hpp"
#include "dmxwb/persistence_runtime.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

std::atomic_bool g_stop_requested{false};

void handle_signal(int) noexcept {
    g_stop_requested.store(true, std::memory_order_release);
}

class RandomPollReplyDelaySource final : public dmxwb::IArtNetPollReplyDelaySource {
public:
    RandomPollReplyDelaySource()
        : engine_(make_seed()), distribution_(0, 1000) {}

    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept override {
        return std::chrono::milliseconds{distribution_(engine_)};
    }

private:
    [[nodiscard]] static std::uint32_t make_seed() noexcept {
        try {
            std::random_device random_device;
            return random_device();
        } catch (...) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            return static_cast<std::uint32_t>(static_cast<std::uint64_t>(now));
        }
    }

    std::mt19937 engine_;
    std::uniform_int_distribution<int> distribution_;
};

class DmxOutputPhysicalSink final {
public:
    explicit DmxOutputPhysicalSink(dmxwb::DmxOutput& output) noexcept
        : output_(output) {}

    [[nodiscard]] bool publish(const dmxwb::DmxSnapshot& snapshot) {
        if (!output_.publish_snapshot(snapshot)) {
            publish_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!output_.running()) {
            if (ever_started_.load(std::memory_order_acquire)) {
                unexpected_stops_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (!output_.start()) {
                start_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            ever_started_.store(true, std::memory_order_release);
        }
        return true;
    }

    [[nodiscard]] bool ever_started() const noexcept {
        return ever_started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t start_failures() const noexcept {
        return start_failures_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t publish_failures() const noexcept {
        return publish_failures_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t unexpected_stops() const noexcept {
        return unexpected_stops_.load(std::memory_order_acquire);
    }

private:
    dmxwb::DmxOutput& output_;
    std::atomic_bool ever_started_{false};
    std::atomic<std::uint64_t> start_failures_{0};
    std::atomic<std::uint64_t> publish_failures_{0};
    std::atomic<std::uint64_t> unexpected_stops_{0};
};

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
        << "This is a DEV-010 engineering acceptance runtime. The OEM value supplied here\n"
        << "is development-only and must not be used as a production Art-Net assignment.\n";
}

[[nodiscard]] bool parse_unsigned_decimal(
    std::string_view text,
    std::uint64_t max_value,
    std::uint64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > max_value) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool parse_hex_u16(std::string_view text, std::uint16_t& value) noexcept {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 4) {
        return false;
    }
    unsigned int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > 0xffffU) {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_hex_byte(std::string_view text, std::uint8_t& value) noexcept {
    if (text.empty() || text.size() > 2) {
        return false;
    }
    unsigned int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
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
                std::cerr << "Invalid --development-oem-code: " << value << '\n';
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
            if (!parse_unsigned_decimal(value, 60000, parsed) || parsed < 100) {
                std::cerr << "Invalid --status-interval-ms: " << value << '\n';
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
        std::cerr << "--development-oem-code is required for DEV-010 discovery acceptance\n";
        return std::nullopt;
    }
    if (!options.mac_set) {
        std::cerr << "--mac is required for DEV-010 discovery acceptance\n";
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] const char* source_state_name(dmxwb::ArtNetSourceState state) noexcept {
    switch (state) {
        case dmxwb::ArtNetSourceState::waiting: return "WAITING";
        case dmxwb::ArtNetSourceState::active: return "ACTIVE";
        case dmxwb::ArtNetSourceState::lost: return "LOST";
        case dmxwb::ArtNetSourceState::conflict: return "CONFLICT";
    }
    return "UNKNOWN";
}

[[nodiscard]] const char* sync_mode_name(dmxwb::ArtNetSyncMode mode) noexcept {
    switch (mode) {
        case dmxwb::ArtNetSyncMode::asynchronous: return "ASYNC";
        case dmxwb::ArtNetSyncMode::synchronous: return "SYNC";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view source_name(dmxwb::PersistedSource source) noexcept {
    return source == dmxwb::PersistedSource::mqtt ? "mqtt" : "artnet";
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

void print_status(
    const dmxwb::DmxSourceRouter& router,
    const dmxwb::DmxOutput& output,
    const DmxOutputPhysicalSink& physical_sink) {
    const auto router_diag = router.diagnostics();
    const auto dmx_diag = output.diagnostics();
    std::cout
        << "status_selected_source: " << source_name(router_diag.selected_source) << '\n'
        << "status_has_mqtt_snapshot: " << (router.has_mqtt_snapshot() ? 1 : 0) << '\n'
        << "status_has_artnet_snapshot: " << (router.has_artnet_snapshot() ? 1 : 0) << '\n'
        << "status_artnet_output_active: " << (router_diag.artnet_output_active ? 1 : 0) << '\n'
        << "status_dmx_output_ever_started: " << (physical_sink.ever_started() ? 1 : 0) << '\n'
        << "status_dmx_output_running: " << (output.running() ? 1 : 0) << '\n'
        << "status_dmx_frames_sent: " << dmx_diag.frames_sent << '\n';
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

    dmxwb::PersistenceRuntime persistence{options->config_path, options->state_path};
    const auto initial_source = persistence.source();
    if (persistence.config().fixture_count == 0) {
        std::cerr << "DEV-010 source acceptance requires at least one configured Fixture\n";
        return 2;
    }

    dmxwb::MqttCommandQueue command_queue;
    dmxwb::MqttController controller{persistence};
    dmxwb::MqttClient mqtt{command_queue};
    dmxwb::DmxOutput output{dmxwb::DmxOutputConfig{
        persistence.config().dmx_port,
        std::chrono::milliseconds{250}}};
    DmxOutputPhysicalSink physical_sink{output};
    dmxwb::DmxSourceRouter router{
        initial_source,
        [&physical_sink](const dmxwb::DmxSnapshot& snapshot) {
            return physical_sink.publish(snapshot);
        }};
    dmxwb::MqttRuntimeCoordinator mqtt_runtime{
        persistence,
        command_queue,
        controller,
        mqtt,
        router};

    // Always build/cache the logical MQTT snapshot. If persisted Source=artnet,
    // DmxSourceRouter does not call the physical sink, so DmxOutput remains
    // stopped until a real Art-Net snapshot is selected or Source changes.
    if (!mqtt_runtime.publish_initial_snapshot()) {
        std::cerr << "Cannot initialize MQTT/source routing\n";
        output.stop();
        return 1;
    }

    // This check is deliberately made before the Art-Net socket/worker starts.
    // With persisted Source=artnet there cannot yet be a process-local ArtDmx
    // snapshot, so the physical DMX worker must still be stopped. This avoids
    // emitting an artificial empty/zero startup frame while waiting for ArtDmx.
    const bool startup_artnet_output_deferred =
        initial_source != dmxwb::PersistedSource::artnet || !output.running();
    if (!startup_artnet_output_deferred) {
        std::cerr << "ART-NET startup incorrectly started DmxOutput before first ArtDmx\n";
        output.stop();
        return 1;
    }

    dmxwb::LinuxArtNetDatagramTransport artnet_transport;
    RandomPollReplyDelaySource poll_reply_delay;
    dmxwb::ArtNetRuntimeConfig artnet_config;
    artnet_config.core.port_address = persistence.config().artnet_universe;
    artnet_config.poll_reply_identity.oem_code = options->development_oem_code;
    artnet_config.poll_reply_identity.mac = options->mac;
    artnet_config.poll_reply_identity.port_name = "DMXWB DEV010B3";
    artnet_config.poll_reply_identity.long_name = "DMXWB DEV-010B3 source switching acceptance";
    artnet_config.poll_reply_identity.firmware_version = 0x0100;
    artnet_config.rebind_delay = std::chrono::milliseconds{500};

    auto artnet_runtime = dmxwb::ArtNetRuntime::create(
        artnet_config,
        artnet_transport,
        poll_reply_delay);
    if (!artnet_runtime) {
        std::cerr << "Cannot create ArtNetRuntime\n";
        output.stop();
        return 1;
    }
    dmxwb::ArtNetSourceCoordinator artnet_source{*artnet_runtime, router};

    if (!mqtt.start()) {
        artnet_source.shutdown();
        output.stop();
        std::cerr << "Cannot start MQTT client\n";
        return 1;
    }

    std::atomic_bool artnet_stop{false};
    std::thread artnet_worker;
    try {
        artnet_worker = std::thread{[&artnet_source, &artnet_stop] {
            while (!artnet_stop.load(std::memory_order_acquire)) {
                artnet_source.step(dmxwb::ArtNetCore::clock::now());
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }};
    } catch (...) {
        mqtt.stop();
        artnet_source.shutdown();
        output.stop();
        std::cerr << "Cannot start Art-Net worker\n";
        return 1;
    }

    std::cout
        << "dmxwb_dev010_source_acceptance: running\n"
        << "config_path: " << options->config_path << '\n'
        << "state_path: " << options->state_path << '\n'
        << "dmx_port: " << persistence.config().dmx_port << '\n'
        << "fixture_count: " << persistence.config().fixture_count << '\n'
        << "artnet_port_address: " << persistence.config().artnet_universe << '\n'
        << "initial_source: " << source_name(initial_source) << '\n'
        << "development_oem_placeholder: 1\n"
        << "mqtt_runtime_connected: 1\n"
        << "artnet_runtime_connected: 1\n"
        << "source_router_connected: 1\n"
        << "physical_dmx_path_connected: 1\n"
        << "startup_artnet_without_snapshot_output_deferred: "
        << (startup_artnet_output_deferred ? 1 : 0) << '\n'
        << "runtime_started: PASS\n";

    auto next_status = std::chrono::steady_clock::now();
    bool unexpected_output_stop = false;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        mqtt_runtime.step(dmxwb::StatePersistenceManager::clock::now());

        if (physical_sink.ever_started() && !output.running()) {
            unexpected_output_stop = true;
            std::cerr << "DmxOutput worker stopped unexpectedly\n";
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status) {
            print_status(router, output, physical_sink);
            next_status = now + options->status_interval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    artnet_stop.store(true, std::memory_order_release);
    if (artnet_worker.joinable()) {
        artnet_worker.join();
    }

    const bool artnet_transport_was_open = artnet_runtime->diagnostics().transport_open;
    artnet_source.shutdown();
    const auto flush_result = mqtt_runtime.flush_state();
    mqtt.stop();
    output.stop();

    const auto mqtt_diag = mqtt.diagnostics();
    const auto& mqtt_runtime_diag = mqtt_runtime.diagnostics();
    const auto artnet_diag = artnet_runtime->diagnostics();
    const auto& artnet_source_diag = artnet_source.diagnostics();
    const auto router_diag = router.diagnostics();
    const auto dmx_diag = output.diagnostics();

    std::cout
        << "final_selected_source: " << source_name(router_diag.selected_source) << '\n'
        << "final_mqtt_successful_connections: " << mqtt_diag.successful_connections << '\n'
        << "final_mqtt_disconnects: " << mqtt_diag.disconnects << '\n'
        << "final_mqtt_callback_failures: " << mqtt_diag.callback_failures << '\n'
        << "final_mqtt_runtime_commands_processed: " << mqtt_runtime_diag.commands_processed << '\n'
        << "final_mqtt_runtime_dmx_publish_failures: " << mqtt_runtime_diag.dmx_publish_failures << '\n'
        << "final_mqtt_runtime_state_save_failures: " << mqtt_runtime_diag.state_save_failures << '\n'
        << "final_artnet_source_state: " << source_state_name(artnet_runtime->core().source_state()) << '\n'
        << "final_artnet_sync_mode: " << sync_mode_name(artnet_runtime->core().sync_mode()) << '\n'
        << "final_artnet_bind_attempts: " << artnet_diag.bind_attempts << '\n'
        << "final_artnet_bind_failures: " << artnet_diag.bind_failures << '\n'
        << "final_artnet_transport_recoveries: " << artnet_diag.transport_recoveries << '\n'
        << "final_artnet_datagrams_received: " << artnet_diag.datagrams_received << '\n'
        << "final_artnet_receive_errors: " << artnet_diag.receive_errors << '\n'
        << "final_artnet_send_errors: " << artnet_diag.send_errors << '\n'
        << "final_artnet_core_rejections: " << artnet_diag.core_rejections << '\n'
        << "final_artnet_conflicts: " << artnet_diag.conflicts << '\n'
        << "final_artnet_source_lost_events: " << artnet_diag.source_lost_events << '\n'
        << "final_artnet_snapshots_published: " << artnet_diag.snapshots_published << '\n'
        << "final_artnet_poll_replies_sent: " << artnet_diag.poll_replies_sent << '\n'
        << "final_artnet_route_failures: " << artnet_source_diag.route_failures << '\n'
        << "final_router_mqtt_snapshots_received: " << router_diag.mqtt_snapshots_received << '\n'
        << "final_router_artnet_snapshots_received: " << router_diag.artnet_snapshots_received << '\n'
        << "final_router_source_switches: " << router_diag.source_switches << '\n'
        << "final_router_source_switches_without_snapshot: "
        << router_diag.source_switches_without_snapshot << '\n'
        << "final_router_physical_snapshots_published: "
        << router_diag.physical_snapshots_published << '\n'
        << "final_router_physical_publish_failures: "
        << router_diag.physical_publish_failures << '\n'
        << "final_router_artnet_output_active: " << (router_diag.artnet_output_active ? 1 : 0) << '\n'
        << "final_dmx_sink_ever_started: " << (physical_sink.ever_started() ? 1 : 0) << '\n'
        << "final_dmx_sink_start_failures: " << physical_sink.start_failures() << '\n'
        << "final_dmx_sink_publish_failures: " << physical_sink.publish_failures() << '\n'
        << "final_dmx_sink_unexpected_stops: " << physical_sink.unexpected_stops() << '\n'
        << "final_dmx_frames_sent: " << dmx_diag.frames_sent << '\n'
        << "final_dmx_open_failures: " << dmx_diag.open_failures << '\n'
        << "final_dmx_send_failures: " << dmx_diag.send_failures << '\n'
        << "final_dmx_recoveries: " << dmx_diag.recoveries << '\n'
        << "final_dmx_missed_deadlines: " << dmx_diag.missed_deadlines << '\n'
        << "final_dmx_active_refresh_hz: " << dmx_diag.active_refresh_hz << '\n'
        << "final_dmx_serial_open_after_stop: " << (dmx_diag.serial_open ? 1 : 0) << '\n'
        << "final_artnet_transport_open_before_shutdown: " << (artnet_transport_was_open ? 1 : 0) << '\n'
        << "final_artnet_transport_open_after_shutdown: "
        << (artnet_runtime->diagnostics().transport_open ? 1 : 0) << '\n'
        << "state_flush_action: " << save_action_name(flush_result.action) << '\n';

    const bool dmx_ok =
        (!physical_sink.ever_started() || dmx_diag.frames_sent > 0) &&
        physical_sink.start_failures() == 0 &&
        physical_sink.publish_failures() == 0 &&
        physical_sink.unexpected_stops() == 0 &&
        dmx_diag.missed_deadlines == 0 &&
        dmx_diag.active_refresh_hz == dmxwb::kDmxOutputRefreshHz &&
        !dmx_diag.serial_open;

    const bool pass =
        !unexpected_output_stop &&
        flush_result.ok() &&
        mqtt_diag.successful_connections >= 1 &&
        mqtt_diag.callback_failures == 0 &&
        mqtt_runtime_diag.dmx_publish_failures == 0 &&
        mqtt_runtime_diag.state_save_failures == 0 &&
        artnet_source_diag.route_failures == 0 &&
        router_diag.physical_publish_failures == 0 &&
        !artnet_runtime->diagnostics().transport_open &&
        dmx_ok;

    std::cout << "software_result: " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
