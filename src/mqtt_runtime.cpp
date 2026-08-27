#include "dmxwb/mqtt_runtime.hpp"

#include <utility>

namespace dmxwb {

MqttRuntimeCoordinator::MqttRuntimeCoordinator(
    PersistenceRuntime& persistence,
    MqttCommandQueue& command_queue,
    MqttController& controller,
    MqttRuntimeTransport& transport,
    DmxSourceRouter& dmx_router)
    : persistence_(persistence),
      command_queue_(command_queue),
      controller_(controller),
      transport_(transport),
      dmx_router_(dmx_router) {}

bool MqttRuntimeCoordinator::publish_initial_snapshot() {
    const auto source_result = dmx_router_.select_source(persistence_.source());
    record_route_result(source_result);
    if (!source_result.ok()) {
        return false;
    }

    auto snapshot = controller_.build_current_snapshot();
    if (!snapshot) {
        ++diagnostics_.dmx_publish_failures;
        return false;
    }

    const auto route_result = dmx_router_.publish_mqtt_snapshot(*snapshot);
    record_route_result(route_result);
    return route_result.ok();
}

void MqttRuntimeCoordinator::step(time_point now) {
    if (transport_.take_full_republish_request() && transport_.connected()) {
        const auto publications = controller_.build_full_republish(MqttApplicationStatus::running);
        publish_batch(publications, true);
    }

    while (true) {
        auto command = command_queue_.try_pop();
        if (!command.has_value()) {
            break;
        }

        auto update = controller_.process_command(*command, now);
        if (!update.applied) {
            ++diagnostics_.commands_rejected;
            if (transport_.connected() && !update.publications.empty()) {
                publish_batch(update.publications, false);
            }
            continue;
        }

        ++diagnostics_.commands_processed;
        publish_controller_update(*command, std::move(update));
    }

    const auto save_result = persistence_.save_state_if_due(now);
    if (!save_result.ok()) {
        ++diagnostics_.state_save_failures;
    }
}

StateSaveResult MqttRuntimeCoordinator::flush_state() {
    const auto result = persistence_.flush_state();
    if (!result.ok()) {
        ++diagnostics_.state_save_failures;
    }
    return result;
}

const MqttRuntimeDiagnostics& MqttRuntimeCoordinator::diagnostics() const noexcept {
    return diagnostics_;
}

void MqttRuntimeCoordinator::publish_controller_update(
    const MqttCommand& command,
    MqttControllerUpdate update) {
    if (update.snapshot) {
        record_route_result(dmx_router_.publish_mqtt_snapshot(*update.snapshot));
    }

    if (command.type == MqttCommandType::set_source) {
        record_route_result(dmx_router_.select_source(command.source));
    }

    if (transport_.connected() && !update.publications.empty()) {
        publish_batch(update.publications, false);
    }
}

void MqttRuntimeCoordinator::record_route_result(
    const DmxSourceRouteResult& result) noexcept {
    if (!result.ok()) {
        ++diagnostics_.dmx_publish_failures;
        return;
    }
    if (result.physical_published) {
        ++diagnostics_.dmx_snapshots_published;
    }
}

void MqttRuntimeCoordinator::publish_batch(
    std::span<const MqttPublication> publications,
    bool full_republish) {
    if (publications.empty()) {
        return;
    }
    if (!transport_.publish_all(publications)) {
        ++diagnostics_.mqtt_publish_failures;
        return;
    }
    ++diagnostics_.mqtt_publish_batches;
    if (full_republish) {
        ++diagnostics_.full_republishes;
    }
}

}  // namespace dmxwb
