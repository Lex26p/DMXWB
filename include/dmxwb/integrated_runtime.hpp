#pragma once

#include "dmxwb/artnet_core.hpp"
#include "dmxwb/artnet_runtime.hpp"
#include "dmxwb/artnet_source_coordinator.hpp"
#include "dmxwb/dmx_output.hpp"
#include "dmxwb/dmx_source_router.hpp"
#include "dmxwb/mqtt_client.hpp"
#include "dmxwb/mqtt_runtime.hpp"
#include "dmxwb/persistence_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dmxwb {

// The integrated runtime owns the already-proven Persistence -> MQTT/Art-Net ->
// DmxSourceRouter -> DmxOutput orchestration. Production and engineering
// acceptance executables are only frontends around this one runtime path.
struct IntegratedRuntimeConfig final {
    std::string config_path;
    std::string state_path;
    std::optional<std::uint16_t> artnet_oem_code;
    std::array<std::uint8_t, 6> artnet_mac{};
    std::string artnet_port_name{"DMXWB"};
    std::string artnet_long_name{"DMXWB Art-Net output"};
    std::uint16_t firmware_version{0x0100};
};

struct IntegratedRuntimeStatus final {
    PersistedSource selected_source{PersistedSource::mqtt};
    bool has_mqtt_snapshot{false};
    bool has_artnet_snapshot{false};
    bool artnet_output_active{false};
    bool dmx_output_ever_started{false};
    bool dmx_output_running{false};
    std::uint64_t dmx_frames_sent{0};
};

struct IntegratedRuntimeDiagnostics final {
    MqttClientDiagnostics mqtt;
    MqttRuntimeDiagnostics mqtt_runtime;
    ArtNetRuntimeDiagnostics artnet;
    ArtNetSourceCoordinatorDiagnostics artnet_source;
    DmxSourceRouterDiagnostics router;
    DmxOutputDiagnostics dmx;

    ArtNetSourceState artnet_source_state{ArtNetSourceState::waiting};
    ArtNetSyncMode artnet_sync_mode{ArtNetSyncMode::asynchronous};

    bool startup_artnet_output_deferred{false};
    bool dmx_sink_ever_started{false};
    std::uint64_t dmx_sink_start_failures{0};
    std::uint64_t dmx_sink_publish_failures{0};
    std::uint64_t dmx_sink_unexpected_stops{0};
    std::uint64_t dmx_port_reconfigurations{0};
    std::uint64_t dmx_port_reconfigure_failures{0};
    std::uint64_t artnet_universe_reconfigurations{0};
    std::uint64_t artnet_universe_reconfigure_failures{0};
    std::string applied_dmx_port;
    std::uint16_t applied_artnet_port_address{0};
    bool artnet_transport_open_before_shutdown{false};
    bool artnet_transport_open_after_shutdown{false};
};

class IntegratedRuntime final {
public:
    explicit IntegratedRuntime(IntegratedRuntimeConfig config);
    ~IntegratedRuntime();

    IntegratedRuntime(const IntegratedRuntime&) = delete;
    IntegratedRuntime& operator=(const IntegratedRuntime&) = delete;
    IntegratedRuntime(IntegratedRuntime&&) = delete;
    IntegratedRuntime& operator=(IntegratedRuntime&&) = delete;

    [[nodiscard]] bool start();
    [[nodiscard]] bool step();
    [[nodiscard]] StateSaveResult shutdown();

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;

    [[nodiscard]] std::string_view config_path() const noexcept;
    [[nodiscard]] std::string_view state_path() const noexcept;
    [[nodiscard]] std::string_view configured_dmx_port() const noexcept;
    [[nodiscard]] std::uint16_t configured_artnet_port_address() const noexcept;
    [[nodiscard]] std::string_view applied_dmx_port() const noexcept;
    [[nodiscard]] std::uint16_t applied_artnet_port_address() const noexcept;
    [[nodiscard]] std::size_t fixture_count() const noexcept;
    [[nodiscard]] PersistedSource initial_source() const noexcept;
    [[nodiscard]] bool startup_artnet_output_deferred() const noexcept;

    [[nodiscard]] IntegratedRuntimeStatus status() const;
    [[nodiscard]] IntegratedRuntimeDiagnostics diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dmxwb
