#pragma once

#include "dmxwb/artnet_runtime.hpp"
#include "dmxwb/dmx_source_router.hpp"
#include "dmxwb/instrumentation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace dmxwb {

struct ArtNetSourceCoordinatorDiagnostics final {
    std::uint64_t steps{0};
    std::uint64_t snapshots_observed{0};
    std::uint64_t snapshots_routed{0};
    std::uint64_t route_failures{0};
    DmxSnapshot::Generation last_artnet_generation{0};
    bool artnet_output_active{false};
};

// Single-Art-Net-thread bridge between ArtNetRuntime and DmxSourceRouter.
// ArtNetRuntime remains active for both application Sources; every new committed
// Art-Net snapshot is cached in the router, while only the selected Source can
// reach the physical DmxOutput mailbox.
class ArtNetSourceCoordinator final {
public:
    using time_point = ArtNetRuntime::time_point;
    static constexpr auto kRouteRetryInterval = std::chrono::milliseconds{50};

    ArtNetSourceCoordinator(
        ArtNetRuntime& runtime,
        DmxSourceRouter& router,
        InstrumentationMode instrumentation_mode = InstrumentationMode::engineering) noexcept;

    ArtNetSourceCoordinator(const ArtNetSourceCoordinator&) = delete;
    ArtNetSourceCoordinator& operator=(const ArtNetSourceCoordinator&) = delete;
    ArtNetSourceCoordinator(ArtNetSourceCoordinator&&) = delete;
    ArtNetSourceCoordinator& operator=(ArtNetSourceCoordinator&&) = delete;

    void step(time_point now) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const ArtNetSourceCoordinatorDiagnostics& diagnostics() const noexcept;
    [[nodiscard]] InstrumentationMode instrumentation_mode() const noexcept;

private:
    void route_latest_snapshot(time_point now) noexcept;
    void synchronize_output_active() noexcept;

    ArtNetRuntime& runtime_;
    DmxSourceRouter& router_;
    std::optional<DmxSnapshot::Generation> last_observed_generation_;
    std::optional<DmxSnapshot::Generation> last_routed_generation_;
    std::optional<time_point> next_route_attempt_;
    InstrumentationMode instrumentation_mode_{InstrumentationMode::engineering};
    ArtNetSourceCoordinatorDiagnostics diagnostics_{};
};

}  // namespace dmxwb
