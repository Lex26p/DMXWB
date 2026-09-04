#include "dmxwb/artnet_source_coordinator.hpp"

namespace dmxwb {

ArtNetSourceCoordinator::ArtNetSourceCoordinator(
    ArtNetRuntime& runtime,
    DmxSourceRouter& router,
    InstrumentationMode instrumentation_mode) noexcept
    : runtime_(runtime),
      router_(router),
      instrumentation_mode_(instrumentation_mode) {
    synchronize_output_active();
}

void ArtNetSourceCoordinator::step(time_point now) noexcept {
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.steps;
    }

    // Source may have changed in the Controller/MQTT context since the previous
    // Art-Net tick. Set GoodOutput state before ArtNetRuntime can emit replies.
    synchronize_output_active();

    runtime_.step(now);
    route_latest_snapshot(now);

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

InstrumentationMode ArtNetSourceCoordinator::instrumentation_mode() const noexcept {
    return instrumentation_mode_;
}

void ArtNetSourceCoordinator::route_latest_snapshot(time_point now) noexcept {
    const auto snapshot = runtime_.latest_physical_snapshot();
    if (!snapshot) {
        return;
    }

    const auto generation = snapshot->generation();
    const bool new_generation =
        !last_observed_generation_.has_value() ||
        *last_observed_generation_ != generation;
    if (new_generation) {
        last_observed_generation_ = generation;
        diagnostics_.last_artnet_generation = generation;
        next_route_attempt_.reset();
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.snapshots_observed;
        }
    }

    if (last_routed_generation_.has_value() &&
        *last_routed_generation_ == generation) {
        return;
    }
    if (next_route_attempt_.has_value() && now < *next_route_attempt_) {
        return;
    }

    const auto result = router_.publish_artnet_snapshot(*snapshot);
    if (result.ok()) {
        last_routed_generation_ = generation;
        next_route_attempt_.reset();
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.snapshots_routed;
        }
        return;
    }

    next_route_attempt_ = now + kRouteRetryInterval;
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.route_failures;
    }
}

void ArtNetSourceCoordinator::synchronize_output_active() noexcept {
    const bool active = router_.artnet_output_active();
    runtime_.set_artnet_output_active(active);
    diagnostics_.artnet_output_active = active;
}

}  // namespace dmxwb
