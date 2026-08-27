#include "dmxwb/artnet_source_coordinator.hpp"

namespace dmxwb {

ArtNetSourceCoordinator::ArtNetSourceCoordinator(
    ArtNetRuntime& runtime,
    DmxSourceRouter& router) noexcept
    : runtime_(runtime),
      router_(router) {
    synchronize_output_active();
}

void ArtNetSourceCoordinator::step(time_point now) noexcept {
    ++diagnostics_.steps;

    // Source may have changed in the Controller/MQTT context since the previous
    // Art-Net tick. Set GoodOutput state before ArtNetRuntime can emit replies.
    synchronize_output_active();

    runtime_.step(now);
    route_latest_snapshot();

    // A newly routed Art-Net snapshot can make ART-NET physically active during
    // this step. Keep subsequent PollReply generation aligned with the router.
    synchronize_output_active();
}

void ArtNetSourceCoordinator::shutdown() noexcept {
    runtime_.shutdown();
}

const ArtNetSourceCoordinatorDiagnostics&
ArtNetSourceCoordinator::diagnostics() const noexcept {
    return diagnostics_;
}

void ArtNetSourceCoordinator::route_latest_snapshot() noexcept {
    const auto snapshot = runtime_.latest_physical_snapshot();
    if (!snapshot) {
        return;
    }

    const auto generation = snapshot->generation();
    if (last_observed_generation_.has_value() &&
        *last_observed_generation_ == generation) {
        return;
    }

    last_observed_generation_ = generation;
    diagnostics_.last_artnet_generation = generation;
    ++diagnostics_.snapshots_observed;

    const auto result = router_.publish_artnet_snapshot(*snapshot);
    if (result.accepted) {
        ++diagnostics_.snapshots_routed;
    }
    if (!result.ok()) {
        ++diagnostics_.route_failures;
    }
}

void ArtNetSourceCoordinator::synchronize_output_active() noexcept {
    const bool active = router_.artnet_output_active();
    runtime_.set_artnet_output_active(active);
    diagnostics_.artnet_output_active = active;
}

}  // namespace dmxwb
