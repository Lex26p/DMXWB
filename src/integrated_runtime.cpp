#include "dmxwb/integrated_runtime.hpp"

#include "dmxwb/artnet_source_coordinator.hpp"
#include "dmxwb/artnet_transport_linux.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace dmxwb {
namespace {

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

class DmxOutputPhysicalSink final {
public:
    explicit DmxOutputPhysicalSink(DmxOutputConfig config)
        : config_(std::move(config)),
          output_(std::make_unique<DmxOutput>(config_)) {}

    [[nodiscard]] bool publish(const DmxSnapshot& snapshot) {
        if (!output_->publish_snapshot(snapshot)) {
            publish_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        latest_snapshot_ = snapshot;

        if (!output_->running()) {
            if (ever_started_.load(std::memory_order_acquire)) {
                unexpected_stops_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (!output_->start()) {
                start_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            ever_started_.store(true, std::memory_order_release);
        }
        return true;
    }

    [[nodiscard]] bool reconfigure_port(std::string port) {
        if (port == config_.port) {
            return true;
        }

        auto replacement_config = config_;
        replacement_config.port = std::move(port);

        std::unique_ptr<DmxOutput> replacement;
        try {
            replacement = std::make_unique<DmxOutput>(replacement_config);
        } catch (...) {
            reconfigure_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (latest_snapshot_.has_value() &&
            !replacement->publish_snapshot(*latest_snapshot_)) {
            reconfigure_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const bool was_running = output_->running();
        if (was_running) {
            output_->stop();
        }

        if (was_running && !replacement->start()) {
            if (!output_->start()) {
                unexpected_stops_.fetch_add(1, std::memory_order_relaxed);
            }
            reconfigure_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        output_ = std::move(replacement);
        config_ = std::move(replacement_config);
        reconfigurations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] DmxOutput& output() noexcept {
        return *output_;
    }

    [[nodiscard]] const DmxOutput& output() const noexcept {
        return *output_;
    }

    void stop() noexcept {
        output_->stop();
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

    [[nodiscard]] std::uint64_t reconfigurations() const noexcept {
        return reconfigurations_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t reconfigure_failures() const noexcept {
        return reconfigure_failures_.load(std::memory_order_acquire);
    }

private:
    DmxOutputConfig config_;
    std::unique_ptr<DmxOutput> output_;
    std::optional<DmxSnapshot> latest_snapshot_;
    std::atomic_bool ever_started_{false};
    std::atomic<std::uint64_t> start_failures_{0};
    std::atomic<std::uint64_t> publish_failures_{0};
    std::atomic<std::uint64_t> unexpected_stops_{0};
    std::atomic<std::uint64_t> reconfigurations_{0};
    std::atomic<std::uint64_t> reconfigure_failures_{0};
};

}  // namespace

class IntegratedRuntime::Impl final {
public:
    explicit Impl(IntegratedRuntimeConfig runtime_config)
        : config(std::move(runtime_config)),
          persistence(this->config.config_path, this->config.state_path),
          initial_source_value(persistence.source()),
          controller(persistence),
          mqtt(command_queue),
          physical_sink(DmxOutputConfig{
              persistence.config().dmx_port,
              std::chrono::milliseconds{250}}),
          router(
              initial_source_value,
              [this](const DmxSnapshot& snapshot) {
                  return physical_sink.publish(snapshot);
              }),
          mqtt_runtime(
              persistence,
              command_queue,
              controller,
              mqtt,
              router),
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
            poll_reply_delay);
        if (!artnet_runtime) {
            throw std::runtime_error("Cannot create ArtNetRuntime");
        }

        artnet_source = std::make_unique<ArtNetSourceCoordinator>(
            *artnet_runtime,
            router);
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
            !physical_sink.output().running();
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
                return false;
            }
            applied_dmx_port = current_config.dmx_port;
        }

        if (current_config.artnet_universe != applied_artnet_universe) {
            if (!reconfigure_artnet_universe(current_config.artnet_universe)) {
                set_error("Cannot apply configured Art-Net Universe without restart");
                return false;
            }
            applied_artnet_universe = current_config.artnet_universe;
        }

        if (physical_sink.ever_started() && !physical_sink.output().running()) {
            set_error("DmxOutput worker stopped unexpectedly");
            return false;
        }

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
        mqtt.stop();
        physical_sink.stop();

        started_flag = false;
        shutdown_done = true;
        return flush_result;
    }

    [[nodiscard]] bool start_artnet_worker() {
        artnet_stop.store(false, std::memory_order_release);
        try {
            artnet_worker = std::thread{[this] {
                while (!artnet_stop.load(std::memory_order_acquire)) {
                    artnet_source->step(ArtNetCore::clock::now());
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
            poll_reply_delay);
        if (!replacement) {
            ++artnet_universe_reconfigure_failures;
            return false;
        }

        artnet_runtime = std::move(replacement);
        artnet_source = std::make_unique<ArtNetSourceCoordinator>(
            *artnet_runtime,
            router);

        if (!start_artnet_worker()) {
            ++artnet_universe_reconfigure_failures;
            artnet_source->shutdown();
            return false;
        }

        ++artnet_universe_reconfigurations;
        return true;
    }

    void set_error(std::string value) {
        last_error_value = std::move(value);
    }

    [[nodiscard]] IntegratedRuntimeStatus status() const {
        IntegratedRuntimeStatus result;
        const auto router_diag = router.diagnostics();
        const auto dmx_diag = physical_sink.output().diagnostics();

        result.selected_source = router_diag.selected_source;
        result.has_mqtt_snapshot = router.has_mqtt_snapshot();
        result.has_artnet_snapshot = router.has_artnet_snapshot();
        result.artnet_output_active = router_diag.artnet_output_active;
        result.dmx_output_ever_started = physical_sink.ever_started();
        result.dmx_output_running = physical_sink.output().running();
        result.dmx_frames_sent = dmx_diag.frames_sent;
        return result;
    }

    [[nodiscard]] IntegratedRuntimeDiagnostics diagnostics() const {
        IntegratedRuntimeDiagnostics result;
        result.mqtt = mqtt.diagnostics();
        result.mqtt_runtime = mqtt_runtime.diagnostics();
        result.artnet = artnet_runtime->diagnostics();
        result.artnet_source = artnet_source->diagnostics();
        result.router = router.diagnostics();
        result.dmx = physical_sink.output().diagnostics();

        result.artnet_source_state = artnet_runtime->core().source_state();
        result.artnet_sync_mode = artnet_runtime->core().sync_mode();

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
        result.artnet_transport_open_after_shutdown =
            artnet_runtime->diagnostics().transport_open;
        return result;
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

    std::atomic_bool artnet_stop{false};
    std::thread artnet_worker;

    std::string applied_dmx_port;
    std::uint16_t applied_artnet_universe{0};
    std::uint64_t artnet_universe_reconfigurations{0};
    std::uint64_t artnet_universe_reconfigure_failures{0};

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

IntegratedRuntimeStatus IntegratedRuntime::status() const {
    return impl_->status();
}

IntegratedRuntimeDiagnostics IntegratedRuntime::diagnostics() const {
    return impl_->diagnostics();
}

}  // namespace dmxwb
