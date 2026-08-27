#include "dmxwb/artnet_runtime.hpp"
#include "dmxwb/artnet_transport_linux.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic_bool g_stop_requested{false};

void handle_signal(int) noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
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

struct Options final {
    std::uint16_t port_address{0};
    std::optional<std::uint16_t> development_oem_code;
    std::array<std::uint8_t, 6> mac{};
    bool mac_set{false};
    std::chrono::milliseconds status_interval{1000};
};

void print_usage(std::ostream& stream) {
    stream
        << "Usage:\n"
        << "  dmxwb-artnet-acceptance --development-oem-code HEX --mac XX:XX:XX:XX:XX:XX [options]\n\n"
        << "Options:\n"
        << "  --port-address N            Art-Net Port-Address 0..32767 (default 0)\n"
        << "  --development-oem-code HEX  Explicit development-only OEM placeholder\n"
        << "  --mac XX:XX:XX:XX:XX:XX    MAC advertised in ArtPollReply\n"
        << "  --status-interval-ms N      Periodic diagnostics interval (default 1000)\n"
        << "  --help                      Show this help\n\n"
        << "This engineering acceptance runtime never writes physical DMX and never switches\n"
        << "the application Source. The OEM value supplied here is development-only and must\n"
        << "not be used as a production Art-Net OEM assignment.\n";
}

[[nodiscard]] bool parse_unsigned_decimal(std::string_view text, std::uint64_t max_value, std::uint64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > max_value) {
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
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 0xffffU) {
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
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 0xffU) {
        return false;
    }
    value = static_cast<std::uint8_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_mac(std::string_view text, std::array<std::uint8_t, 6>& mac) noexcept {
    std::size_t position = 0;
    for (std::size_t index = 0; index < mac.size(); ++index) {
        const auto separator = text.find(':', position);
        const bool last = index + 1 == mac.size();
        const auto end = last ? text.size() : separator;
        if ((!last && separator == std::string_view::npos) || end == std::string_view::npos || end <= position) {
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
        if (argument == "--help") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return std::nullopt;
        }
        const std::string_view value{argv[++index]};
        if (argument == "--port-address") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(value, dmxwb::kArtNetPortAddressMax, parsed)) {
                std::cerr << "Invalid --port-address: " << value << '\n';
                return std::nullopt;
            }
            options.port_address = static_cast<std::uint16_t>(parsed);
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

    if (!options.development_oem_code.has_value()) {
        std::cerr << "--development-oem-code is required for QLC+ discovery acceptance\n";
        return std::nullopt;
    }
    if (!options.mac_set) {
        std::cerr << "--mac is required for QLC+ discovery acceptance\n";
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

void print_ip(std::ostream& stream, dmxwb::ArtNetIpv4Address address) {
    stream
        << static_cast<unsigned int>(address.octets[0]) << '.'
        << static_cast<unsigned int>(address.octets[1]) << '.'
        << static_cast<unsigned int>(address.octets[2]) << '.'
        << static_cast<unsigned int>(address.octets[3]);
}

void print_snapshot(const dmxwb::DmxSnapshot& snapshot, const dmxwb::ArtNetCore& core) {
    std::cout << "snapshot_revision: " << snapshot.generation() << '\n';
    std::cout << "snapshot_channels_1_8:";
    for (std::size_t channel = 1; channel <= 8; ++channel) {
        std::cout << ' ' << static_cast<unsigned int>(snapshot.channel(channel).value_or(0));
    }
    std::cout << '\n';
    if (const auto source = core.active_source(); source.has_value()) {
        std::cout << "snapshot_source_ip: ";
        print_ip(std::cout, source->ip);
        std::cout << '\n';
        std::cout << "snapshot_source_physical: " << static_cast<unsigned int>(source->physical) << '\n';
    }
}

void print_status(const dmxwb::ArtNetRuntime& runtime) {
    const auto& diagnostics = runtime.diagnostics();
    const auto& core = runtime.core();
    std::cout << "status_source_state: " << source_state_name(core.source_state()) << '\n';
    std::cout << "status_sync_mode: " << sync_mode_name(core.sync_mode()) << '\n';
    std::cout << "status_transport_open: " << (diagnostics.transport_open ? 1 : 0) << '\n';
    std::cout << "status_datagrams_received: " << diagnostics.datagrams_received << '\n';
    std::cout << "status_core_rejections: " << diagnostics.core_rejections << '\n';
    std::cout << "status_conflicts: " << diagnostics.conflicts << '\n';
    std::cout << "status_source_lost_events: " << diagnostics.source_lost_events << '\n';
    std::cout << "status_snapshots_published: " << diagnostics.snapshots_published << '\n';
    std::cout << "status_poll_replies_sent: " << diagnostics.poll_replies_sent << '\n';
    std::cout << "status_bind_failures: " << diagnostics.bind_failures << '\n';
    std::cout << "status_receive_errors: " << diagnostics.receive_errors << '\n';
    std::cout << "status_send_errors: " << diagnostics.send_errors << '\n';
}

void print_final(const dmxwb::ArtNetRuntime& runtime) {
    const auto& diagnostics = runtime.diagnostics();
    const auto& core = runtime.core();
    std::cout << "final_source_state: " << source_state_name(core.source_state()) << '\n';
    std::cout << "final_sync_mode: " << sync_mode_name(core.sync_mode()) << '\n';
    std::cout << "final_bind_attempts: " << diagnostics.bind_attempts << '\n';
    std::cout << "final_bind_failures: " << diagnostics.bind_failures << '\n';
    std::cout << "final_transport_recoveries: " << diagnostics.transport_recoveries << '\n';
    std::cout << "final_datagrams_received: " << diagnostics.datagrams_received << '\n';
    std::cout << "final_core_rejections: " << diagnostics.core_rejections << '\n';
    std::cout << "final_conflicts: " << diagnostics.conflicts << '\n';
    std::cout << "final_source_lost_events: " << diagnostics.source_lost_events << '\n';
    std::cout << "final_snapshots_published: " << diagnostics.snapshots_published << '\n';
    std::cout << "final_poll_replies_scheduled: " << diagnostics.poll_replies_scheduled << '\n';
    std::cout << "final_poll_replies_sent: " << diagnostics.poll_replies_sent << '\n';
    std::cout << "final_poll_replies_dropped: " << diagnostics.poll_replies_dropped << '\n';
    std::cout << "final_poll_replies_not_built: " << diagnostics.poll_replies_not_built << '\n';
    std::cout << "final_receive_errors: " << diagnostics.receive_errors << '\n';
    std::cout << "final_send_errors: " << diagnostics.send_errors << '\n';
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

    dmxwb::LinuxArtNetDatagramTransport transport;
    RandomPollReplyDelaySource delay_source;

    dmxwb::ArtNetRuntimeConfig config;
    config.core.port_address = options->port_address;
    config.poll_reply_identity.oem_code = options->development_oem_code;
    config.poll_reply_identity.mac = options->mac;
    config.poll_reply_identity.port_name = "DMXWB DEV010A";
    config.poll_reply_identity.long_name = "DMXWB DEV-010A Art-Net network acceptance";
    config.poll_reply_identity.firmware_version = 0x0100;
    config.rebind_delay = std::chrono::milliseconds{500};

    auto runtime = dmxwb::ArtNetRuntime::create(config, transport, delay_source);
    if (!runtime) {
        std::cerr << "Failed to create ArtNetRuntime\n";
        return 1;
    }

    // This acceptance runtime is network-only. It deliberately does not claim
    // that Art-Net data is selected for the physical DMX output.
    runtime->set_artnet_output_active(false);

    std::cout << "dmxwb_artnet_acceptance: running\n";
    std::cout << "artnet_udp_port: " << dmxwb::kArtNetUdpPort << '\n';
    std::cout << "artnet_port_address: " << options->port_address << '\n';
    std::cout << "physical_dmx_connected: 0\n";
    std::cout << "source_switching_connected: 0\n";
    std::cout << "development_oem_placeholder: 1\n";

    auto next_status = dmxwb::ArtNetRuntime::time_point{};
    std::uint64_t last_snapshot_revision = 0;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const auto now = dmxwb::ArtNetCore::clock::now();
        runtime->step(now);

        const auto snapshot = runtime->latest_physical_snapshot();
        if (snapshot && snapshot->generation() != last_snapshot_revision) {
            last_snapshot_revision = snapshot->generation();
            print_snapshot(*snapshot, runtime->core());
        }

        if (next_status == dmxwb::ArtNetRuntime::time_point{} || now >= next_status) {
            print_status(*runtime);
            next_status = now + options->status_interval;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    print_final(*runtime);
    const bool transport_was_open = runtime->diagnostics().transport_open;
    runtime->shutdown();
    std::cout << "final_transport_open_before_shutdown: " << (transport_was_open ? 1 : 0) << '\n';
    std::cout << "final_transport_open_after_shutdown: " << (runtime->diagnostics().transport_open ? 1 : 0) << '\n';
    std::cout << "dmxwb_artnet_acceptance: stopped\n";
    return 0;
}
