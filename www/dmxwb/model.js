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
    state: null,
    status: null,
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
  return {
    ...model,
    config: snapshot,
    configDraft: deepClone(snapshot),
  };
}

export function resetConfigDraft(model) {
  return {
    ...model,
    configDraft: deepClone(model.config),
  };
}

export function updateConfigDraft(model, updater) {
  if (!model.configDraft) {
    return model;
  }

  const draft = deepClone(model.configDraft);
  updater(draft);

  return {
    ...model,
    configDraft: draft,
  };
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

export function sceneItems(model) {
  const items = model.config?.scenes;
  return Array.isArray(items) ? items : [];
}

export function fixtureViewModels(model) {
  const runtimeById = new Map(
    fixtureRuntimeItems(model).map((item) => [String(item.id), item]),
  );

  return fixtureConfigItems(model).map((fixture, index) => ({
    id: fixture.id,
    name: fixture.name || `Fixture ${fixture.id}`,
    startAddress: finiteInteger(model.config?.fixtures?.start_address, 1) + index * 4,
    runtime: runtimeById.get(String(fixture.id)) ?? null,
  }));
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

export function diagnosticSummary(model) {
  const status = model.status ?? {};
  return {
    application: status.application ?? "—",
    dmx: status.dmx ?? "—",
    mqtt: status.mqtt ?? "—",
    artnet: status.artnet ?? "—",
  };
}
