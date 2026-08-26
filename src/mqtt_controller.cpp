#include "dmxwb/mqtt_controller.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>
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

[[nodiscard]] bool is_group_control(MqttCommandType type) noexcept {
    switch (type) {
        case MqttCommandType::group_power:
        case MqttCommandType::group_red:
        case MqttCommandType::group_green:
        case MqttCommandType::group_blue:
        case MqttCommandType::group_color:
        case MqttCommandType::group_brightness:
        case MqttCommandType::group_temperature:
        case MqttCommandType::group_reset:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::optional<GroupControlCommand> to_group_command(const MqttCommand& command) {
    GroupControlCommand group;
    group.group_id = command.group_id;
    group.boolean_value = command.boolean_value;
    group.value = command.value;
    group.color = command.color;

    switch (command.type) {
        case MqttCommandType::group_power:
            group.type = GroupControlType::power;
            break;
        case MqttCommandType::group_red:
            group.type = GroupControlType::red;
            break;
        case MqttCommandType::group_green:
            group.type = GroupControlType::green;
            break;
        case MqttCommandType::group_blue:
            group.type = GroupControlType::blue;
            break;
        case MqttCommandType::group_color:
            group.type = GroupControlType::color;
            break;
        case MqttCommandType::group_brightness:
            group.type = GroupControlType::brightness;
            break;
        case MqttCommandType::group_temperature:
            group.type = GroupControlType::temperature;
            break;
        case MqttCommandType::group_reset:
            group.type = GroupControlType::reset;
            break;
        default:
            return std::nullopt;
    }
    return group;
}

}  // namespace

MqttController::MqttController(PersistenceRuntime& runtime)
    : runtime_(runtime),
      group_scene_(runtime) {}

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

    if (command.type == MqttCommandType::set_config) {
        return apply_config_set(command.text, now);
    }
    if (command.type == MqttCommandType::fixture_name) {
        return apply_fixture_name(command.fixture_id, command.text, now);
    }
    if (command.type == MqttCommandType::group_name) {
        return apply_group_name(command.group_id, command.text, now);
    }
    if (is_group_control(command.type)) {
        return apply_group_command(command, now);
    }
    if (command.type == MqttCommandType::scene_name) {
        return apply_scene_name(command.scene_id, command.text, now);
    }
    if (command.type == MqttCommandType::scene_apply) {
        return apply_scene_apply(command.scene_id, now);
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
    append_groups_for_fixture(result.publications, fixture->id());
    return result;
}

std::vector<MqttPublication> MqttController::build_full_republish(
    MqttApplicationStatus status) {
    group_scene_.synchronize_config();
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

    for (const auto& group : runtime_.config().groups) {
        append_publications(output, build_group_metadata_publications(group));
        const auto state = group_scene_.group_state(group.id);
        if (state.has_value()) {
            append_publications(output, build_group_state_publications(group, *state));
        }
    }

    for (const auto& scene : runtime_.config().scenes) {
        append_publications(output, build_scene_metadata_publications(scene));
        append_publications(output, build_scene_state_publications(scene));
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

const GroupConfigRecord* MqttController::find_group(GroupId id) const noexcept {
    const auto& groups = runtime_.config().groups;
    const auto found = std::find_if(
        groups.begin(),
        groups.end(),
        [id](const GroupConfigRecord& group) { return group.id == id; });
    return found == groups.end() ? nullptr : &*found;
}

const SceneConfigRecord* MqttController::find_scene(SceneId id) const noexcept {
    const auto& scenes = runtime_.config().scenes;
    const auto found = std::find_if(
        scenes.begin(),
        scenes.end(),
        [id](const SceneConfigRecord& scene) { return scene.id == id; });
    return found == scenes.end() ? nullptr : &*found;
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
        default:
            return false;
    }
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
        [fixture_id](const FixtureConfigRecord& candidate) { return candidate.id == fixture_id; });
    if (record == proposed.fixtures.end()) {
        return {false, {}, {}, "fixture config record does not exist"};
    }
    record->name = std::move(name);

    auto committed = runtime_.apply_config_transaction(runtime_.config().revision, proposed, now);
    if (!committed.ok()) {
        return {false, {}, {}, committed.error.message};
    }
    group_scene_.synchronize_config();

    const auto* updated = find_fixture(fixture_id);
    MqttControllerUpdate result;
    result.applied = true;
    result.publications = build_state_confirmation(updated, true);
    return result;
}

MqttControllerUpdate MqttController::apply_group_name(
    GroupId group_id,
    std::string name,
    time_point now) {
    if (find_group(group_id) == nullptr) {
        return {false, {}, {}, "group stable ID does not exist"};
    }

    AppConfig proposed = runtime_.config();
    const auto record = std::find_if(
        proposed.groups.begin(),
        proposed.groups.end(),
        [group_id](const GroupConfigRecord& candidate) { return candidate.id == group_id; });
    if (record == proposed.groups.end()) {
        return {false, {}, {}, "group config record does not exist"};
    }
    record->name = std::move(name);

    auto committed = runtime_.apply_config_transaction(runtime_.config().revision, proposed, now);
    if (!committed.ok()) {
        return {false, {}, {}, committed.error.message};
    }
    group_scene_.synchronize_config();

    MqttControllerUpdate result;
    result.applied = true;
    const auto* updated = find_group(group_id);
    const auto state = group_scene_.group_state(group_id);
    if (updated != nullptr) {
        append_publications(result.publications, build_group_metadata_publications(*updated));
        if (state.has_value()) {
            append_publications(result.publications, build_group_state_publications(*updated, *state));
        }
    }
    result.publications.push_back(MqttPublication{
        std::string{kMqttConfigTopic}, serialize_config_json(runtime_.config()), true});
    result.publications.push_back(MqttPublication{
        std::string{kMqttStateTopic}, serialize_state_json(runtime_.capture_state()), true});
    return result;
}

MqttControllerUpdate MqttController::apply_group_command(
    const MqttCommand& command,
    time_point now) {
    const auto mapped = to_group_command(command);
    if (!mapped.has_value()) {
        return {false, {}, {}, "MQTT command is not a Group control"};
    }
    const auto* group = find_group(command.group_id);
    if (group == nullptr) {
        return {false, {}, {}, "group stable ID does not exist"};
    }
    const auto members = group->members;

    const auto applied = group_scene_.apply_group_command(*mapped, now);
    if (!applied.applied) {
        return {false, {}, {}, applied.error};
    }

    MqttControllerUpdate result;
    result.applied = true;
    if (applied.fixture_state_changed) {
        result.snapshot = build_next_snapshot();
        if (!result.snapshot) {
            return {false, {}, {}, "cannot build whole MQTT DMX snapshot after Group command"};
        }
    }

    for (const auto fixture_id : members) {
        const auto* fixture = find_fixture(fixture_id);
        if (fixture != nullptr) {
            append_publications(result.publications, build_fixture_state_publications(*fixture));
        }
    }
    append_all_group_states(result.publications);
    result.publications.push_back(MqttPublication{
        std::string{kMqttStateTopic}, serialize_state_json(runtime_.capture_state()), true});
    return result;
}

MqttControllerUpdate MqttController::apply_scene_name(
    SceneId scene_id,
    std::string name,
    time_point now) {
    const auto renamed = group_scene_.rename_scene(scene_id, std::move(name), now);
    if (!renamed.ok) {
        return {false, {}, {}, renamed.error};
    }

    MqttControllerUpdate result;
    result.applied = true;
    const auto* scene = find_scene(scene_id);
    if (scene != nullptr) {
        append_publications(result.publications, build_scene_metadata_publications(*scene));
        append_publications(result.publications, build_scene_state_publications(*scene));
    }
    result.publications.push_back(MqttPublication{
        std::string{kMqttConfigTopic}, serialize_config_json(runtime_.config()), true});
    result.publications.push_back(MqttPublication{
        std::string{kMqttStateTopic}, serialize_state_json(runtime_.capture_state()), true});
    return result;
}

MqttControllerUpdate MqttController::apply_scene_apply(
    SceneId scene_id,
    time_point now) {
    const auto applied = group_scene_.apply_scene(scene_id, now);
    if (!applied.ok) {
        return {false, {}, {}, applied.error};
    }

    MqttControllerUpdate result;
    result.applied = true;
    if (applied.fixture_state_changed) {
        // GroupSceneManager mutates all matching Fixtures first. Only now is one
        // whole snapshot constructed, so Scene Apply cannot visually iterate.
        result.snapshot = build_next_snapshot();
        if (!result.snapshot) {
            return {false, {}, {}, "cannot build atomic whole DMX snapshot after Scene Apply"};
        }
    }

    append_all_fixture_states(result.publications);
    append_all_group_states(result.publications);
    if (const auto* scene = find_scene(scene_id); scene != nullptr) {
        append_publications(result.publications, build_scene_state_publications(*scene));
    }
    result.publications.push_back(MqttPublication{
        std::string{kMqttStateTopic}, serialize_state_json(runtime_.capture_state()), true});
    return result;
}

MqttControllerUpdate MqttController::apply_config_set(
    std::string_view payload,
    time_point now) {
    const auto parsed = parse_mqtt_config_set_request(payload);
    if (!parsed.ok()) {
        MqttControllerUpdate result;
        result.publications.push_back(build_mqtt_config_result_publication(
            parsed.request_id,
            false,
            runtime_.config().revision,
            parsed.error_code.empty() ? std::string_view{"invalid_request"} : std::string_view{parsed.error_code},
            parsed.message));
        result.error = parsed.message;
        return result;
    }

    const auto request = *parsed.request;
    std::unordered_set<Fixture::Id> previous_fixture_ids;
    std::unordered_set<GroupId> previous_group_ids;
    std::unordered_set<SceneId> previous_scene_ids;
    previous_fixture_ids.reserve(runtime_.config().fixtures.size());
    previous_group_ids.reserve(runtime_.config().groups.size());
    previous_scene_ids.reserve(runtime_.config().scenes.size());
    for (const auto& fixture : runtime_.config().fixtures) previous_fixture_ids.insert(fixture.id);
    for (const auto& group : runtime_.config().groups) previous_group_ids.insert(group.id);
    for (const auto& scene : runtime_.config().scenes) previous_scene_ids.insert(scene.id);

    auto committed = runtime_.apply_config_transaction(
        request.expected_revision,
        request.proposed_config,
        now);
    if (!committed.ok()) {
        MqttControllerUpdate result;
        result.publications.push_back(build_mqtt_config_result_publication(
            request.request_id,
            false,
            runtime_.config().revision,
            mqtt_config_file_error_code_name(committed.error.code),
            committed.error.message));
        result.error = committed.error.message;
        return result;
    }
    group_scene_.synchronize_config();

    MqttControllerUpdate result;
    result.applied = true;
    result.snapshot = build_next_snapshot();
    if (!result.snapshot) {
        result.error = "configuration committed but whole MQTT DMX snapshot could not be built";
    }

    for (const auto& fixture : runtime_.config().fixtures) previous_fixture_ids.erase(fixture.id);
    for (const auto& group : runtime_.config().groups) previous_group_ids.erase(group.id);
    for (const auto& scene : runtime_.config().scenes) previous_scene_ids.erase(scene.id);

    for (const auto removed_id : previous_fixture_ids) {
        append_publications(result.publications, build_fixture_retained_cleanup_publications(removed_id));
    }
    for (const auto removed_id : previous_group_ids) {
        append_publications(result.publications, build_group_retained_cleanup_publications(removed_id));
    }
    for (const auto removed_id : previous_scene_ids) {
        append_publications(result.publications, build_scene_retained_cleanup_publications(removed_id));
    }

    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixtures().fixture_at(index);
        if (fixture == nullptr) continue;
        append_publications(result.publications, build_fixture_metadata_publications(*fixture));
        append_publications(result.publications, build_fixture_state_publications(*fixture));
    }
    for (const auto& group : runtime_.config().groups) {
        append_publications(result.publications, build_group_metadata_publications(group));
        const auto state = group_scene_.group_state(group.id);
        if (state.has_value()) {
            append_publications(result.publications, build_group_state_publications(group, *state));
        }
    }
    for (const auto& scene : runtime_.config().scenes) {
        append_publications(result.publications, build_scene_metadata_publications(scene));
        append_publications(result.publications, build_scene_state_publications(scene));
    }

    append_publications(
        result.publications,
        build_internal_snapshot_publications(
            serialize_config_json(runtime_.config()),
            serialize_state_json(runtime_.capture_state()),
            build_status_json(MqttApplicationStatus::running)));
    result.publications.push_back(build_mqtt_config_result_publication(
        request.request_id,
        true,
        runtime_.config().revision,
        "none",
        "configuration applied"));
    return result;
}

std::shared_ptr<const DmxSnapshot> MqttController::build_next_snapshot() {
    auto snapshot = runtime_.fixtures().build_snapshot(generation_);
    if (snapshot) {
        ++generation_;
        if (generation_ == 0) generation_ = 1;
    }
    return snapshot;
}

std::vector<MqttPublication> MqttController::build_state_confirmation(
    const Fixture* fixture,
    bool include_config) {
    std::vector<MqttPublication> output;
    if (fixture != nullptr) {
        append_publications(output, build_fixture_state_publications(*fixture));
        if (include_config) {
            append_publications(output, build_fixture_metadata_publications(*fixture));
        }
    }
    if (include_config) {
        output.push_back(MqttPublication{
            std::string{kMqttConfigTopic}, serialize_config_json(runtime_.config()), true});
    }
    output.push_back(MqttPublication{
        std::string{kMqttStateTopic}, serialize_state_json(runtime_.capture_state()), true});
    return output;
}

void MqttController::append_all_group_states(std::vector<MqttPublication>& output) {
    group_scene_.synchronize_config();
    for (const auto& group : runtime_.config().groups) {
        const auto state = group_scene_.group_state(group.id);
        if (state.has_value()) {
            append_publications(output, build_group_state_publications(group, *state));
        }
    }
}

void MqttController::append_groups_for_fixture(
    std::vector<MqttPublication>& output,
    Fixture::Id fixture_id) {
    group_scene_.synchronize_config();
    for (const auto& group : runtime_.config().groups) {
        if (std::find(group.members.begin(), group.members.end(), fixture_id) == group.members.end()) {
            continue;
        }
        const auto state = group_scene_.group_state(group.id);
        if (state.has_value()) {
            append_publications(output, build_group_state_publications(group, *state));
        }
    }
}

void MqttController::append_all_fixture_states(std::vector<MqttPublication>& output) const {
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixtures().fixture_at(index);
        if (fixture != nullptr) {
            append_publications(output, build_fixture_state_publications(*fixture));
        }
    }
}

void MqttController::append_all_scene_states(std::vector<MqttPublication>& output) const {
    for (const auto& scene : runtime_.config().scenes) {
        append_publications(output, build_scene_state_publications(scene));
    }
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
