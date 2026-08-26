#include "dmxwb/mqtt_runtime.hpp"

#include <utility>

namespace dmxwb {

MqttRuntimeCoordinator::MqttRuntimeCoordinator(
    PersistenceRuntime& persistence,
    MqttCommandQueue& command_queue,
    MqttController& controller,
    MqttRuntimeTransport& transport,
    MqttDmxSnapshotSink& dmx_sink)
    : persistence_(persistence),
      command_queue_(command_queue),
      controller_(controller),
      transport_(transport),
      dmx_sink_(dmx_sink) {}

bool MqttRuntimeCoordinator::publish_initial_snapshot() {
    if (persistence_.source() != PersistedSource::mqtt) {
        return true;
    }

    auto snapshot = controller_.build_current_snapshot();
    if (!snapshot) {
        ++diagnostics_.dmx_publish_failures;
        return false;
    }
    if (!dmx_sink_.publish_snapshot(*snapshot)) {
        ++diagnostics_.dmx_publish_failures;
        return false;
    }
    ++diagnostics_.dmx_snapshots_published;
    return true;
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
    if (persistence_.source() == PersistedSource::mqtt) {
        if (update.snapshot) {
            if (dmx_sink_.publish_snapshot(*update.snapshot)) {
                ++diagnostics_.dmx_snapshots_published;
            } else {
                ++diagnostics_.dmx_publish_failures;
            }
        } else if (command.type == MqttCommandType::set_source &&
                   command.source == PersistedSource::mqtt) {
            publish_current_mqtt_snapshot();
        }
    }

    if (transport_.connected() && !update.publications.empty()) {
        publish_batch(update.publications, false);
    }
}

void MqttRuntimeCoordinator::publish_current_mqtt_snapshot() {
    auto snapshot = controller_.build_current_snapshot();
    if (!snapshot) {
        ++diagnostics_.dmx_publish_failures;
        return;
    }
    if (!dmx_sink_.publish_snapshot(*snapshot)) {
        ++diagnostics_.dmx_publish_failures;
        return;
    }
    ++diagnostics_.dmx_snapshots_published;
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
