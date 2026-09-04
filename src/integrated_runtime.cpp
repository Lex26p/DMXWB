#include "dmxwb/integrated_runtime.hpp"

#include "dmxwb/artnet_source_coordinator.hpp"
#include "dmxwb/artnet_transport_linux.hpp"
#include "dmxwb/dmx_output_physical_sink.hpp"
#include "dmxwb/mqtt_contract.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace dmxwb {
namespace {

constexpr auto kOperationalStatusPublishInterval = std::chrono::seconds{1};
constexpr auto kOperationalStatusOfflineRetryInterval = std::chrono::milliseconds{250};

class RandomPollReplyDelaySource final : public IArtNetPollReplyDelaySource {
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

void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[(byte >> 4U) & 0x0FU]);
                    output.push_back(hex[byte & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string_view artnet_source_state_name(ArtNetSourceState state) noexcept {
    switch (state) {
        case ArtNetSourceState::waiting: return "WAITING";
        case ArtNetSourceState::active: return "ACTIVE";
        case ArtNetSourceState::lost: return "LOST";
        case ArtNetSourceState::conflict: return "CONFLICT";
    }
    return "WAITING";
}

[[nodiscard]] std::string_view artnet_sync_mode_name(ArtNetSyncMode mode) noexcept {
    return mode == ArtNetSyncMode::synchronous ? "SYNC" : "ASYNC";
}

[[nodiscard]] std::string ipv4_address_string(const ArtNetIpv4Address& address) {
    return std::to_string(address.octets[0]) + "." +
           std::to_string(address.octets[1]) + "." +
           std::to_string(address.octets[2]) + "." +
           std::to_string(address.octets[3]);
}

}  // namespace

OperationalStatusPayload build_operational_status_payload(
    const IntegratedRuntimeOperationalState& state,
    bool stopping) {
    const bool dmx_ready = state.dmx.running && state.dmx.serial_open;
    const std::string_view configuration_state = state.configuration_ok
        ? std::string_view{"ok"}
        : state.configuration_fallback ? std::string_view{"fallback"} : std::string_view{"error"};

    std::string_view dmx_detail_state = "waiting";
    if (stopping) {
        dmx_detail_state = "off";
    } else if (!state.dmx_output_ever_started) {
        dmx_detail_state = "waiting";
    } else if (dmx_ready) {
        dmx_detail_state = "running";
    } else if (state.dmx.running && state.dmx.last_error.empty()) {
        dmx_detail_state = "waiting";
    } else if (state.dmx.running) {
        dmx_detail_state = "reconnecting";
    } else {
        dmx_detail_state = "error";
    }

    const std::string_view dmx_top_state =
        dmx_detail_state == "running" ? std::string_view{"running"} :
        (dmx_detail_state == "waiting" || dmx_detail_state == "off") ?
            std::string_view{"off"} : std::string_view{"error"};
    const std::string_view dmx_recovery =
        dmx_detail_state == "running" ? std::string_view{"ok"} : dmx_detail_state;

    const std::string_view mqtt_detail_state =
        stopping ? std::string_view{"offline"} :
        state.mqtt.connected ? std::string_view{"connected"} :
        state.mqtt.running ? std::string_view{"reconnecting"} :
            std::string_view{"offline"};
    const std::string_view mqtt_recovery =
        stopping || !state.mqtt.running ? std::string_view{"off"} :
        state.mqtt.connected ? std::string_view{"ok"} :
            std::string_view{"reconnecting"};

    const auto artnet_source_state = artnet_source_state_name(state.artnet.source_state);
    const std::string_view artnet_top_state =
        stopping ? std::string_view{"off"} :
        state.artnet.transport_open ? artnet_source_state : std::string_view{"error"};
    const std::string_view artnet_recovery =
        stopping || !state.started ? std::string_view{"off"} :
        state.artnet.transport_open ? std::string_view{"ok"} :
            std::string_view{"reconnecting"};
    const std::string_view artnet_output_mode =
        state.selected_source != PersistedSource::artnet ? std::string_view{"inactive"} :
        (state.artnet_output_active && state.artnet.source_state == ArtNetSourceState::active && dmx_ready) ?
            std::string_view{"live"} : std::string_view{"hold_last"};

    std::string artnet_last_error;
    if (!stopping) {
        if (!state.artnet.transport_open) {
            artnet_last_error = "Art-Net transport unavailable";
        } else if (state.artnet.source_state == ArtNetSourceState::lost) {
            artnet_last_error = "Art-Net source lost";
        } else if (state.artnet.source_state == ArtNetSourceState::conflict) {
            artnet_last_error = "Art-Net source conflict";
        }
    }

    MqttApplicationStatus application = MqttApplicationStatus::running;
    if (stopping) {
        application = MqttApplicationStatus::off;
    } else if (!state.configuration_ok ||
               dmx_detail_state == "reconnecting" ||
               dmx_detail_state == "error" ||
               (state.selected_source == PersistedSource::mqtt && !state.mqtt.connected) ||
               (state.selected_source == PersistedSource::artnet &&
                (!state.artnet.transport_open ||
                 state.artnet.source_state == ArtNetSourceState::lost ||
                 state.artnet.source_state == ArtNetSourceState::conflict))) {
        application = MqttApplicationStatus::error;
    }

    std::string last_error;
    if (!stopping) {
        if (!state.runtime_last_error.empty()) {
            last_error = state.runtime_last_error;
        } else if ((dmx_detail_state == "reconnecting" || dmx_detail_state == "error") &&
                   !state.dmx.last_error.empty()) {
            last_error = state.dmx.last_error;
        } else if (state.selected_source == PersistedSource::mqtt && !state.mqtt.connected) {
            last_error = state.mqtt.last_error.empty()
                ? "MQTT broker unavailable"
                : state.mqtt.last_error;
        } else if (state.selected_source == PersistedSource::artnet &&
                   !artnet_last_error.empty()) {
            last_error = artnet_last_error;
        } else if (!state.configuration_ok) {
            last_error = state.persistence_last_error.empty()
                ? "Persistence unavailable"
                : state.persistence_last_error;
        }
    }

    std::string output{"{\"application\":"};
    append_json_string(output, mqtt_application_status_name(application));
    output += ",\"dmx\":";
    append_json_string(output, dmx_top_state);
    output += ",\"mqtt\":";
    append_json_string(
        output,
        stopping ? std::string_view{"offline"} :
            (state.mqtt.connected ? std::string_view{"connected"} : std::string_view{"offline"}));
    output += ",\"artnet\":";
    append_json_string(output, artnet_top_state);
    output += ",\"configuration\":";
    append_json_string(output, configuration_state);
    output += ",\"last_error\":";
    append_json_string(output, last_error);
    output += ",\"diagnostics\":{\"selected_source\":";
    append_json_string(output, persisted_source_name(state.selected_source));

    output += ",\"dmx\":{\"state\":";
    append_json_string(output, dmx_detail_state);
    output += ",\"port\":";
    append_json_string(output, state.applied_dmx_port);
    output += ",\"slot_count\":" + std::to_string(state.dmx.slot_count);
    output += ",\"refresh_hz\":" + std::to_string(state.dmx.refresh_hz);
    output += ",\"physical_slot_limit\":" + std::to_string(kDmxPhysicalMaxSlots);
    output += ",\"active_generation\":" + std::to_string(state.dmx.active_generation);
    output += ",\"last_error\":";
    append_json_string(output, state.dmx.last_error);
    output += ",\"recovery_state\":";
    append_json_string(output, dmx_recovery);
    output += '}';

    output += ",\"mqtt\":{\"state\":";
    append_json_string(output, mqtt_detail_state);
    output += ",\"connected\":";
    output += state.mqtt.connected && !stopping ? "true" : "false";
    output += ",\"recovery_state\":";
    append_json_string(output, mqtt_recovery);
    output += ",\"last_error\":";
    append_json_string(output, state.mqtt.last_error);
    output += '}';

    output += ",\"artnet\":{\"state\":";
    append_json_string(output, artnet_source_state);
    output += ",\"universe\":" + std::to_string(state.applied_artnet_port_address);
    output += ",\"active_source_ip\":";
    if (state.artnet.active_source.has_value()) {
        append_json_string(output, ipv4_address_string(state.artnet.active_source->ip));
    } else {
        output += "null";
    }
    output += ",\"active_source_physical\":";
    if (state.artnet.active_source.has_value()) {
        output += std::to_string(state.artnet.active_source->physical);
    } else {
        output += "null";
    }
    output += ",\"last_packet_age_ms\":";
    if (state.artnet.last_packet_age.has_value()) {
        output += std::to_string(state.artnet.last_packet_age->count());
    } else {
        output += "null";
    }
    output += ",\"last_sequence\":";
    if (state.artnet.last_sequence.has_value()) {
        output += std::to_string(*state.artnet.last_sequence);
    } else {
        output += "null";
    }
    output += ",\"sync_mode\":";
    append_json_string(output, artnet_sync_mode_name(state.artnet.sync_mode));
    output += ",\"last_sync_age_ms\":";
    if (state.artnet.last_sync_age.has_value()) {
        output += std::to_string(state.artnet.last_sync_age->count());
    } else {
        output += "null";
    }
    output += ",\"conflicting_source_ip\":";
    if (state.artnet.conflicting_source.has_value()) {
        append_json_string(output, ipv4_address_string(state.artnet.conflicting_source->ip));
    } else {
        output += "null";
    }
    output += ",\"conflicting_source_physical\":";
    if (state.artnet.conflicting_source.has_value()) {
        output += std::to_string(state.artnet.conflicting_source->physical);
    } else {
        output += "null";
    }
    output += ",\"output_mode\":";
    append_json_string(output, artnet_output_mode);
    output += ",\"transport_open\":";
    output += state.artnet.transport_open ? "true" : "false";
    output += ",\"committed_revision\":" + std::to_string(state.artnet.committed_revision);
    output += ",\"last_error\":";
    append_json_string(output, artnet_last_error);
    output += ",\"recovery_state\":";
    append_json_string(output, artnet_recovery);
    output += '}';

    output += ",\"configuration\":{\"state\":";
    append_json_string(output, configuration_state);
    output += ",\"revision\":" + std::to_string(state.configuration_revision);
    output += ",\"dmx_port\":";
    append_json_string(output, state.applied_dmx_port);
    output += ",\"artnet_universe\":" + std::to_string(state.applied_artnet_port_address);
    output += ",\"config_path\":";
    append_json_string(output, state.config_path);
    output += ",\"state_path\":";
    append_json_string(output, state.state_path);
    output += ",\"last_error\":";
    append_json_string(output, state.persistence_last_error);
    output += ",\"recovery_state\":";
    append_json_string(output, state.configuration_ok ? std::string_view{"ok"} : configuration_state);
    output += "}}}";

    return {application, std::move(output)};
}

class IntegratedRuntime::Impl final {
public:
    explicit Impl(IntegratedRuntimeConfig runtime_config)
        : config(std::move(runtime_config)),
          persistence(this->config.config_path, this->config.state_path),
          initial_source_value(persistence.source()),
          controller(persistence),
          mqtt(command_queue, this->config.instrumentation_mode),
          physical_sink(DmxOutputConfig{
              persistence.config().dmx_port,
              std::chrono::milliseconds{250}},
              this->config.instrumentation_mode),
          router(
              initial_source_value,
              [this](const DmxSnapshot& snapshot) {
                  return physical_sink.publish(snapshot);
              },
              this->config.instrumentation_mode),
          mqtt_runtime(
              persistence,
              command_queue,
              controller,
              mqtt,
              router,
              this->config.instrumentation_mode),
          applied_dmx_port(persistence.config().dmx_port),
          applied_artnet_universe(persistence.config().artnet_universe) {
        artnet_config.core.port_address = persistence.config().artnet_universe;
        artnet_config.poll_reply_identity.oem_code = this->config.artnet_oem_code;
        artnet_config.poll_reply_identity.mac = this->config.artnet_mac;
        artnet_config.poll_reply_identity.port_name = this->config.artnet_port_name;
        artnet_config.poll_reply_identity.long_name = this->config.artnet_long_name;
        artnet_config.poll_reply_identity.firmware_version = this->config.firmware_version;
        artnet_config.rebind_delay = std::chrono::milliseconds{500};

        artnet_runtime = ArtNetRuntime::create(
            artnet_config,
            artnet_transport,
            poll_reply_delay,
            this->config.instrumentation_mode);
        if (!artnet_runtime) {
            throw std::runtime_error("Cannot create ArtNetRuntime");
        }

        artnet_source = std::make_unique<ArtNetSourceCoordinator>(
            *artnet_runtime,
            router,
            this->config.instrumentation_mode);
    }

    ~Impl() {
        (void)shutdown();
    }

    [[nodiscard]] bool start() {
        if (started_flag) {
            return true;
        }
        if (shutdown_done) {
            set_error("Integrated runtime was already shut down");
            return false;
        }

        if (!mqtt_runtime.publish_initial_snapshot()) {
            set_error("Cannot initialize MQTT/source routing");
            physical_sink.stop();
            return false;
        }

        startup_artnet_output_deferred_value =
            initial_source_value != PersistedSource::artnet ||
            !physical_sink.running();
        if (!startup_artnet_output_deferred_value) {
            set_error("ART-NET startup started DmxOutput before first ArtDmx");
            physical_sink.stop();
            return false;
        }

        if (!mqtt.start()) {
            artnet_source->shutdown();
            physical_sink.stop();
            set_error("Cannot start MQTT client");
            return false;
        }

        if (!start_artnet_worker()) {
            mqtt.stop();
            artnet_source->shutdown();
            physical_sink.stop();
            set_error("Cannot start Art-Net worker");
            return false;
        }

        started_flag = true;
        next_operational_status_publish = std::chrono::steady_clock::now();
        return true;
    }

    [[nodiscard]] bool step() {
        if (!started_flag || shutdown_done) {
            set_error("Integrated runtime is not running");
            return false;
        }

        mqtt_runtime.step(StatePersistenceManager::clock::now());

        const auto& current_config = persistence.config();
        if (current_config.dmx_port != applied_dmx_port) {
            if (!physical_sink.reconfigure_port(current_config.dmx_port)) {
                set_error("Cannot apply configured DMX Port without restart");
                publish_operational_status_now(false);
                return false;
            }
            applied_dmx_port = current_config.dmx_port;
        }

        if (current_config.artnet_universe != applied_artnet_universe) {
            if (!reconfigure_artnet_universe(current_config.artnet_universe)) {
                set_error("Cannot apply configured Art-Net Universe without restart");
                publish_operational_status_now(false);
                return false;
            }
            applied_artnet_universe = current_config.artnet_universe;
        }

        if (physical_sink.ever_started() && !physical_sink.running()) {
            set_error("DmxOutput worker stopped unexpectedly");
            publish_operational_status_now(false);
            return false;
        }

        publish_operational_status_if_due();
        return true;
    }

    [[nodiscard]] StateSaveResult shutdown() {
        if (shutdown_done) {
            return flush_result;
        }

        artnet_stop.store(true, std::memory_order_release);
        if (artnet_worker.joinable()) {
            artnet_worker.join();
        }

        if (artnet_runtime) {
            artnet_transport_was_open =
                artnet_runtime->diagnostics().transport_open;
        }
        if (artnet_source) {
            artnet_source->shutdown();
        }

        flush_result = mqtt_runtime.flush_state();
        physical_sink.stop();
        publish_operational_status_now(true);
        mqtt.stop();

        started_flag = false;
        shutdown_done = true;
        return flush_result;
    }

    [[nodiscard]] bool start_artnet_worker() {
        artnet_stop.store(false, std::memory_order_release);
        try {
            artnet_worker = std::thread{[this] {
                while (!artnet_stop.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard lock{artnet_mutex};
                        artnet_source->step(ArtNetCore::clock::now());
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
            }};
        } catch (...) {
            return false;
        }
        return true;
    }

    void stop_artnet_for_reconfigure() {
        artnet_stop.store(true, std::memory_order_release);
        if (artnet_worker.joinable()) {
            artnet_worker.join();
        }
        artnet_source->shutdown();
    }

    [[nodiscard]] bool reconfigure_artnet_universe(std::uint16_t port_address) {
        stop_artnet_for_reconfigure();

        // Data from the previous Port-Address must never be replayed after a
        // structural config transaction. Physical output remains Hold Last.
        router.clear_artnet_snapshot();

        artnet_config.core.port_address = port_address;
        auto replacement = ArtNetRuntime::create(
            artnet_config,
            artnet_transport,
            poll_reply_delay,
            config.instrumentation_mode);
        if (!replacement) {
            if (engineering_instrumentation_enabled(config.instrumentation_mode)) {
                ++artnet_universe_reconfigure_failures;
            }
            return false;
        }

        artnet_runtime = std::move(replacement);
        artnet_source = std::make_unique<ArtNetSourceCoordinator>(
            *artnet_runtime,
            router,
            config.instrumentation_mode);

        if (!start_artnet_worker()) {
            if (engineering_instrumentation_enabled(config.instrumentation_mode)) {
                ++artnet_universe_reconfigure_failures;
            }
            artnet_source->shutdown();
            return false;
        }

        if (engineering_instrumentation_enabled(config.instrumentation_mode)) {
            ++artnet_universe_reconfigurations;
        }
        return true;
    }

    void set_error(std::string value) {
        last_error_value = std::move(value);
    }

    [[nodiscard]] IntegratedRuntimeStatus status() const {
        IntegratedRuntimeStatus result;
        const auto router_diag = router.diagnostics();
        const auto dmx_diag = physical_sink.diagnostics();

        result.selected_source = router_diag.selected_source;
        result.has_mqtt_snapshot = router.has_mqtt_snapshot();
        result.has_artnet_snapshot = router.has_artnet_snapshot();
        result.artnet_output_active = router_diag.artnet_output_active;
        result.dmx_output_ever_started = physical_sink.ever_started();
        result.dmx_output_running = physical_sink.running();
        result.dmx_frames_sent = dmx_diag.frames_sent;
        return result;
    }

    [[nodiscard]] IntegratedRuntimeOperationalState operational_state() const {
        IntegratedRuntimeOperationalState result;
        result.started = started_flag && !shutdown_done;
        result.selected_source = router.selected_source();
        result.has_mqtt_snapshot = router.has_mqtt_snapshot();
        result.has_artnet_snapshot = router.has_artnet_snapshot();
        result.artnet_output_active = router.artnet_output_active();
        result.dmx_output_ever_started = physical_sink.ever_started();
        result.dmx = physical_sink.operational_state();
        result.mqtt = mqtt.operational_state();
        {
            std::lock_guard lock{artnet_mutex};
            result.artnet = artnet_runtime->operational_state(ArtNetCore::clock::now());
        }
        const auto& persistence_status = persistence.operational_status();
        result.configuration_ok = persistence_status.ok();
        result.configuration_fallback = persistence_status.fallback_active;
        result.configuration_revision = persistence.config().revision;
        result.applied_dmx_port = applied_dmx_port;
        result.applied_artnet_port_address = applied_artnet_universe;
        result.config_path = persistence.config_path();
        result.state_path = persistence.state_path();
        result.persistence_last_error = std::string{persistence_status.last_error()};
        result.runtime_last_error = last_error_value;
        return result;
    }

    [[nodiscard]] IntegratedRuntimeDiagnostics diagnostics() const {
        IntegratedRuntimeDiagnostics result;
        result.mqtt = mqtt.diagnostics();
        result.mqtt_runtime = mqtt_runtime.diagnostics();
        {
            std::lock_guard lock{artnet_mutex};
            result.artnet = artnet_runtime->diagnostics();
            result.artnet_source = artnet_source->diagnostics();
            result.artnet_source_state = artnet_runtime->core().source_state();
            result.artnet_sync_mode = artnet_runtime->core().sync_mode();
            result.artnet_transport_open_after_shutdown =
                artnet_runtime->diagnostics().transport_open;
        }
        result.router = router.diagnostics();
        result.dmx = physical_sink.diagnostics();

        result.startup_artnet_output_deferred =
            startup_artnet_output_deferred_value;
        result.dmx_sink_ever_started = physical_sink.ever_started();
        result.dmx_sink_start_failures = physical_sink.start_failures();
        result.dmx_sink_publish_failures = physical_sink.publish_failures();
        result.dmx_sink_unexpected_stops = physical_sink.unexpected_stops();
        result.dmx_port_reconfigurations = physical_sink.reconfigurations();
        result.dmx_port_reconfigure_failures =
            physical_sink.reconfigure_failures();
        result.artnet_universe_reconfigurations =
            artnet_universe_reconfigurations;
        result.artnet_universe_reconfigure_failures =
            artnet_universe_reconfigure_failures;
        result.applied_dmx_port = applied_dmx_port;
        result.applied_artnet_port_address = applied_artnet_universe;
        result.artnet_transport_open_before_shutdown =
            artnet_transport_was_open;
        return result;
    }

    [[nodiscard]] OperationalStatusPayload build_operational_status(bool stopping) const {
        return build_operational_status_payload(operational_state(), stopping);
    }

    void publish_operational_status_if_due() {
        const auto now = std::chrono::steady_clock::now();
        if (next_operational_status_publish.has_value() &&
            now < *next_operational_status_publish) {
            return;
        }

        if (!mqtt.connected()) {
            next_operational_status_publish = now + kOperationalStatusOfflineRetryInterval;
            return;
        }

        publish_operational_status_now(false);
        next_operational_status_publish = now + kOperationalStatusPublishInterval;
    }

    void publish_operational_status_now(bool stopping) {
        if (!mqtt.connected()) {
            return;
        }

        const auto payload = build_operational_status(stopping);
        const std::array<MqttPublication, 2> publications{{
            MqttPublication{
                std::string{kMqttStatusTopic},
                payload.json,
                true},
            MqttPublication{
                "/devices/dmxwb/controls/status",
                std::string{mqtt_application_status_name(payload.application)},
                true},
        }};
        (void)mqtt.publish_all(publications);
    }

    IntegratedRuntimeConfig config;
    PersistenceRuntime persistence;
    PersistedSource initial_source_value{PersistedSource::mqtt};
    MqttCommandQueue command_queue;
    MqttController controller;
    MqttClient mqtt;
    DmxOutputPhysicalSink physical_sink;
    DmxSourceRouter router;
    MqttRuntimeCoordinator mqtt_runtime;

    LinuxArtNetDatagramTransport artnet_transport;
    RandomPollReplyDelaySource poll_reply_delay;
    ArtNetRuntimeConfig artnet_config;
    std::unique_ptr<ArtNetRuntime> artnet_runtime;
    std::unique_ptr<ArtNetSourceCoordinator> artnet_source;
    mutable std::mutex artnet_mutex;

    std::atomic_bool artnet_stop{false};
    std::thread artnet_worker;

    std::string applied_dmx_port;
    std::uint16_t applied_artnet_universe{0};
    std::uint64_t artnet_universe_reconfigurations{0};
    std::uint64_t artnet_universe_reconfigure_failures{0};

    std::optional<std::chrono::steady_clock::time_point> next_operational_status_publish;
    bool startup_artnet_output_deferred_value{false};
    bool artnet_transport_was_open{false};
    bool started_flag{false};
    bool shutdown_done{false};
    StateSaveResult flush_result{};
    std::string last_error_value;
};

IntegratedRuntime::IntegratedRuntime(IntegratedRuntimeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

IntegratedRuntime::~IntegratedRuntime() = default;

bool IntegratedRuntime::start() {
    return impl_->start();
}

bool IntegratedRuntime::step() {
    return impl_->step();
}

StateSaveResult IntegratedRuntime::shutdown() {
    return impl_->shutdown();
}

bool IntegratedRuntime::started() const noexcept {
    return impl_->started_flag;
}

std::string_view IntegratedRuntime::last_error() const noexcept {
    return impl_->last_error_value;
}

std::string_view IntegratedRuntime::config_path() const noexcept {
    return impl_->persistence.config_path();
}

std::string_view IntegratedRuntime::state_path() const noexcept {
    return impl_->persistence.state_path();
}

std::string_view IntegratedRuntime::configured_dmx_port() const noexcept {
    return impl_->persistence.config().dmx_port;
}

std::uint16_t IntegratedRuntime::configured_artnet_port_address() const noexcept {
    return impl_->persistence.config().artnet_universe;
}

std::string_view IntegratedRuntime::applied_dmx_port() const noexcept {
    return impl_->applied_dmx_port;
}

std::uint16_t IntegratedRuntime::applied_artnet_port_address() const noexcept {
    return impl_->applied_artnet_universe;
}

std::size_t IntegratedRuntime::fixture_count() const noexcept {
    return impl_->persistence.config().fixture_count;
}

PersistedSource IntegratedRuntime::initial_source() const noexcept {
    return impl_->initial_source_value;
}

bool IntegratedRuntime::startup_artnet_output_deferred() const noexcept {
    return impl_->startup_artnet_output_deferred_value;
}

InstrumentationMode IntegratedRuntime::instrumentation_mode() const noexcept {
    return impl_->config.instrumentation_mode;
}

IntegratedRuntimeStatus IntegratedRuntime::status() const {
    return impl_->status();
}

IntegratedRuntimeOperationalState IntegratedRuntime::operational_state() const {
    return impl_->operational_state();
}

IntegratedRuntimeDiagnostics IntegratedRuntime::diagnostics() const {
    return impl_->diagnostics();
}

}  // namespace dmxwb
