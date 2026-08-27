#pragma once

#include "dmxwb/artnet_runtime.hpp"
#include "dmxwb/dmx_source_router.hpp"

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

    ArtNetSourceCoordinator(
        ArtNetRuntime& runtime,
        DmxSourceRouter& router) noexcept;

    ArtNetSourceCoordinator(const ArtNetSourceCoordinator&) = delete;
    ArtNetSourceCoordinator& operator=(const ArtNetSourceCoordinator&) = delete;
    ArtNetSourceCoordinator(ArtNetSourceCoordinator&&) = delete;
    ArtNetSourceCoordinator& operator=(ArtNetSourceCoordinator&&) = delete;

    void step(time_point now) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const ArtNetSourceCoordinatorDiagnostics& diagnostics() const noexcept;

private:
    void route_latest_snapshot() noexcept;
    void synchronize_output_active() noexcept;

    ArtNetRuntime& runtime_;
    DmxSourceRouter& router_;
    std::optional<DmxSnapshot::Generation> last_observed_generation_;
    ArtNetSourceCoordinatorDiagnostics diagnostics_{};
};

}  // namespace dmxwb
