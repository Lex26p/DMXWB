#include "dmxwb/mqtt_controller.hpp"

#include <algorithm>
#include <utility>

namespace dmxwb {
namespace {

void append_publications(
    std::vector<MqttPublication>& target,
    std::vector<MqttPublication> source) {
    target.insert(
        target.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
}

}  // namespace

MqttController::MqttController(PersistenceRuntime& runtime)
    : runtime_(runtime) {}

MqttControllerUpdate MqttController::process_command(
    const MqttCommand& command,
    time_point now) {
    if (command.type == MqttCommandType::set_source) {
        runtime_.set_source(command.source, now);

        MqttControllerUpdate result;
        result.applied = true;
        result.publications = build_system_state_publications(
            MqttApplicationStatus::running,
            runtime_.source());
        append_publications(
            result.publications,
            build_internal_snapshot_publications(
                serialize_config_json(runtime_.config()),
                serialize_state_json(runtime_.capture_state()),
                build_status_json(MqttApplicationStatus::running)));
        return result;
    }

    if (command.type == MqttCommandType::fixture_name) {
        return apply_fixture_name(command.fixture_id, command.text, now);
    }

    auto* fixture = find_fixture(command.fixture_id);
    if (fixture == nullptr) {
        return {false, {}, {}, "fixture stable ID does not exist"};
    }
    if (!apply_fixture_command(*fixture, command)) {
        return {false, {}, {}, "fixture command is invalid for the selected control"};
    }

    runtime_.mark_fixture_state_changed(now);
    auto snapshot = build_next_snapshot();
    if (!snapshot) {
        return {false, {}, {}, "cannot build whole MQTT DMX snapshot"};
    }

    MqttControllerUpdate result;
    result.applied = true;
    result.snapshot = std::move(snapshot);
    result.publications = build_state_confirmation(fixture, false);
    return result;
}

std::vector<MqttPublication> MqttController::build_full_republish(
    MqttApplicationStatus status) const {
    std::vector<MqttPublication> output;

    append_publications(output, build_system_metadata_publications());
    append_publications(output, build_system_state_publications(status, runtime_.source()));

    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixtures().fixture_at(index);
        if (fixture == nullptr) {
            continue;
        }
        append_publications(output, build_fixture_metadata_publications(*fixture));
        append_publications(output, build_fixture_state_publications(*fixture));
    }

    append_publications(
        output,
        build_internal_snapshot_publications(
            serialize_config_json(runtime_.config()),
            serialize_state_json(runtime_.capture_state()),
            build_status_json(status)));
    return output;
}

std::shared_ptr<const DmxSnapshot> MqttController::build_current_snapshot() {
    return build_next_snapshot();
}

DmxSnapshot::Generation MqttController::next_generation() const noexcept {
    return generation_;
}

Fixture* MqttController::find_fixture(Fixture::Id id) noexcept {
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        auto* fixture = runtime_.fixture_at(index);
        if (fixture != nullptr && fixture->id() == id) {
            return fixture;
        }
    }
    return nullptr;
}

const Fixture* MqttController::find_fixture(Fixture::Id id) const noexcept {
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixture_at(index);
        if (fixture != nullptr && fixture->id() == id) {
            return fixture;
        }
    }
    return nullptr;
}

bool MqttController::apply_fixture_command(Fixture& fixture, const MqttCommand& command) {
    switch (command.type) {
        case MqttCommandType::fixture_power:
            fixture.set_power(command.boolean_value);
            return true;
        case MqttCommandType::fixture_red:
            fixture.set_red(command.value);
            return true;
        case MqttCommandType::fixture_green:
            fixture.set_green(command.value);
            return true;
        case MqttCommandType::fixture_blue:
            fixture.set_blue(command.value);
            return true;
        case MqttCommandType::fixture_color:
            fixture.set_color(command.color.red, command.color.green, command.color.blue);
            return true;
        case MqttCommandType::fixture_brightness:
            return fixture.set_brightness(command.value);
        case MqttCommandType::fixture_temperature:
            return fixture.set_temperature(command.value);
        case MqttCommandType::fixture_reset:
            fixture.reset();
            return true;
        case MqttCommandType::set_source:
        case MqttCommandType::fixture_name:
            return false;
    }
    return false;
}

MqttControllerUpdate MqttController::apply_fixture_name(
    Fixture::Id fixture_id,
    std::string name,
    time_point now) {
    const auto* fixture = find_fixture(fixture_id);
    if (fixture == nullptr) {
        return {false, {}, {}, "fixture stable ID does not exist"};
    }

    AppConfig proposed = runtime_.config();
    const auto record = std::find_if(
        proposed.fixtures.begin(),
        proposed.fixtures.end(),
        [fixture_id](const FixtureConfigRecord& candidate) {
            return candidate.id == fixture_id;
        });
    if (record == proposed.fixtures.end()) {
        return {false, {}, {}, "fixture config record does not exist"};
    }
    record->name = std::move(name);

    auto committed = runtime_.apply_config_transaction(
        runtime_.config().revision,
        proposed,
        now);
    if (!committed.ok()) {
        return {false, {}, {}, committed.error.message};
    }

    const auto* updated = find_fixture(fixture_id);
    MqttControllerUpdate result;
    result.applied = true;
    result.publications = build_state_confirmation(updated, true);
    return result;
}

std::shared_ptr<const DmxSnapshot> MqttController::build_next_snapshot() {
    auto snapshot = runtime_.fixtures().build_snapshot(generation_);
    if (snapshot) {
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
        }
    }
    return snapshot;
}

std::vector<MqttPublication> MqttController::build_state_confirmation(
    const Fixture* fixture,
    bool include_config) const {
    std::vector<MqttPublication> output;
    if (fixture != nullptr) {
        append_publications(output, build_fixture_state_publications(*fixture));
        if (include_config) {
            append_publications(output, build_fixture_metadata_publications(*fixture));
        }
    }

    if (include_config) {
        output.push_back(MqttPublication{
            std::string{kMqttConfigTopic},
            serialize_config_json(runtime_.config()),
            true});
    }
    output.push_back(MqttPublication{
        std::string{kMqttStateTopic},
        serialize_state_json(runtime_.capture_state()),
        true});
    return output;
}

std::string MqttController::build_status_json(MqttApplicationStatus status) const {
    std::string output{"{\"application\":\""};
    output += mqtt_application_status_name(status);
    output += "\",\"mqtt\":\"controller\",\"configuration\":";
    output += runtime_.startup_status().ok() ? "\"ok\"" : "\"fallback\"";
    output += ",\"last_error\":\"\"}";
    return output;
}

}  // namespace dmxwb
