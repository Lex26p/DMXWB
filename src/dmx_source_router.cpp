#include "dmxwb/dmx_source_router.hpp"

#include <cstddef>
#include <utility>

namespace dmxwb {

DmxSourceRouter::DmxSourceRouter(
    PersistedSource initial_source,
    PhysicalPublish physical_publish,
    InstrumentationMode instrumentation_mode)
    : physical_publish_(std::move(physical_publish)),
      selected_source_(initial_source),
      instrumentation_mode_(instrumentation_mode) {
    diagnostics_.selected_source = initial_source;
}

DmxSourceRouteResult DmxSourceRouter::publish_mqtt_snapshot(
    const DmxSnapshot& snapshot) {
    std::scoped_lock lock{mutex_};
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.mqtt_snapshots_received;
    }
    return cache_snapshot_locked(PersistedSource::mqtt, snapshot);
}

DmxSourceRouteResult DmxSourceRouter::publish_artnet_snapshot(
    const DmxSnapshot& snapshot) {
    std::scoped_lock lock{mutex_};
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.artnet_snapshots_received;
    }
    return cache_snapshot_locked(PersistedSource::artnet, snapshot);
}

DmxSourceRouteResult DmxSourceRouter::select_source(PersistedSource source) {
    std::scoped_lock lock{mutex_};

    if (selected_source_ == source) {
        return DmxSourceRouteResult{};
    }

    selected_source_ = source;
    diagnostics_.selected_source = source;
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.source_switches;
    }
    update_artnet_output_active_locked();

    const auto& cached = cached_snapshot_locked(source);
    if (!cached) {
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.source_switches_without_snapshot;
        }
        return DmxSourceRouteResult{
            true,
            true,
            false,
            false};
    }

    return publish_cached_locked(source, true);
}

void DmxSourceRouter::clear_artnet_snapshot() noexcept {
    std::scoped_lock lock{mutex_};
    latest_artnet_.reset();
    if (last_physical_source_.has_value() &&
        *last_physical_source_ == PersistedSource::artnet) {
        last_physical_source_.reset();
    }
    update_artnet_output_active_locked();
}

PersistedSource DmxSourceRouter::selected_source() const noexcept {
    std::scoped_lock lock{mutex_};
    return selected_source_;
}

bool DmxSourceRouter::has_mqtt_snapshot() const noexcept {
    std::scoped_lock lock{mutex_};
    return static_cast<bool>(latest_mqtt_);
}

bool DmxSourceRouter::has_artnet_snapshot() const noexcept {
    std::scoped_lock lock{mutex_};
    return static_cast<bool>(latest_artnet_);
}

bool DmxSourceRouter::artnet_output_active() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_.artnet_output_active;
}

DmxSourceRouterDiagnostics DmxSourceRouter::diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_;
}

InstrumentationMode DmxSourceRouter::instrumentation_mode() const noexcept {
    return instrumentation_mode_;
}

DmxSourceRouteResult DmxSourceRouter::cache_snapshot_locked(
    PersistedSource source,
    const DmxSnapshot& snapshot) {
    if (!is_valid_physical_dmx_slot_count(snapshot.slot_count())) {
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.rejected_snapshots;
        }
        return DmxSourceRouteResult{
            false,
            false,
            false,
            false};
    }

    cached_snapshot_locked(source) = std::make_shared<const DmxSnapshot>(snapshot);

    if (selected_source_ != source) {
        return DmxSourceRouteResult{};
    }

    return publish_cached_locked(source, false);
}

DmxSourceRouteResult DmxSourceRouter::publish_cached_locked(
    PersistedSource source,
    bool source_changed) {
    const auto& cached = cached_snapshot_locked(source);
    if (!cached) {
        return DmxSourceRouteResult{
            true,
            source_changed,
            false,
            false};
    }

    auto physical = build_physical_snapshot_locked(*cached);
    if (!physical) {
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.physical_publish_failures;
        }
        update_artnet_output_active_locked();
        return DmxSourceRouteResult{
            true,
            source_changed,
            true,
            false};
    }

    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.physical_publish_attempts;
    }
    const bool published =
        static_cast<bool>(physical_publish_) &&
        physical_publish_(*physical);

    if (!published) {
        if (engineering_instrumentation_enabled(instrumentation_mode_)) {
            ++diagnostics_.physical_publish_failures;
        }
        update_artnet_output_active_locked();
        return DmxSourceRouteResult{
            true,
            source_changed,
            true,
            false};
    }

    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++diagnostics_.physical_snapshots_published;
    }
    diagnostics_.last_physical_generation = physical->generation();
    last_physical_source_ = source;
    update_artnet_output_active_locked();

    return DmxSourceRouteResult{
        true,
        source_changed,
        true,
        true};
}

std::shared_ptr<const DmxSnapshot> DmxSourceRouter::build_physical_snapshot_locked(
    const DmxSnapshot& snapshot) {
    auto builder = DmxSnapshotBuilder::create(snapshot.slot_count());
    if (!builder.has_value()) {
        return {};
    }

    const auto active = snapshot.active_channels();
    for (std::size_t index = 0; index < active.size(); ++index) {
        if (!builder->set_channel(index + 1, active[index])) {
            return {};
        }
    }

    const auto generation = next_physical_generation_;
    ++next_physical_generation_;
    return builder->build(generation);
}

std::shared_ptr<const DmxSnapshot>& DmxSourceRouter::cached_snapshot_locked(
    PersistedSource source) noexcept {
    return source == PersistedSource::mqtt ? latest_mqtt_ : latest_artnet_;
}

const std::shared_ptr<const DmxSnapshot>& DmxSourceRouter::cached_snapshot_locked(
    PersistedSource source) const noexcept {
    return source == PersistedSource::mqtt ? latest_mqtt_ : latest_artnet_;
}

void DmxSourceRouter::update_artnet_output_active_locked() noexcept {
    diagnostics_.artnet_output_active =
        selected_source_ == PersistedSource::artnet &&
        last_physical_source_.has_value() &&
        *last_physical_source_ == PersistedSource::artnet;
}

}  // namespace dmxwb
