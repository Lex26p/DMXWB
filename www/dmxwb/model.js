export const SOURCE_MQTT = "mqtt";
export const SOURCE_ARTNET = "artnet";

const EMPTY_CONNECTION = Object.freeze({
  state: "offline",
  connected: false,
  url: "",
  attempt: 0,
});

function deepClone(value) {
  if (value === null || value === undefined) {
    return value;
  }
  return JSON.parse(JSON.stringify(value));
}

function finiteInteger(value, fallback = 0) {
  const number = Number(value);
  return Number.isInteger(number) ? number : fallback;
}

export function normalizeSource(value) {
  return value === SOURCE_ARTNET ? SOURCE_ARTNET : SOURCE_MQTT;
}

export function createInitialModel() {
  return {
    connection: { ...EMPTY_CONNECTION },
    config: null,
    configDraft: null,
    configDraftBaseRevision: null,
    configDraftDirty: false,
    state: null,
    status: null,
    groupStates: {},
  };
}

export function setConnection(model, connection) {
  return {
    ...model,
    connection: {
      ...EMPTY_CONNECTION,
      ...deepClone(connection),
      connected: Boolean(connection?.connected),
    },
  };
}

export function setConfigSnapshot(model, config) {
  const snapshot = deepClone(config);
  const revision = finiteInteger(snapshot?.revision, 0);

  if (model.configDraftDirty && model.configDraft) {
    // A newer retained config must never destroy an in-progress local draft.
    // The preserved base revision intentionally makes a later Apply stale,
    // so the backend can reject it with revision_conflict.
    return {
      ...model,
      config: snapshot,
    };
  }

  return {
    ...model,
    config: snapshot,
    configDraft: deepClone(snapshot),
    configDraftBaseRevision: revision,
    configDraftDirty: false,
  };
}

export function resetConfigDraft(model) {
  const revision = model.config
    ? finiteInteger(model.config.revision, 0)
    : null;
  return {
    ...model,
    configDraft: deepClone(model.config),
    configDraftBaseRevision: revision,
    configDraftDirty: false,
  };
}

export function updateConfigDraft(model, updater) {
  if (!model.configDraft || !model.config) {
    return model;
  }

  const draft = deepClone(model.configDraft);
  updater(draft);

  const currentRevision = finiteInteger(model.config.revision, 0);
  const baseRevision = model.configDraftDirty
    ? model.configDraftBaseRevision
    : currentRevision;

  // config.revision is part of the canonical full proposal and must match
  // expected_revision in /dmxwb/config/set.
  draft.revision = baseRevision;

  const dirty = JSON.stringify(draft) !== JSON.stringify(model.config);
  return {
    ...model,
    configDraft: draft,
    configDraftBaseRevision: dirty ? baseRevision : currentRevision,
    configDraftDirty: dirty,
  };
}

export function resizeFixtureDraft(model, fixtureCount) {
  if (!Number.isSafeInteger(fixtureCount) || fixtureCount < 0) {
    return model;
  }

  return updateConfigDraft(model, (draft) => {
    draft.fixtures ??= { count: 0, start_address: 1, items: [] };
    draft.fixtures.items = Array.isArray(draft.fixtures.items)
      ? draft.fixtures.items
      : [];
    draft.id_counters ??= {
      next_fixture_id: 1,
      next_group_id: 1,
      next_scene_id: 1,
    };

    const items = draft.fixtures.items;
    while (items.length < fixtureCount) {
      const id = Number(draft.id_counters.next_fixture_id);
      if (!Number.isSafeInteger(id) || id <= 0) {
        throw new RangeError("next_fixture_id is invalid");
      }
      items.push({ id, name: `Fixture ${id}` });
      draft.id_counters.next_fixture_id = id + 1;
    }

    if (items.length > fixtureCount) {
      items.splice(fixtureCount);
      const activeIds = new Set(items.map((fixture) => String(fixture.id)));
      if (Array.isArray(draft.groups)) {
        for (const group of draft.groups) {
          if (Array.isArray(group.members)) {
            group.members = group.members.filter((id) =>
              activeIds.has(String(id)),
            );
          }
        }
      }
      // Scene snapshots are historical records and intentionally keep
      // stable IDs of Fixtures that were removed.
    }

    draft.fixtures.count = fixtureCount;
  });
}

export function configDraftInfo(model) {
  if (!model.config || !model.configDraft) {
    return null;
  }

  const currentRevision = finiteInteger(model.config.revision, 0);
  const baseRevision = Number.isSafeInteger(model.configDraftBaseRevision)
    ? model.configDraftBaseRevision
    : currentRevision;

  return {
    currentRevision,
    baseRevision,
    dirty: Boolean(model.configDraftDirty),
    stale: Boolean(model.configDraftDirty) && baseRevision !== currentRevision,
    proposal: deepClone(model.configDraft),
  };
}

export function structuralGroupDrafts(model) {
  const draft = model.configDraft;
  if (!draft) {
    return null;
  }

  const fixtures = Array.isArray(draft.fixtures?.items)
    ? draft.fixtures.items.map((fixture) => ({
        id: fixture.id,
        name: fixture.name || `Fixture ${fixture.id}`,
      }))
    : [];

  const groups = Array.isArray(draft.groups)
    ? draft.groups.map((group) => ({
        id: group.id,
        name: group.name || `Group ${group.id}`,
        members: Array.isArray(group.members) ? [...group.members] : [],
      }))
    : [];

  return { fixtures, groups };
}

export function addGroupDraft(model) {
  return updateConfigDraft(model, (draft) => {
    draft.groups = Array.isArray(draft.groups) ? draft.groups : [];
    draft.id_counters ??= {
      next_fixture_id: 1,
      next_group_id: 1,
      next_scene_id: 1,
    };

    const id = Number(draft.id_counters.next_group_id);
    if (!Number.isSafeInteger(id) || id <= 0) {
      throw new RangeError("next_group_id is invalid");
    }

    draft.groups.push({
      id,
      name: `Group ${id}`,
      members: [],
    });
    draft.id_counters.next_group_id = id + 1;
  });
}

export function removeGroupDraft(model, groupIdValue) {
  const groupId = Number(groupIdValue);
  if (!Number.isSafeInteger(groupId) || groupId <= 0) {
    return model;
  }

  return updateConfigDraft(model, (draft) => {
    draft.groups = Array.isArray(draft.groups) ? draft.groups : [];
    draft.groups = draft.groups.filter(
      (group) => Number(group.id) !== groupId,
    );
    // Stable Group IDs are never reused: next_group_id is intentionally kept.
  });
}

export function setGroupDraftMember(
  model,
  groupIdValue,
  fixtureIdValue,
  included,
) {
  const groupId = Number(groupIdValue);
  const fixtureId = Number(fixtureIdValue);
  if (
    !Number.isSafeInteger(groupId) ||
    groupId <= 0 ||
    !Number.isSafeInteger(fixtureId) ||
    fixtureId <= 0
  ) {
    return model;
  }

  return updateConfigDraft(model, (draft) => {
    const fixtures = Array.isArray(draft.fixtures?.items)
      ? draft.fixtures.items
      : [];
    const fixtureOrder = fixtures.map((fixture) => Number(fixture.id));
    if (!fixtureOrder.includes(fixtureId)) {
      throw new RangeError("Group member references missing Fixture");
    }

    draft.groups = Array.isArray(draft.groups) ? draft.groups : [];
    const group = draft.groups.find(
      (candidate) => Number(candidate.id) === groupId,
    );
    if (!group) {
      throw new RangeError("Group draft does not exist");
    }

    const members = new Set(
      Array.isArray(group.members)
        ? group.members.map(Number)
        : [],
    );
    if (included) {
      members.add(fixtureId);
    } else {
      members.delete(fixtureId);
    }

    // Canonicalize membership in current Fixture order: unique and stable.
    group.members = fixtureOrder.filter((id) => members.has(id));
  });
}

export function setStateSnapshot(model, state) {
  return {
    ...model,
    state: deepClone(state),
  };
}

export function setStatusSnapshot(model, status) {
  return {
    ...model,
    status: deepClone(status),
  };
}

export function configRevision(model) {
  return model.config ? finiteInteger(model.config.revision, 0) : null;
}

export function selectedSource(model) {
  return model.state ? normalizeSource(model.state.source) : null;
}

export function fixtureConfigItems(model) {
  const items = model.config?.fixtures?.items;
  return Array.isArray(items) ? items : [];
}

export function fixtureRuntimeItems(model) {
  const items = model.state?.fixtures;
  return Array.isArray(items) ? items : [];
}

export function groupItems(model) {
  const items = model.config?.groups;
  return Array.isArray(items) ? items : [];
}

function parseIntegerRangeText(value, minimum, maximum) {
  if (!/^[0-9]+$/.test(String(value))) {
    return null;
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < minimum || parsed > maximum) {
    return null;
  }
  return parsed;
}

export function setGroupControlState(model, groupIdValue, control, payload) {
  const groupId = Number(groupIdValue);
  if (!Number.isSafeInteger(groupId) || groupId <= 0) {
    return model;
  }

  let value;
  if (control === "name") {
    value = String(payload);
  } else if (control === "power") {
    if (payload !== "0" && payload !== "1") {
      return model;
    }
    value = payload === "1";
  } else if (control === "red" || control === "green" || control === "blue") {
    value = parseIntegerRangeText(payload, 0, 255);
    if (value === null) {
      return model;
    }
  } else if (control === "brightness" || control === "temperature") {
    value = parseIntegerRangeText(payload, 0, 100);
    if (value === null) {
      return model;
    }
  } else if (control === "color") {
    const match = /^([0-9]+);([0-9]+);([0-9]+)$/.exec(String(payload));
    if (!match) {
      return model;
    }
    const components = match.slice(1).map((part) =>
      parseIntegerRangeText(part, 0, 255),
    );
    if (components.some((part) => part === null)) {
      return model;
    }
    value = components;
  } else {
    return model;
  }

  return {
    ...model,
    groupStates: {
      ...model.groupStates,
      [String(groupId)]: {
        ...(model.groupStates[String(groupId)] ?? {}),
        [control]: value,
      },
    },
  };
}

export function groupViewModels(model) {
  const fixtureNames = new Map(
    fixtureConfigItems(model).map((fixture) => [
      String(fixture.id),
      fixture.name || `Fixture ${fixture.id}`,
    ]),
  );

  return groupItems(model).map((group) => {
    const state = model.groupStates[String(group.id)] ?? null;
    const members = Array.isArray(group.members) ? [...group.members] : [];
    return {
      id: group.id,
      name:
        typeof state?.name === "string"
          ? state.name
          : group.name || `Group ${group.id}`,
      members,
      memberNames: members.map(
        (memberId) =>
          fixtureNames.get(String(memberId)) ?? `Fixture ${memberId}`,
      ),
      runtime: state
        ? {
            actualPower:
              typeof state.power === "boolean" ? state.power : null,
            red: Number.isInteger(state.red) ? state.red : null,
            green: Number.isInteger(state.green) ? state.green : null,
            blue: Number.isInteger(state.blue) ? state.blue : null,
            brightness: Number.isInteger(state.brightness)
              ? state.brightness
              : null,
            temperature: Number.isInteger(state.temperature)
              ? state.temperature
              : null,
          }
        : null,
    };
  });
}

export function sceneItems(model) {
  const items = model.config?.scenes;
  return Array.isArray(items) ? items : [];
}

export function sceneViewModels(model) {
  return sceneItems(model).map((scene) => ({
    id: scene.id,
    name: scene.name || `Scene ${scene.id}`,
    fixtures: Array.isArray(scene.fixtures) ? scene.fixtures : [],
    snapshotCount: Array.isArray(scene.fixtures) ? scene.fixtures.length : 0,
  }));
}

export function sceneSnapshotMatchesState(model, scene) {
  if (!scene || !Array.isArray(scene.fixtures)) {
    return false;
  }

  const currentFixtureIds = new Set(
    fixtureConfigItems(model).map((fixture) => String(fixture.id)),
  );
  const runtimeById = new Map(
    fixtureRuntimeItems(model).map((fixture) => [String(fixture.id), fixture]),
  );

  for (const saved of scene.fixtures) {
    const id = String(saved.fixture_id);
    // Deleted Fixture records remain valid Scene history and are ignored by Apply.
    if (!currentFixtureIds.has(id)) {
      continue;
    }

    const runtime = runtimeById.get(id);
    if (!runtime) {
      return false;
    }

    if (
      Boolean(runtime.requested_power) !== Boolean(saved.requested_power) ||
      finiteInteger(runtime.red, -1) !== finiteInteger(saved.red, -2) ||
      finiteInteger(runtime.green, -1) !== finiteInteger(saved.green, -2) ||
      finiteInteger(runtime.blue, -1) !== finiteInteger(saved.blue, -2) ||
      finiteInteger(runtime.white, -1) !== finiteInteger(saved.white, -2) ||
      finiteInteger(runtime.brightness, -1) !==
        finiteInteger(saved.brightness, -2)
    ) {
      return false;
    }
  }

  return true;
}

function clampInteger(value, minimum, maximum, fallback) {
  const parsed = finiteInteger(value, fallback);
  return Math.max(minimum, Math.min(maximum, parsed));
}

export function fixtureViewModels(model) {
  const runtimeById = new Map(
    fixtureRuntimeItems(model).map((item) => [String(item.id), item]),
  );

  return fixtureConfigItems(model).map((fixture, index) => {
    const runtime = runtimeById.get(String(fixture.id)) ?? null;
    return {
      id: fixture.id,
      name: fixture.name || `Fixture ${fixture.id}`,
      startAddress:
        finiteInteger(model.config?.fixtures?.start_address, 1) + index * 4,
      runtime: runtime
        ? {
            requestedPower: Boolean(runtime.requested_power),
            red: clampInteger(runtime.red, 0, 255, 255),
            green: clampInteger(runtime.green, 0, 255, 255),
            blue: clampInteger(runtime.blue, 0, 255, 255),
            white: clampInteger(runtime.white, 0, 255, 255),
            brightness: clampInteger(runtime.brightness, 0, 100, 100),
            temperature: clampInteger(runtime.temperature, 0, 100, 100),
          }
        : null,
    };
  });
}

export function isLegacyArtNetUniverse(value) {
  return finiteInteger(value, -1) === 0;
}

export function structuralSettings(model) {
  const draft = model.configDraft;
  if (!draft) {
    return null;
  }

  return {
    dmxPort: draft.dmx?.port ?? "",
    fixtureCount: finiteInteger(draft.fixtures?.count, 0),
    startAddress: finiteInteger(draft.fixtures?.start_address, 1),
    artnetUniverse: finiteInteger(draft.artnet?.universe, 0),
  };
}

function diagnosticText(value, fallback = "—") {
  if (value === null || value === undefined || value === "") return fallback;
  return String(value);
}
function diagnosticBoolean(value, yes, no) {
  if (value === true) return yes;
  if (value === false) return no;
  return null;
}
function diagnosticInteger(value) {
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : null;
}
function joinDiagnosticParts(parts) {
  return parts.filter((part) => part !== null && part !== "").join(" · ");
}
function summarizeDmx(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return {value: diagnosticText(value), detail: "", severity: "neutral"};
  const refresh=diagnosticInteger(value.active_refresh_hz ?? value.refresh_hz);
  const frames=diagnosticInteger(value.frames_sent), missed=diagnosticInteger(value.missed_deadlines);
  const openFailures=diagnosticInteger(value.open_failures), sendFailures=diagnosticInteger(value.send_failures), recoveries=diagnosticInteger(value.recoveries);
  const running=diagnosticBoolean(value.running ?? value.serial_open,"running","stopped");
  const errors=(openFailures??0)+(sendFailures??0)+(missed??0);
  return {value:joinDiagnosticParts([running,refresh===null?null:`${refresh} Hz`])||"snapshot",detail:joinDiagnosticParts([frames===null?null:`frames ${frames}`,missed===null?null:`missed ${missed}`,recoveries===null?null:`recoveries ${recoveries}`]),severity:errors>0?"error":"ok"};
}
function summarizeMqtt(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return {value: diagnosticText(value), detail: "", severity: "neutral"};
  const connected=diagnosticBoolean(value.connected,"connected","offline");
  const connections=diagnosticInteger(value.successful_connections), disconnects=diagnosticInteger(value.disconnects), callbackFailures=diagnosticInteger(value.callback_failures), commands=diagnosticInteger(value.commands_processed), publishFailures=diagnosticInteger(value.publish_failures);
  const errors=(callbackFailures??0)+(publishFailures??0);
  return {value:connected??"snapshot",detail:joinDiagnosticParts([connections===null?null:`connections ${connections}`,disconnects===null?null:`disconnects ${disconnects}`,commands===null?null:`commands ${commands}`]),severity:connected==="offline"||errors>0?"error":connected==="connected"?"ok":"neutral"};
}
function summarizeArtNet(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return {value: diagnosticText(value), detail: "", severity: "neutral"};
  const state=diagnosticText(value.source_state??value.state,"snapshot"), sync=diagnosticText(value.sync_mode,"");
  const rx=diagnosticInteger(value.datagrams_received??value.packets_received), conflicts=diagnosticInteger(value.conflicts), receiveErrors=diagnosticInteger(value.receive_errors), sendErrors=diagnosticInteger(value.send_errors), bindFailures=diagnosticInteger(value.bind_failures);
  const outputActive=diagnosticBoolean(value.output_active,"output active","output idle");
  const errors=(conflicts??0)+(receiveErrors??0)+(sendErrors??0)+(bindFailures??0);
  return {value:joinDiagnosticParts([state,sync])||"snapshot",detail:joinDiagnosticParts([outputActive,rx===null?null:`rx ${rx}`,conflicts===null?null:`conflicts ${conflicts}`]),severity:errors>0?"error":"ok"};
}
function summarizeConfiguration(value, revision) {
  if (value && typeof value === "object" && !Array.isArray(value)) {
    const state=diagnosticText(value.state??value.status??value.result,"snapshot"), statusRevision=diagnosticInteger(value.revision);
    return {value:state,detail:statusRevision===null?(revision===null?"":`revision ${revision}`):`revision ${statusRevision}`,severity:state==="ok"||state==="loaded"?"ok":state==="fallback"||state==="error"?"error":"neutral"};
  }
  const text=diagnosticText(value);
  return {value:text,detail:revision===null?"":`revision ${revision}`,severity:text==="ok"||text==="loaded"?"ok":text==="fallback"||text==="error"?"error":"neutral"};
}
export function diagnosticSummary(model) {
  const status=model.status??{}; const revision=model.config?finiteInteger(model.config.revision,0):null; const lastError=diagnosticText(status.last_error,"нет");
  return [
    {key:"application",label:"Application",value:diagnosticText(status.application),detail:"",severity:status.application==="running"?"ok":status.application==="error"||status.application==="off"?"error":"neutral"},
    {key:"dmx",label:"DMX",...summarizeDmx(status.dmx)},
    {key:"mqtt",label:"MQTT",...summarizeMqtt(status.mqtt)},
    {key:"artnet",label:"Art-Net",...summarizeArtNet(status.artnet)},
    {key:"configuration",label:"Configuration",...summarizeConfiguration(status.configuration,revision)},
    {key:"last_error",label:"Last error",value:lastError,detail:"",severity:lastError==="нет"?"ok":"error"},
  ];
}
