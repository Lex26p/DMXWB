#pragma once

#include "dmxwb/mqtt_contract.hpp"
#include "dmxwb/persistence_runtime.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dmxwb {

// Результат одной последовательной Controller-команды. Snapshot отделён от
// MQTT publications намеренно: orchestration сначала отдаёт whole snapshot в
// DmxOutput, и только затем публикует подтверждённое MQTT state.
struct MqttControllerUpdate final {
    bool applied{false};
    std::shared_ptr<const DmxSnapshot> snapshot;
    std::vector<MqttPublication> publications;
    std::string error;
};

class MqttController final {
public:
    using time_point = PersistenceRuntime::time_point;

    explicit MqttController(PersistenceRuntime& runtime);

    [[nodiscard]] MqttControllerUpdate process_command(
        const MqttCommand& command,
        time_point now);

    [[nodiscard]] std::vector<MqttPublication> build_full_republish(
        MqttApplicationStatus status = MqttApplicationStatus::running) const;

    // Полный текущий MQTT snapshot нужен startup/re-entry orchestration.
    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build_current_snapshot();

    [[nodiscard]] DmxSnapshot::Generation next_generation() const noexcept;

private:
    [[nodiscard]] Fixture* find_fixture(Fixture::Id id) noexcept;
    [[nodiscard]] const Fixture* find_fixture(Fixture::Id id) const noexcept;
    [[nodiscard]] bool apply_fixture_command(Fixture& fixture, const MqttCommand& command);
    [[nodiscard]] MqttControllerUpdate apply_fixture_name(
        Fixture::Id fixture_id,
        std::string name,
        time_point now);
    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build_next_snapshot();
    [[nodiscard]] std::vector<MqttPublication> build_state_confirmation(
        const Fixture* fixture,
        bool include_config) const;
    [[nodiscard]] std::string build_status_json(MqttApplicationStatus status) const;

    PersistenceRuntime& runtime_;
    DmxSnapshot::Generation generation_{1};
};

}  // namespace dmxwb
