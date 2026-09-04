#pragma once

#include "dmxwb/dmx_snapshot.hpp"
#include "dmxwb/instrumentation.hpp"
#include "dmxwb/persistence.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace dmxwb {

struct DmxSourceRouteResult final {
    bool accepted{true};
    bool source_changed{false};
    bool physical_publish_attempted{false};
    bool physical_published{false};

    [[nodiscard]] bool ok() const noexcept {
        return accepted &&
               (!physical_publish_attempted || physical_published);
    }

    [[nodiscard]] bool physical_publish_failed() const noexcept {
        return physical_publish_attempted && !physical_published;
    }
};

struct DmxSourceRouterDiagnostics final {
    PersistedSource selected_source{PersistedSource::mqtt};
    std::uint64_t mqtt_snapshots_received{0};
    std::uint64_t artnet_snapshots_received{0};
    std::uint64_t source_switches{0};
    std::uint64_t source_switches_without_snapshot{0};
    std::uint64_t physical_publish_attempts{0};
    std::uint64_t physical_snapshots_published{0};
    std::uint64_t physical_publish_failures{0};
    std::uint64_t rejected_snapshots{0};
    DmxSnapshot::Generation last_physical_generation{0};
    bool artnet_output_active{false};
};

class DmxSourceRouter final {
public:
    using PhysicalPublish = std::function<bool(const DmxSnapshot&)>;

    DmxSourceRouter(
        PersistedSource initial_source,
        PhysicalPublish physical_publish,
        InstrumentationMode instrumentation_mode = InstrumentationMode::engineering);

    DmxSourceRouter(const DmxSourceRouter&) = delete;
    DmxSourceRouter& operator=(const DmxSourceRouter&) = delete;
    DmxSourceRouter(DmxSourceRouter&&) = delete;
    DmxSourceRouter& operator=(DmxSourceRouter&&) = delete;

    [[nodiscard]] DmxSourceRouteResult publish_mqtt_snapshot(
        const DmxSnapshot& snapshot);
    [[nodiscard]] DmxSourceRouteResult publish_artnet_snapshot(
        const DmxSnapshot& snapshot);
    [[nodiscard]] DmxSourceRouteResult select_source(PersistedSource source);

    // Structural Art-Net Universe changes invalidate data from the previous
    // Port-Address. Physical output is intentionally left untouched until the
    // selected source publishes a new whole snapshot.
    void clear_artnet_snapshot() noexcept;

    [[nodiscard]] PersistedSource selected_source() const noexcept;
    [[nodiscard]] bool has_mqtt_snapshot() const noexcept;
    [[nodiscard]] bool has_artnet_snapshot() const noexcept;
    [[nodiscard]] bool artnet_output_active() const noexcept;
    [[nodiscard]] DmxSourceRouterDiagnostics diagnostics() const noexcept;
    [[nodiscard]] InstrumentationMode instrumentation_mode() const noexcept;

private:
    [[nodiscard]] DmxSourceRouteResult cache_snapshot_locked(
        PersistedSource source,
        const DmxSnapshot& snapshot);
    [[nodiscard]] DmxSourceRouteResult publish_cached_locked(
        PersistedSource source,
        bool source_changed);
    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build_physical_snapshot_locked(
        const DmxSnapshot& snapshot);
    [[nodiscard]] std::shared_ptr<const DmxSnapshot>& cached_snapshot_locked(
        PersistedSource source) noexcept;
    [[nodiscard]] const std::shared_ptr<const DmxSnapshot>& cached_snapshot_locked(
        PersistedSource source) const noexcept;
    void update_artnet_output_active_locked() noexcept;

    mutable std::mutex mutex_;
    PhysicalPublish physical_publish_;
    PersistedSource selected_source_{PersistedSource::mqtt};
    std::shared_ptr<const DmxSnapshot> latest_mqtt_;
    std::shared_ptr<const DmxSnapshot> latest_artnet_;
    std::optional<PersistedSource> last_physical_source_;
    DmxSnapshot::Generation next_physical_generation_{1};
    InstrumentationMode instrumentation_mode_{InstrumentationMode::engineering};
    DmxSourceRouterDiagnostics diagnostics_{};
};

}  // namespace dmxwb
