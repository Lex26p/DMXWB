#pragma once

#include "dmxwb/mqtt_controller.hpp"

#include <cstdint>
#include <span>

namespace dmxwb {

class MqttRuntimeTransport {
public:
    virtual ~MqttRuntimeTransport() = default;

    [[nodiscard]] virtual bool connected() const noexcept = 0;
    [[nodiscard]] virtual bool publish_all(std::span<const MqttPublication> publications) = 0;
    [[nodiscard]] virtual bool take_full_republish_request() noexcept = 0;
};

class MqttDmxSnapshotSink {
public:
    virtual ~MqttDmxSnapshotSink() = default;
    [[nodiscard]] virtual bool publish_snapshot(const DmxSnapshot& snapshot) = 0;
};

struct MqttRuntimeDiagnostics final {
    std::uint64_t commands_processed{0};
    std::uint64_t commands_rejected{0};
    std::uint64_t dmx_snapshots_published{0};
    std::uint64_t dmx_publish_failures{0};
    std::uint64_t mqtt_publish_batches{0};
    std::uint64_t mqtt_publish_failures{0};
    std::uint64_t full_republishes{0};
    std::uint64_t state_save_failures{0};
};

// Однопоточный orchestration layer между network callback queue и владельцем
// логической модели. Он никогда не вызывается из DmxOutput thread.
class MqttRuntimeCoordinator final {
public:
    using time_point = PersistenceRuntime::time_point;

    MqttRuntimeCoordinator(
        PersistenceRuntime& persistence,
        MqttCommandQueue& command_queue,
        MqttController& controller,
        MqttRuntimeTransport& transport,
        MqttDmxSnapshotSink& dmx_sink);

    // До запуска physical worker можно положить текущий MQTT snapshot в mailbox.
    // При persisted Source=artnet ничего не подменяется MQTT-кадром.
    [[nodiscard]] bool publish_initial_snapshot();

    // Один неблокирующий Controller tick: resync, queued commands, persistence.
    void step(time_point now);

    [[nodiscard]] StateSaveResult flush_state();
    [[nodiscard]] const MqttRuntimeDiagnostics& diagnostics() const noexcept;

private:
    void publish_controller_update(const MqttCommand& command, MqttControllerUpdate update);
    void publish_current_mqtt_snapshot();
    void publish_batch(std::span<const MqttPublication> publications, bool full_republish);

    PersistenceRuntime& persistence_;
    MqttCommandQueue& command_queue_;
    MqttController& controller_;
    MqttRuntimeTransport& transport_;
    MqttDmxSnapshotSink& dmx_sink_;
    MqttRuntimeDiagnostics diagnostics_{};
};

}  // namespace dmxwb
