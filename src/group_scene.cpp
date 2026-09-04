#include "dmxwb/group_scene.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace dmxwb {

GroupSceneManager::GroupSceneManager(PersistenceRuntime& runtime)
    : runtime_(runtime) {
    synchronize_config();
}

std::optional<GroupControlState> GroupSceneManager::group_state(GroupId group_id) {
    synchronize_config();
    const auto* group = find_group(group_id);
    const auto* stored = find_group_state(group_id);
    if (group == nullptr || stored == nullptr) {
        return std::nullopt;
    }
    return build_group_state(*group, *stored);
}

GroupControlResult GroupSceneManager::apply_group_command(
    const GroupControlCommand& command,
    time_point now) {
    synchronize_config();
    const auto* group = find_group(command.group_id);
    auto* stored = find_group_state(command.group_id);
    if (group == nullptr || stored == nullptr) {
        return {false, false, {}, "group stable ID does not exist"};
    }

    bool fixtures_changed = false;
    for (const auto fixture_id : group->members) {
        auto* fixture = find_fixture(fixture_id);
        if (fixture == nullptr) {
            continue;
        }
        fixtures_changed = true;
        switch (command.type) {
            case GroupControlType::power:
                fixture->set_power(command.boolean_value);
                break;
            case GroupControlType::red:
                fixture->set_red(command.value);
                break;
            case GroupControlType::green:
                fixture->set_green(command.value);
                break;
            case GroupControlType::blue:
                fixture->set_blue(command.value);
                break;
            case GroupControlType::color:
                fixture->set_color(command.color.red, command.color.green, command.color.blue);
                break;
            case GroupControlType::brightness:
                if (!fixture->set_brightness(command.value)) {
                    return {false, false, build_group_state(*group, *stored), "group brightness is outside 0..100"};
                }
                break;
            case GroupControlType::temperature:
                if (!fixture->set_temperature(command.value)) {
                    return {false, false, build_group_state(*group, *stored), "group temperature is outside 0..100"};
                }
                break;
            case GroupControlType::reset:
                fixture->reset();
                break;
        }
    }

    switch (command.type) {
        case GroupControlType::red:
            stored->red = command.value;
            break;
        case GroupControlType::green:
            stored->green = command.value;
            break;
        case GroupControlType::blue:
            stored->blue = command.value;
            break;
        case GroupControlType::color:
            stored->red = command.color.red;
            stored->green = command.color.green;
            stored->blue = command.color.blue;
            break;
        case GroupControlType::brightness:
            if (command.value > 100) {
                return {false, false, build_group_state(*group, *stored), "group brightness is outside 0..100"};
            }
            stored->brightness = command.value;
            break;
        case GroupControlType::temperature:
            if (command.value > 100) {
                return {false, false, build_group_state(*group, *stored), "group temperature is outside 0..100"};
            }
            stored->temperature = command.value;
            break;
        case GroupControlType::power:
        case GroupControlType::reset:
            break;
    }

    if (fixtures_changed) {
        runtime_.mark_fixture_state_changed(now);
    }
    return {true, fixtures_changed, build_group_state(*group, *stored), {}};
}

SceneOperationResult GroupSceneManager::create_scene(
    std::string name,
    time_point now,
    std::optional<std::string> idempotency_request_id) {
    synchronize_config();
    AppConfig proposed = runtime_.config();
    if (proposed.id_counters.next_scene_id == 0 ||
        proposed.id_counters.next_scene_id == std::numeric_limits<SceneId>::max()) {
        return {false, false, false, 0, runtime_.config().revision, "scene ID counter exhausted"};
    }

    const SceneId scene_id = proposed.id_counters.next_scene_id;
    ++proposed.id_counters.next_scene_id;
    const std::string idempotency_name = name;
    proposed.scenes.push_back(SceneConfigRecord{
        scene_id,
        std::move(name),
        capture_scene_snapshot()});
    std::optional<SceneCreateIdempotencyRecord> idempotency_record;
    if (idempotency_request_id.has_value()) {
        idempotency_record = SceneCreateIdempotencyRecord{
            std::move(*idempotency_request_id),
            idempotency_name,
            scene_id,
            runtime_.config().revision + 1U};
    }
    return commit_scene_config(
        std::move(proposed),
        scene_id,
        now,
        std::move(idempotency_record));
}

SceneOperationResult GroupSceneManager::overwrite_scene(SceneId scene_id, time_point now) {
    synchronize_config();
    AppConfig proposed = runtime_.config();
    const auto scene = std::find_if(
        proposed.scenes.begin(),
        proposed.scenes.end(),
        [scene_id](const SceneConfigRecord& candidate) { return candidate.id == scene_id; });
    if (scene == proposed.scenes.end()) {
        return {false, false, false, scene_id, runtime_.config().revision, "scene stable ID does not exist"};
    }
    scene->fixtures = capture_scene_snapshot();
    return commit_scene_config(std::move(proposed), scene_id, now);
}

SceneOperationResult GroupSceneManager::rename_scene(SceneId scene_id, std::string name, time_point now) {
    synchronize_config();
    AppConfig proposed = runtime_.config();
    const auto scene = std::find_if(
        proposed.scenes.begin(),
        proposed.scenes.end(),
        [scene_id](const SceneConfigRecord& candidate) { return candidate.id == scene_id; });
    if (scene == proposed.scenes.end()) {
        return {false, false, false, scene_id, runtime_.config().revision, "scene stable ID does not exist"};
    }
    scene->name = std::move(name);
    return commit_scene_config(std::move(proposed), scene_id, now);
}

SceneOperationResult GroupSceneManager::delete_scene(SceneId scene_id, time_point now) {
    synchronize_config();
    AppConfig proposed = runtime_.config();
    const auto old_size = proposed.scenes.size();
    std::erase_if(proposed.scenes, [scene_id](const SceneConfigRecord& scene) {
        return scene.id == scene_id;
    });
    if (proposed.scenes.size() == old_size) {
        return {false, false, false, scene_id, runtime_.config().revision, "scene stable ID does not exist"};
    }
    return commit_scene_config(std::move(proposed), scene_id, now);
}

SceneOperationResult GroupSceneManager::apply_scene(SceneId scene_id, time_point now) {
    synchronize_config();
    const auto* scene = find_scene(scene_id);
    if (scene == nullptr) {
        return {false, false, false, scene_id, runtime_.config().revision, "scene stable ID does not exist"};
    }

    bool fixtures_changed = false;
    for (const auto& saved : scene->fixtures) {
        auto* fixture = find_fixture(saved.fixture_id);
        if (fixture == nullptr) {
            continue;
        }
        fixtures_changed = true;
        const auto current_temperature = fixture->temperature();
        if (!fixture->restore_state(
                saved.requested_power,
                saved.rgbw,
                saved.brightness,
                current_temperature)) {
            return {false, false, false, scene_id, runtime_.config().revision, "scene snapshot contains invalid Fixture state"};
        }
    }
    if (fixtures_changed) {
        runtime_.mark_fixture_state_changed(now);
    }
    return {true, false, fixtures_changed, scene_id, runtime_.config().revision, {}};
}

void GroupSceneManager::synchronize_config() {
    const auto& config = runtime_.config();
    if (synchronized_revision_ == config.revision && group_states_.size() == config.groups.size()) {
        bool same_ids = true;
        for (const auto& group : config.groups) {
            if (find_group_state(group.id) == nullptr) {
                same_ids = false;
                break;
            }
        }
        if (same_ids) {
            return;
        }
    }

    std::vector<StoredGroupState> reconciled;
    reconciled.reserve(config.groups.size());
    for (const auto& group : config.groups) {
        const auto existing = std::find_if(
            group_states_.begin(),
            group_states_.end(),
            [&group](const StoredGroupState& state) { return state.id == group.id; });
        if (existing != group_states_.end()) {
            reconciled.push_back(*existing);
        } else {
            StoredGroupState fresh;
            fresh.id = group.id;
            reconciled.push_back(fresh);
        }
    }
    group_states_ = std::move(reconciled);
    synchronized_revision_ = config.revision;
}

const GroupConfigRecord* GroupSceneManager::find_group(GroupId group_id) const noexcept {
    const auto& groups = runtime_.config().groups;
    const auto found = std::find_if(
        groups.begin(),
        groups.end(),
        [group_id](const GroupConfigRecord& group) { return group.id == group_id; });
    return found == groups.end() ? nullptr : &*found;
}

const SceneConfigRecord* GroupSceneManager::find_scene(SceneId scene_id) const noexcept {
    const auto& scenes = runtime_.config().scenes;
    const auto found = std::find_if(
        scenes.begin(),
        scenes.end(),
        [scene_id](const SceneConfigRecord& scene) { return scene.id == scene_id; });
    return found == scenes.end() ? nullptr : &*found;
}

Fixture* GroupSceneManager::find_fixture(Fixture::Id fixture_id) noexcept {
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        auto* fixture = runtime_.fixture_at(index);
        if (fixture != nullptr && fixture->id() == fixture_id) {
            return fixture;
        }
    }
    return nullptr;
}

const Fixture* GroupSceneManager::find_fixture(Fixture::Id fixture_id) const noexcept {
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixture_at(index);
        if (fixture != nullptr && fixture->id() == fixture_id) {
            return fixture;
        }
    }
    return nullptr;
}

GroupSceneManager::StoredGroupState* GroupSceneManager::find_group_state(GroupId group_id) noexcept {
    const auto found = std::find_if(
        group_states_.begin(),
        group_states_.end(),
        [group_id](const StoredGroupState& state) { return state.id == group_id; });
    return found == group_states_.end() ? nullptr : &*found;
}

bool GroupSceneManager::group_actual_power(const GroupConfigRecord& group) const noexcept {
    for (const auto fixture_id : group.members) {
        const auto* fixture = find_fixture(fixture_id);
        if (fixture != nullptr && fixture->actual_power()) {
            return true;
        }
    }
    return false;
}

GroupControlState GroupSceneManager::build_group_state(
    const GroupConfigRecord& group,
    const StoredGroupState& stored) const noexcept {
    return GroupControlState{
        group.id,
        group_actual_power(group),
        stored.red,
        stored.green,
        stored.blue,
        stored.brightness,
        stored.temperature};
}

std::vector<SceneFixtureRecord> GroupSceneManager::capture_scene_snapshot() const {
    std::vector<SceneFixtureRecord> snapshot;
    snapshot.reserve(runtime_.fixtures().fixture_count());
    for (std::size_t index = 0; index < runtime_.fixtures().fixture_count(); ++index) {
        const auto* fixture = runtime_.fixture_at(index);
        if (fixture == nullptr) {
            continue;
        }
        snapshot.push_back(SceneFixtureRecord{
            fixture->id(),
            fixture->saved_rgbw(),
            fixture->brightness(),
            fixture->requested_power()});
    }
    return snapshot;
}

SceneOperationResult GroupSceneManager::commit_scene_config(
    AppConfig proposed,
    SceneId scene_id,
    time_point now,
    std::optional<SceneCreateIdempotencyRecord> idempotency_record) {
    const auto committed = runtime_.apply_config_transaction(
        runtime_.config().revision,
        proposed,
        now,
        std::move(idempotency_record));
    if (!committed.ok()) {
        return {false, false, false, scene_id, runtime_.config().revision, committed.error.message};
    }
    synchronize_config();
    return {true, true, false, scene_id, runtime_.config().revision, {}};
}

}  // namespace dmxwb
