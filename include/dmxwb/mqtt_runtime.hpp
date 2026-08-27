#pragma once

#include "dmxwb/dmx_source_router.hpp"
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
// Все whole MQTT snapshots передаются DmxSourceRouter даже когда Source=ART-NET:
// router хранит background MQTT state, но физически публикует только выбранный
// источник.
class MqttRuntimeCoordinator final {
public:
    using time_point = PersistenceRuntime::time_point;

    MqttRuntimeCoordinator(
        PersistenceRuntime& persistence,
        MqttCommandQueue& command_queue,
        MqttController& controller,
        MqttRuntimeTransport& transport,
        DmxSourceRouter& dmx_router);

    // До запуска physical worker строит текущий whole MQTT snapshot и передаёт
    // его router. При persisted Source=artnet snapshot только кэшируется и не
    // подменяет физический выход.
    [[nodiscard]] bool publish_initial_snapshot();

    // Один неблокирующий Controller tick: resync, queued commands, persistence.
    void step(time_point now);

    [[nodiscard]] StateSaveResult flush_state();
    [[nodiscard]] const MqttRuntimeDiagnostics& diagnostics() const noexcept;

private:
    void publish_controller_update(const MqttCommand& command, MqttControllerUpdate update);
    void record_route_result(const DmxSourceRouteResult& result) noexcept;
    void publish_batch(std::span<const MqttPublication> publications, bool full_republish);

    PersistenceRuntime& persistence_;
    MqttCommandQueue& command_queue_;
    MqttController& controller_;
    MqttRuntimeTransport& transport_;
    DmxSourceRouter& dmx_router_;
    MqttRuntimeDiagnostics diagnostics_{};
};

}  // namespace dmxwb
