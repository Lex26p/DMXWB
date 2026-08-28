import {
  addGroupDraft,
  configDraftInfo,
  configRevision,
  createInitialModel,
  diagnosticSummary,
  fixtureViewModels,
  groupViewModels,
  isLegacyArtNetUniverse,
  sceneSnapshotMatchesState,
  sceneViewModels,
  selectedSource,
  removeGroupDraft,
  resetConfigDraft,
  resizeFixtureDraft,
  setConfigSnapshot,
  setConnection,
  setGroupControlState,
  setStateSnapshot,
  setStatusSnapshot,
  setGroupDraftMember,
  structuralGroupDrafts,
  structuralSettings,
  updateConfigDraft,
} from "./model.js?v=011g1a";;
import {
  MQTT_CONFIG_RESULT_TOPIC,
  MQTT_CONFIG_SET_TOPIC,
  MQTT_CONFIG_TOPIC,
  MQTT_SCENE_CREATE_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
  MQTT_SYSTEM_SOURCE_COMMAND_TOPIC,
  MqttWebSocketClient,
  fixtureCommandTopic,
  groupCommandTopic,
  groupStateTopics,
  mqttTransportDescriptor,
  parseGroupStateTopic,
  sceneCommandTopic,
  sceneLifecycleTopic,
} from "./mqtt-client.js?v=011g1a";;

let model = createInitialModel();
let fixtureStructureKey = "";
let groupStructureKey = "";
let sceneStructureKey = "";
const fixturePublishers = new Map();
const groupPublishers = new Map();
const LIVE_PUBLISH_INTERVAL_MS = 40; // 25/s, inside the required 20–30/s window.
const LIVE_CONFIRMATION_TIMEOUT_MS = 2000;
const pendingConfirmations = new Map();
let renderFramePending = false;
let sceneRequestCounter = 0;
let configRequestCounter = 0;
let pendingConfigSet = null;
let settingsResult = { kind: "idle", text: "" };
const pendingSceneLifecycle = new Map();
const pendingSceneRenames = new Map();
let pendingSceneApply = null;
let sceneResult = { kind: "idle", text: "" };

function pendingKey(kind, id, control) {
  return `${kind}:${id}:${control}`;
}

function clearPendingConfirmations() {
  for (const entry of pendingConfirmations.values()) {
    window.clearTimeout(entry.timer);
  }
  pendingConfirmations.clear();
}

function setPendingConfirmation(kind, id, control, expected) {
  const key = pendingKey(kind, id, control);
  const previous = pendingConfirmations.get(key);
  if (previous) {
    window.clearTimeout(previous.timer);
  }

  const timer = window.setTimeout(() => {
    const current = pendingConfirmations.get(key);
    if (current?.timer !== timer) {
      return;
    }
    pendingConfirmations.delete(key);
    scheduleRender();
  }, LIVE_CONFIRMATION_TIMEOUT_MS);

  pendingConfirmations.set(key, {
    expected: String(expected),
    timer,
  });
}

function pendingAllowsFactualValue(kind, id, control, factual) {
  const key = pendingKey(kind, id, control);
  const pending = pendingConfirmations.get(key);
  if (!pending) {
    return true;
  }

  if (pending.expected !== String(factual)) {
    return false;
  }

  window.clearTimeout(pending.timer);
  pendingConfirmations.delete(key);
  return true;
}

function scheduleRender() {
  if (renderFramePending) {
    return;
  }
  renderFramePending = true;
  window.requestAnimationFrame(() => {
    renderFramePending = false;
    render();
  });
}

const elements = {
  navButtons: [...document.querySelectorAll("[data-section-target]")],
  sections: [...document.querySelectorAll("[data-section]")],
  mqttEndpoint: document.querySelector("#mqtt-endpoint"),
  configRevision: document.querySelector("#config-revision"),
  stateStatus: document.querySelector("#state-status"),
  sourceBadge: document.querySelector("#source-badge"),
  sourceButtons: [...document.querySelectorAll("[data-source-command]")],
  sourceNote: document.querySelector("#source-note"),
  fixtureCountBadge: document.querySelector("#fixture-count-badge"),
  fixtureList: document.querySelector("#fixture-list"),
  groupList: document.querySelector("#group-list"),
  sceneCountBadge: document.querySelector("#scene-count-badge"),
  sceneList: document.querySelector("#scene-list"),
  sceneCreateName: document.querySelector("#scene-create-name"),
  sceneCreateButton: document.querySelector("#scene-create-button"),
  sceneResult: document.querySelector("#scene-result"),
  diagnosticGrid: document.querySelector("#diagnostic-grid"),
  pageTitle: document.querySelector("#page-title"),
  pageSubtitle: document.querySelector("#page-subtitle"),
  connectionIndicator: document.querySelector("#connection-indicator"),
  connectionTitle: document.querySelector("#connection-title"),
  settings: {
    dmxPort: document.querySelector("#setting-dmx-port"),
    fixtureCount: document.querySelector("#setting-fixture-count"),
    startAddress: document.querySelector("#setting-start-address"),
    artnetUniverse: document.querySelector("#setting-artnet-universe"),
    draftBadge: document.querySelector("#draft-badge"),
    legacyNote: document.querySelector("#legacy-universe-note"),
    result: document.querySelector("#settings-result"),
    groupList: document.querySelector("#settings-group-list"),
    addGroupButton: document.querySelector("#settings-add-group-button"),
    resetButton: document.querySelector("#settings-reset-button"),
    applyButton: document.querySelector("#settings-apply-button"),
  },
};

const sectionMeta = {
  control: ["Управление", "Источник и состояние DMXWB"],
  fixtures: ["Светильники и группы", "Fixture и Group"],
  scenes: ["Сцены", "Scene lifecycle"],
  settings: ["Настройки", "Структурная конфигурация"],
};

function activateSection(sectionName) {
  for (const button of elements.navButtons) {
    button.classList.toggle(
      "is-active",
      button.dataset.sectionTarget === sectionName,
    );
  }

  for (const section of elements.sections) {
    const active = section.dataset.section === sectionName;
    section.classList.toggle("is-active", active);
    section.hidden = !active;
  }

  const meta = sectionMeta[sectionName] ?? sectionMeta.control;
  elements.pageTitle.textContent = meta[0];
  elements.pageSubtitle.textContent = meta[1];
}

function renderConnection() {
  const connection = model.connection;
  const labels = {
    connected: "Связь установлена",
    connecting: "Подключение…",
    reconnecting: "Переподключение…",
    offline: "Нет связи",
  };

  elements.connectionTitle.textContent =
    labels[connection.state] ?? labels.offline;

  elements.connectionIndicator.classList.remove(
    "status-dot--online",
    "status-dot--connecting",
    "status-dot--offline",
  );

  if (connection.connected) {
    elements.connectionIndicator.classList.add("status-dot--online");
  } else if (
    connection.state === "connecting" ||
    connection.state === "reconnecting"
  ) {
    elements.connectionIndicator.classList.add("status-dot--connecting");
  } else {
    elements.connectionIndicator.classList.add("status-dot--offline");
  }
}


function canPublishCommands() {
  return model.connection.connected === true;
}

function publishCommand(topic, payload) {
  if (!canPublishCommands()) {
    return false;
  }
  return mqttClient.publish(topic, String(payload), { retain: false });
}

function createThrottledPublisher(topicFactory) {
  let timer = null;
  let pendingValue = null;
  let lastSentAt = 0;

  const send = (value) => {
    const topic = topicFactory();
    const sent = publishCommand(topic, value);
    if (sent) {
      lastSentAt = performance.now();
    }
    return sent;
  };

  const schedule = (value) => {
    pendingValue = value;
    const elapsed = performance.now() - lastSentAt;
    if (elapsed >= LIVE_PUBLISH_INTERVAL_MS && timer === null) {
      const next = pendingValue;
      pendingValue = null;
      send(next);
      return;
    }

    if (timer !== null) {
      return;
    }

    const delay = Math.max(0, LIVE_PUBLISH_INTERVAL_MS - elapsed);
    timer = window.setTimeout(() => {
      timer = null;
      if (pendingValue === null) {
        return;
      }
      const next = pendingValue;
      pendingValue = null;
      send(next);
    }, delay);
  };

  const final = (value) => {
    if (timer !== null) {
      window.clearTimeout(timer);
      timer = null;
    }
    pendingValue = null;
    // Always publish the final value, even if an identical throttled value
    // was sent immediately before it.
    return send(value);
  };

  return { schedule, final };
}

function fixturePublisher(fixtureId, control) {
  const key = `${fixtureId}:${control}`;
  if (!fixturePublishers.has(key)) {
    fixturePublishers.set(
      key,
      createThrottledPublisher(() => fixtureCommandTopic(fixtureId, control)),
    );
  }
  return fixturePublishers.get(key);
}

function groupPublisher(groupId, control) {
  const key = `${groupId}:${control}`;
  if (!groupPublishers.has(key)) {
    groupPublishers.set(
      key,
      createThrottledPublisher(() => groupCommandTopic(groupId, control)),
    );
  }
  return groupPublishers.get(key);
}

function rgbHex(red, green, blue) {
  const hex = (value) => Number(value).toString(16).padStart(2, "0");
  return `#${hex(red)}${hex(green)}${hex(blue)}`;
}

function hexRgb(value) {
  const match = /^#([0-9a-f]{6})$/i.exec(value);
  if (!match) {
    return null;
  }
  return {
    red: Number.parseInt(match[1].slice(0, 2), 16),
    green: Number.parseInt(match[1].slice(2, 4), 16),
    blue: Number.parseInt(match[1].slice(4, 6), 16),
  };
}

function createRangeControl(label, control, minimum, maximum) {
  const wrapper = document.createElement("label");
  wrapper.className = "fixture-range";

  const heading = document.createElement("span");
  heading.className = "fixture-range__heading";

  const caption = document.createElement("span");
  caption.textContent = label;

  const value = document.createElement("output");
  value.dataset.controlValue = control;
  value.textContent = "—";

  heading.append(caption, value);

  const input = document.createElement("input");
  input.type = "range";
  input.min = String(minimum);
  input.max = String(maximum);
  input.step = "1";
  input.dataset.fixtureControl = control;
  input.disabled = true;

  wrapper.append(heading, input);
  return wrapper;
}

function createFixtureCard(fixture) {
  const card = document.createElement("article");
  card.className = "fixture-card";
  card.dataset.fixtureId = String(fixture.id);

  const header = document.createElement("div");
  header.className = "fixture-card__header";

  const identity = document.createElement("div");
  identity.className = "fixture-card__identity";

  const name = document.createElement("input");
  name.type = "text";
  name.className = "fixture-name";
  name.value = fixture.name;
  name.dataset.fixtureName = "";
  name.setAttribute("aria-label", `Имя светильника ${fixture.id}`);

  const address = document.createElement("span");
  address.className = "fixture-address";
  address.textContent = `DMX ${fixture.startAddress}–${fixture.startAddress + 3}`;

  identity.append(name, address);

  const powerLabel = document.createElement("label");
  powerLabel.className = "fixture-power";
  const power = document.createElement("input");
  power.type = "checkbox";
  power.dataset.fixturePower = "";
  const powerText = document.createElement("span");
  powerText.textContent = "Power";
  powerLabel.append(power, powerText);

  header.append(identity, powerLabel);

  const colorRow = document.createElement("div");
  colorRow.className = "fixture-color-row";

  const colorLabel = document.createElement("label");
  colorLabel.className = "fixture-color-picker";
  const colorCaption = document.createElement("span");
  colorCaption.textContent = "Цвет";
  const color = document.createElement("input");
  color.type = "color";
  color.dataset.fixtureControl = "color";
  color.value = "#ffffff";
  color.disabled = true;
  colorLabel.append(colorCaption, color);

  const reset = document.createElement("button");
  reset.type = "button";
  reset.className = "secondary-button";
  reset.dataset.fixtureReset = "";
  reset.textContent = "Сброс";
  reset.disabled = true;

  colorRow.append(colorLabel, reset);

  const controls = document.createElement("div");
  controls.className = "fixture-controls";
  controls.append(
    createRangeControl("R", "red", 0, 255),
    createRangeControl("G", "green", 0, 255),
    createRangeControl("B", "blue", 0, 255),
    createRangeControl("Яркость", "brightness", 0, 100),
    createRangeControl("Температура", "temperature", 0, 100),
  );

  card.append(header, colorRow, controls);
  return card;
}

function fixtureStructureSignature(fixtures) {
  return fixtures
    .map((fixture) => `${fixture.id}:${fixture.name}:${fixture.startAddress}`)
    .join("|");
}

function rebuildFixtureCards(fixtures) {
  fixturePublishers.clear();
  for (const key of [...pendingConfirmations.keys()]) {
    if (key.startsWith("fixture:")) {
      const pending = pendingConfirmations.get(key);
      window.clearTimeout(pending.timer);
      pendingConfirmations.delete(key);
    }
  }
  elements.fixtureList.replaceChildren();

  if (fixtures.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    const title = document.createElement("strong");
    title.textContent = "Нет светильников";
    const text = document.createElement("span");
    text.textContent = "Добавьте светильники в настройках конфигурации.";
    empty.append(title, text);
    elements.fixtureList.append(empty);
    return;
  }

  for (const fixture of fixtures) {
    elements.fixtureList.append(createFixtureCard(fixture));
  }
}

function setInteractiveValue(
  input,
  value,
  { kind = null, id = null, control = null } = {},
) {
  if (input.dataset.interacting === "1" || document.activeElement === input) {
    return;
  }

  if (
    kind !== null &&
    id !== null &&
    control !== null &&
    !pendingAllowsFactualValue(kind, id, control, value)
  ) {
    return;
  }

  input.value = String(value);
}

function setInteractiveChecked(input, checked, kind, id, control) {
  if (
    input.dataset.interacting === "1" ||
    document.activeElement === input ||
    !pendingAllowsFactualValue(kind, id, control, checked ? "1" : "0")
  ) {
    return;
  }
  input.checked = checked;
}

function updateFixtureCards(fixtures) {
  const connected = canPublishCommands();

  for (const fixture of fixtures) {
    const card = elements.fixtureList.querySelector(
      `[data-fixture-id="${fixture.id}"]`,
    );
    if (!card) {
      continue;
    }

    const runtime = fixture.runtime;
    const name = card.querySelector("[data-fixture-name]");
    const power = card.querySelector("[data-fixture-power]");
    const reset = card.querySelector("[data-fixture-reset]");
    const liveInputs = [...card.querySelectorAll("[data-fixture-control]")];

    name.disabled = !connected;
    reset.disabled = !connected;
    power.disabled = !connected || !runtime;
    for (const input of liveInputs) {
      input.disabled = !connected || !runtime;
    }

    if (!runtime) {
      power.checked = false;
      for (const output of card.querySelectorAll("[data-control-value]")) {
        output.textContent = "—";
      }
      continue;
    }

    setInteractiveChecked(
      power,
      runtime.requestedPower,
      "fixture",
      fixture.id,
      "power",
    );

    const values = {
      red: runtime.red,
      green: runtime.green,
      blue: runtime.blue,
      brightness: runtime.brightness,
      temperature: runtime.temperature,
    };

    for (const [control, value] of Object.entries(values)) {
      const input = card.querySelector(
        `[data-fixture-control="${control}"]`,
      );
      const output = card.querySelector(
        `[data-control-value="${control}"]`,
      );
      setInteractiveValue(input, value, {
        kind: "fixture",
        id: fixture.id,
        control,
      });
      output.textContent = input.value;
    }

    const color = card.querySelector('[data-fixture-control="color"]');
    setInteractiveValue(
      color,
      rgbHex(runtime.red, runtime.green, runtime.blue),
      {
        kind: "fixture",
        id: fixture.id,
        control: "color",
      },
    );
  }
}

function renderFixtureControls(fixtures) {
  const signature = fixtureStructureSignature(fixtures);
  if (signature !== fixtureStructureKey) {
    fixtureStructureKey = signature;
    rebuildFixtureCards(fixtures);
  }
  updateFixtureCards(fixtures);
}


function createGroupRangeControl(label, control, minimum, maximum) {
  const wrapper = document.createElement("label");
  wrapper.className = "fixture-range";

  const heading = document.createElement("span");
  heading.className = "fixture-range__heading";

  const caption = document.createElement("span");
  caption.textContent = label;

  const value = document.createElement("output");
  value.dataset.groupControlValue = control;
  value.textContent = "—";

  heading.append(caption, value);

  const input = document.createElement("input");
  input.type = "range";
  input.min = String(minimum);
  input.max = String(maximum);
  input.step = "1";
  input.dataset.groupControl = control;
  input.disabled = true;

  wrapper.append(heading, input);
  return wrapper;
}

function createGroupCard(group) {
  const card = document.createElement("article");
  card.className = "fixture-card group-card";
  card.dataset.groupId = String(group.id);

  const header = document.createElement("div");
  header.className = "fixture-card__header";

  const identity = document.createElement("div");
  identity.className = "fixture-card__identity";

  const name = document.createElement("input");
  name.type = "text";
  name.className = "fixture-name";
  name.value = group.name;
  name.dataset.groupName = "";
  name.setAttribute("aria-label", `Имя группы ${group.id}`);

  const members = document.createElement("span");
  members.className = "fixture-address group-members";
  members.textContent =
    group.memberNames.length > 0
      ? group.memberNames.join(", ")
      : "Без участников";

  identity.append(name, members);

  const powerLabel = document.createElement("label");
  powerLabel.className = "fixture-power";
  const power = document.createElement("input");
  power.type = "checkbox";
  power.dataset.groupPower = "";
  power.disabled = true;
  const powerText = document.createElement("span");
  powerText.textContent = "Power";
  powerLabel.append(power, powerText);

  header.append(identity, powerLabel);

  const colorRow = document.createElement("div");
  colorRow.className = "fixture-color-row";

  const colorLabel = document.createElement("label");
  colorLabel.className = "fixture-color-picker";
  const colorCaption = document.createElement("span");
  colorCaption.textContent = "Цвет";
  const color = document.createElement("input");
  color.type = "color";
  color.dataset.groupControl = "color";
  color.value = "#ffffff";
  color.disabled = true;
  colorLabel.append(colorCaption, color);

  const reset = document.createElement("button");
  reset.type = "button";
  reset.className = "secondary-button";
  reset.dataset.groupReset = "";
  reset.textContent = "Сброс";
  reset.disabled = true;

  colorRow.append(colorLabel, reset);

  const controls = document.createElement("div");
  controls.className = "fixture-controls";
  controls.append(
    createGroupRangeControl("R", "red", 0, 255),
    createGroupRangeControl("G", "green", 0, 255),
    createGroupRangeControl("B", "blue", 0, 255),
    createGroupRangeControl("Яркость", "brightness", 0, 100),
    createGroupRangeControl("Температура", "temperature", 0, 100),
  );

  card.append(header, colorRow, controls);
  return card;
}

function groupStructureSignature(groups) {
  return groups
    .map(
      (group) =>
        `${group.id}:${group.name}:${group.members.join(",")}`,
    )
    .join("|");
}

function rebuildGroupCards(groups) {
  groupPublishers.clear();
  for (const key of [...pendingConfirmations.keys()]) {
    if (key.startsWith("group:")) {
      const pending = pendingConfirmations.get(key);
      window.clearTimeout(pending.timer);
      pendingConfirmations.delete(key);
    }
  }
  elements.groupList.replaceChildren();

  if (groups.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    const title = document.createElement("strong");
    title.textContent = "Нет групп";
    const text = document.createElement("span");
    text.textContent = "Группы создаются в настройках конфигурации.";
    empty.append(title, text);
    elements.groupList.append(empty);
    return;
  }

  for (const group of groups) {
    elements.groupList.append(createGroupCard(group));
  }
}

function groupRuntimeComplete(runtime) {
  return (
    runtime !== null &&
    typeof runtime.actualPower === "boolean" &&
    Number.isInteger(runtime.red) &&
    Number.isInteger(runtime.green) &&
    Number.isInteger(runtime.blue) &&
    Number.isInteger(runtime.brightness) &&
    Number.isInteger(runtime.temperature)
  );
}

function updateGroupCards(groups) {
  const connected = canPublishCommands();

  for (const group of groups) {
    const card = elements.groupList.querySelector(
      `[data-group-id="${group.id}"]`,
    );
    if (!card) {
      continue;
    }

    const runtime = group.runtime;
    const stateReady = groupRuntimeComplete(runtime);
    const name = card.querySelector("[data-group-name]");
    const power = card.querySelector("[data-group-power]");
    const reset = card.querySelector("[data-group-reset]");
    const liveInputs = [...card.querySelectorAll("[data-group-control]")];

    name.disabled = !connected;
    reset.disabled = !connected;
    power.disabled = !connected || !stateReady;
    for (const input of liveInputs) {
      input.disabled = !connected || !stateReady;
    }

    if (!stateReady) {
      power.checked = false;
      for (const output of card.querySelectorAll(
        "[data-group-control-value]",
      )) {
        output.textContent = "—";
      }
      continue;
    }

    setInteractiveChecked(
      power,
      runtime.actualPower,
      "group",
      group.id,
      "power",
    );

    const values = {
      red: runtime.red,
      green: runtime.green,
      blue: runtime.blue,
      brightness: runtime.brightness,
      temperature: runtime.temperature,
    };

    for (const [control, value] of Object.entries(values)) {
      const input = card.querySelector(
        `[data-group-control="${control}"]`,
      );
      const output = card.querySelector(
        `[data-group-control-value="${control}"]`,
      );
      setInteractiveValue(input, value, {
        kind: "group",
        id: group.id,
        control,
      });
      output.textContent = input.value;
    }

    const color = card.querySelector('[data-group-control="color"]');
    setInteractiveValue(
      color,
      rgbHex(runtime.red, runtime.green, runtime.blue),
      {
        kind: "group",
        id: group.id,
        control: "color",
      },
    );
  }
}

function renderGroupControls(groups) {
  const signature = groupStructureSignature(groups);
  if (signature !== groupStructureKey) {
    groupStructureKey = signature;
    rebuildGroupCards(groups);
  }
  updateGroupCards(groups);
}



function makeConfigRequestId() {
  const uuid = globalThis.crypto?.randomUUID?.();
  if (uuid) {
    return `web-config-${uuid}`;
  }

  configRequestCounter += 1;
  return `web-config-${Date.now().toString(36)}-${configRequestCounter}`;
}

function setSettingsResult(kind, text) {
  settingsResult = { kind, text };
  scheduleRender();
}

function clearPendingConfigSet(reason = "") {
  const hadPending = pendingConfigSet !== null;
  pendingConfigSet = null;
  if (reason && hadPending) {
    settingsResult = { kind: "error", text: reason };
  }
}

function handleConfigSetResult(result) {
  if (!pendingConfigSet || result.request_id !== pendingConfigSet.requestId) {
    return false;
  }

  pendingConfigSet = null;
  if (!result.ok) {
    settingsResult = {
      kind: "error",
      text:
        result.error_code === "revision_conflict"
          ? `Конфликт revision: backend уже revision ${result.revision}. Сбросьте draft или повторите изменения на актуальной конфигурации.`
          : result.message || result.error_code || "Конфигурация отклонена backend.",
    };
    scheduleRender();
    return true;
  }

  model = resetConfigDraft(model);
  settingsResult = {
    kind: "success",
    text: `Конфигурация применена · revision ${result.revision}`,
  };
  scheduleRender();
  return true;
}

function parseUnsignedSetting(input, minimum, maximum) {
  if (!/^[0-9]+$/.test(input.value)) {
    return null;
  }
  const value = Number(input.value);
  if (
    !Number.isSafeInteger(value) ||
    value < minimum ||
    value > maximum
  ) {
    return null;
  }
  return value;
}

function publishConfigDraft() {
  const info = configDraftInfo(model);
  if (!info?.dirty || pendingConfigSet || !canPublishCommands()) {
    return false;
  }

  const requestId = makeConfigRequestId();
  const payload = {
    request_id: requestId,
    expected_revision: info.baseRevision,
    config: info.proposal,
  };

  if (!publishCommand(MQTT_CONFIG_SET_TOPIC, JSON.stringify(payload))) {
    return false;
  }

  pendingConfigSet = {
    requestId,
    expectedRevision: info.baseRevision,
  };
  settingsResult = {
    kind: "pending",
    text: `Ожидание backend · base revision ${info.baseRevision}`,
  };
  scheduleRender();
  return true;
}

function makeSceneRequestId(operation) {
  const uuid = globalThis.crypto?.randomUUID?.();
  if (uuid) {
    return `web-scene-${operation}-${uuid}`;
  }

  sceneRequestCounter += 1;
  return `web-scene-${operation}-${Date.now().toString(36)}-${sceneRequestCounter}`;
}

function setSceneResult(kind, text) {
  sceneResult = { kind, text };
  scheduleRender();
}

function clearScenePending(reason = "") {
  const hadPending =
    pendingSceneRenames.size > 0 ||
    pendingSceneLifecycle.size > 0 ||
    pendingSceneApply !== null;

  for (const pending of pendingSceneRenames.values()) {
    window.clearTimeout(pending.timer);
  }
  pendingSceneRenames.clear();
  pendingSceneLifecycle.clear();
  pendingSceneApply = null;

  if (reason && hadPending) {
    sceneResult = { kind: "error", text: reason };
  }
}

function setPendingSceneRename(sceneId, expected) {
  const old = pendingSceneRenames.get(sceneId);
  if (old) {
    window.clearTimeout(old.timer);
  }

  const timer = window.setTimeout(() => {
    const current = pendingSceneRenames.get(sceneId);
    if (current?.timer !== timer) {
      return;
    }
    pendingSceneRenames.delete(sceneId);
    sceneResult = {
      kind: "error",
      text: "Нет подтверждения нового имени сцены.",
    };
    scheduleRender();
  }, 3000);

  pendingSceneRenames.set(sceneId, {
    expected,
    timer,
  });
}

function confirmSceneRenames(scenes) {
  const byId = new Map(scenes.map((scene) => [scene.id, scene]));
  for (const [sceneId, pending] of [...pendingSceneRenames.entries()]) {
    const factual = byId.get(sceneId);
    if (!factual || factual.name !== pending.expected) {
      continue;
    }
    window.clearTimeout(pending.timer);
    pendingSceneRenames.delete(sceneId);
    sceneResult = {
      kind: "success",
      text: "Имя сцены подтверждено backend.",
    };
  }
}

function parseConfigResult(payload) {
  const result = parseSnapshot(payload);
  if (
    typeof result.request_id !== "string" ||
    typeof result.ok !== "boolean" ||
    !Number.isSafeInteger(result.revision) ||
    result.revision < 0 ||
    typeof result.error_code !== "string" ||
    typeof result.message !== "string"
  ) {
    throw new TypeError("config/result schema mismatch");
  }
  return result;
}

function handleSceneConfigResult(result) {
  const pending = pendingSceneLifecycle.get(result.request_id);
  if (!pending) {
    return;
  }

  pendingSceneLifecycle.delete(result.request_id);
  if (!result.ok) {
    sceneResult = {
      kind: "error",
      text: result.message || result.error_code || "Операция сцены отклонена.",
    };
    scheduleRender();
    return;
  }

  if (pending.operation === "create") {
    elements.sceneCreateName.value = "";
  }

  sceneResult = {
    kind: "success",
    text: `Операция подтверждена · revision ${result.revision}`,
  };
  scheduleRender();
}

function publishSceneLifecycle(topic, payload, operation) {
  if (!canPublishCommands()) {
    return false;
  }

  const requestId = payload.request_id;
  if (!publishCommand(topic, JSON.stringify(payload))) {
    return false;
  }

  pendingSceneLifecycle.set(requestId, { operation });
  sceneResult = {
    kind: "pending",
    text: "Ожидание подтверждения backend…",
  };
  scheduleRender();
  return true;
}

function createSceneCard(scene) {
  const card = document.createElement("article");
  card.className = "scene-card";
  card.dataset.sceneId = String(scene.id);

  const identity = document.createElement("div");
  identity.className = "scene-card__identity";

  const name = document.createElement("input");
  name.type = "text";
  name.className = "scene-name";
  name.dataset.sceneName = "";
  name.setAttribute("aria-label", `Имя сцены ${scene.id}`);

  const meta = document.createElement("span");
  meta.className = "scene-card__meta";
  meta.dataset.sceneMeta = "";

  identity.append(name, meta);

  const actions = document.createElement("div");
  actions.className = "scene-actions";

  for (const [action, title, className] of [
    ["apply", "Применить", "primary-button"],
    ["overwrite", "Перезаписать", "secondary-button"],
    ["delete", "Удалить", "secondary-button scene-delete-button"],
  ]) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = className;
    button.dataset.sceneAction = action;
    button.textContent = title;
    actions.append(button);
  }

  card.append(identity, actions);
  return card;
}

function sceneStructureSignature(scenes) {
  return scenes.map((scene) => scene.id).join("|");
}

function rebuildSceneCards(scenes) {
  elements.sceneList.replaceChildren();

  if (scenes.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state empty-state--wide";
    const title = document.createElement("strong");
    title.textContent = "Нет сцен";
    const text = document.createElement("span");
    text.textContent = "Сохраните текущее состояние как новую сцену.";
    empty.append(title, text);
    elements.sceneList.append(empty);
    return;
  }

  for (const scene of scenes) {
    elements.sceneList.append(createSceneCard(scene));
  }
}

function updateSceneCards(scenes) {
  const connected = canPublishCommands();
  const lifecycleBusy = pendingSceneLifecycle.size > 0;

  for (const scene of scenes) {
    const card = elements.sceneList.querySelector(
      `[data-scene-id="${scene.id}"]`,
    );
    if (!card) {
      continue;
    }

    const name = card.querySelector("[data-scene-name]");
    const meta = card.querySelector("[data-scene-meta]");
    const pendingRename = pendingSceneRenames.get(scene.id);

    if (document.activeElement !== name) {
      name.value = pendingRename?.expected ?? scene.name;
    }
    name.disabled = !connected || lifecycleBusy;

    meta.textContent =
      `${scene.snapshotCount} snapshot ${scene.snapshotCount === 1 ? "Fixture" : "Fixtures"}`;

    for (const button of card.querySelectorAll("[data-scene-action]")) {
      button.disabled = !connected || lifecycleBusy || pendingSceneApply !== null;
    }
  }
}

function renderSceneControls(scenes) {
  confirmSceneRenames(scenes);

  const signature = sceneStructureSignature(scenes);
  if (signature !== sceneStructureKey) {
    sceneStructureKey = signature;
    rebuildSceneCards(scenes);
  }
  updateSceneCards(scenes);

  const connected = canPublishCommands();
  elements.sceneCreateName.disabled = !connected || pendingSceneLifecycle.size > 0;
  elements.sceneCreateButton.disabled =
    !connected || pendingSceneLifecycle.size > 0 || pendingSceneApply !== null;

  elements.sceneResult.classList.toggle(
    "scene-result--error",
    sceneResult.kind === "error",
  );
  elements.sceneResult.classList.toggle(
    "scene-result--success",
    sceneResult.kind === "success",
  );
  elements.sceneResult.textContent = sceneResult.text;
}

function maybeConfirmSceneApply() {
  if (!pendingSceneApply) {
    return;
  }

  if (!sceneSnapshotMatchesState(model, pendingSceneApply.scene)) {
    return;
  }

  pendingSceneApply = null;
  sceneResult = {
    kind: "success",
    text: "Сцена применена и подтверждена runtime state.",
  };
}

function renderSourceControls(source) {
  const connected = canPublishCommands();
  for (const button of elements.sourceButtons) {
    const selected = button.dataset.sourceCommand === source;
    button.classList.toggle("is-selected", selected);
    button.disabled = !connected || source === null;
    button.setAttribute("aria-pressed", selected ? "true" : "false");
  }

  elements.sourceNote.textContent = connected
    ? "Источник переключается явно."
    : "Команды недоступны без связи с DMXWB.";
}

function renderDiagnostics() {
  const diagnostics = diagnosticSummary(model);
  elements.diagnosticGrid.replaceChildren(
    ...diagnostics.map((diagnostic) => {
      const card=document.createElement("article"); card.className="metric-card"; card.dataset.diagnostic=diagnostic.key;
      card.classList.toggle("metric-card--ok", diagnostic.severity === "ok");
      card.classList.toggle("metric-card--error", diagnostic.severity === "error");
      const caption=document.createElement("span"); caption.textContent=diagnostic.label;
      const strong=document.createElement("strong"); strong.textContent=diagnostic.value; card.append(caption,strong);
      if (diagnostic.detail) { const detail=document.createElement("small"); detail.className="metric-card__detail"; detail.textContent=diagnostic.detail; card.append(detail); }
      return card;
    }),
  );
}


function createSettingsGroupCard(group, fixtures) {
  const card = document.createElement("article");
  card.className = "settings-group-card";
  card.dataset.settingsGroupId = String(group.id);

  const header = document.createElement("div");
  header.className = "settings-group-card__header";

  const identity = document.createElement("div");
  const name = document.createElement("strong");
  name.textContent = group.name;
  const id = document.createElement("span");
  id.className = "settings-group-card__id";
  id.textContent = `ID ${group.id}`;
  identity.append(name, id);

  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "secondary-button settings-group-remove";
  remove.dataset.settingsGroupRemove = "";
  remove.textContent = "Удалить";

  header.append(identity, remove);

  const members = document.createElement("div");
  members.className = "settings-group-members";

  if (fixtures.length === 0) {
    const empty = document.createElement("span");
    empty.className = "field-note";
    empty.textContent = "Нет Fixture для добавления в группу.";
    members.append(empty);
  } else {
    const selected = new Set(group.members.map(String));
    for (const fixture of fixtures) {
      const label = document.createElement("label");
      label.className = "settings-member-option";

      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.dataset.settingsGroupMember = String(fixture.id);
      checkbox.checked = selected.has(String(fixture.id));

      const text = document.createElement("span");
      text.textContent = fixture.name;

      label.append(checkbox, text);
      members.append(label);
    }
  }

  card.append(header, members);
  return card;
}

function renderSettingsGroupEditor() {
  const editor = structuralGroupDrafts(model);
  const disabled = pendingConfigSet !== null;

  elements.settings.addGroupButton.disabled = !editor || disabled;
  elements.settings.groupList.replaceChildren();

  if (!editor || editor.groups.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    const title = document.createElement("strong");
    title.textContent = "Нет групп";
    const text = document.createElement("span");
    text.textContent = "Добавьте группу и выберите её Fixture.";
    empty.append(title, text);
    elements.settings.groupList.append(empty);
    return;
  }

  for (const group of editor.groups) {
    const card = createSettingsGroupCard(group, editor.fixtures);
    for (const input of card.querySelectorAll(
      "[data-settings-group-member]",
    )) {
      input.disabled = disabled;
    }
    const remove = card.querySelector("[data-settings-group-remove]");
    remove.disabled = disabled;
    elements.settings.groupList.append(card);
  }
}

function renderSettings() {
  const settings = structuralSettings(model);
  const info = configDraftInfo(model);
  const fields = elements.settings;

  if (!settings || !info) {
    renderSettingsGroupEditor();
    fields.dmxPort.value = "/dev/ttyRS485-1";
    fields.fixtureCount.value = "";
    fields.startAddress.value = "";
    fields.artnetUniverse.value = "";
    fields.draftBadge.textContent = "нет конфигурации";
    fields.legacyNote.textContent = "";
    fields.result.textContent = "";
    fields.applyButton.disabled = true;
    fields.resetButton.disabled = true;
    for (const input of [
      fields.dmxPort,
      fields.fixtureCount,
      fields.startAddress,
      fields.artnetUniverse,
    ]) {
      input.disabled = true;
    }
    return;
  }

  renderSettingsGroupEditor();

  fields.dmxPort.value = settings.dmxPort;
  fields.fixtureCount.value = String(settings.fixtureCount);
  fields.startAddress.value = String(settings.startAddress);
  fields.artnetUniverse.value = String(settings.artnetUniverse);

  for (const input of [
    fields.dmxPort,
    fields.fixtureCount,
    fields.startAddress,
    fields.artnetUniverse,
  ]) {
    input.disabled = pendingConfigSet !== null;
  }

  if (!info.dirty) {
    fields.draftBadge.textContent =
      `без изменений · revision ${info.currentRevision}`;
  } else if (info.stale) {
    fields.draftBadge.textContent =
      `draft · base ${info.baseRevision} / current ${info.currentRevision}`;
  } else {
    fields.draftBadge.textContent = `draft · revision ${info.baseRevision}`;
  }

  fields.legacyNote.textContent = isLegacyArtNetUniverse(settings.artnetUniverse)
    ? "Art-Net Universe 0 — legacy compatibility."
    : "";

  fields.result.classList.toggle(
    "settings-result--error",
    settingsResult.kind === "error",
  );
  fields.result.classList.toggle(
    "settings-result--success",
    settingsResult.kind === "success",
  );
  fields.result.textContent = settingsResult.text;

  fields.applyButton.disabled =
    !info.dirty || pendingConfigSet !== null || !canPublishCommands();
  fields.resetButton.disabled = !info.dirty || pendingConfigSet !== null;
}

function render() {
  
const descriptor = mqttTransportDescriptor(window.location);
  const revision = configRevision(model);
  const source = selectedSource(model);
  const fixtures = fixtureViewModels(model);
  const groups = groupViewModels(model);
  const scenes = sceneViewModels(model);

  elements.mqttEndpoint.textContent = descriptor.url;
  elements.configRevision.textContent =
    revision === null ? "нет snapshot" : `revision ${revision}`;
  elements.stateStatus.textContent = model.state ? "получен" : "нет snapshot";
  elements.sourceBadge.textContent =
    source === null ? "Source: —" : `Source: ${source}`;

  elements.fixtureCountBadge.textContent =
    `${fixtures.length} светильников · ${groups.length} групп`;
  elements.sceneCountBadge.textContent = `${scenes.length} сцен`;

  renderConnection();
  renderSourceControls(source);
  renderFixtureControls(fixtures);
  renderGroupControls(groups);
  renderSceneControls(scenes);
  renderDiagnostics();
  renderSettings();
}

function parseSnapshot(payload) {
  const value = JSON.parse(payload);
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError("MQTT snapshot must be a JSON object");
  }
  return value;
}

function applyMqttMessage(topic, payload) {
  if (topic === MQTT_CONFIG_RESULT_TOPIC) {
    const result = parseConfigResult(payload);
    if (!handleConfigSetResult(result)) {
      handleSceneConfigResult(result);
    }
    return;
  }

  if (
    topic === MQTT_CONFIG_TOPIC ||
    topic === MQTT_STATE_TOPIC ||
    topic === MQTT_STATUS_TOPIC
  ) {
    const snapshot = parseSnapshot(payload);

    if (topic === MQTT_CONFIG_TOPIC) {
      model = setConfigSnapshot(model, snapshot);
      const groups = Array.isArray(snapshot.groups) ? snapshot.groups : [];
      mqttClient.subscribe(
        groups.flatMap((group) => {
          try {
            return groupStateTopics(group.id);
          } catch {
            return [];
          }
        }),
      );
    } else if (topic === MQTT_STATE_TOPIC) {
      model = setStateSnapshot(model, snapshot);
      maybeConfirmSceneApply();
    } else {
      model = setStatusSnapshot(model, snapshot);
    }

    scheduleRender();
    return;
  }

  const groupState = parseGroupStateTopic(topic);
  if (groupState) {
    model = setGroupControlState(
      model,
      groupState.groupId,
      groupState.control,
      payload,
    );
    scheduleRender();
  }
}

for (const button of elements.navButtons) {
  button.addEventListener("click", () => {
    activateSection(button.dataset.sectionTarget);
  });
}

for (const button of elements.sourceButtons) {
  button.addEventListener("click", () => {
    publishCommand(
      MQTT_SYSTEM_SOURCE_COMMAND_TOPIC,
      button.dataset.sourceCommand,
    );
  });
}




elements.settings.addGroupButton.addEventListener("click", () => {
  if (pendingConfigSet) {
    return;
  }

  try {
    model = addGroupDraft(model);
    settingsResult = { kind: "idle", text: "" };
    scheduleRender();
  } catch (error) {
    setSettingsResult("error", String(error?.message ?? error));
  }
});

elements.settings.groupList.addEventListener("change", (event) => {
  const checkbox = event.target.closest("[data-settings-group-member]");
  if (!checkbox || pendingConfigSet) {
    return;
  }

  const card = checkbox.closest("[data-settings-group-id]");
  if (!card) {
    return;
  }

  try {
    model = setGroupDraftMember(
      model,
      Number(card.dataset.settingsGroupId),
      Number(checkbox.dataset.settingsGroupMember),
      checkbox.checked,
    );
    settingsResult = { kind: "idle", text: "" };
    scheduleRender();
  } catch (error) {
    setSettingsResult("error", String(error?.message ?? error));
  }
});

elements.settings.groupList.addEventListener("click", (event) => {
  const remove = event.target.closest("[data-settings-group-remove]");
  if (!remove || remove.disabled || pendingConfigSet) {
    return;
  }

  const card = remove.closest("[data-settings-group-id]");
  if (!card) {
    return;
  }

  model = removeGroupDraft(
    model,
    Number(card.dataset.settingsGroupId),
  );
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.dmxPort.addEventListener("change", () => {
  model = updateConfigDraft(model, (draft) => {
    draft.dmx ??= {};
    draft.dmx.port = elements.settings.dmxPort.value;
  });
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.fixtureCount.addEventListener("input", () => {
  const count = parseUnsignedSetting(elements.settings.fixtureCount, 0, 75);
  if (count === null) {
    return;
  }
  try {
    model = resizeFixtureDraft(model, count);
    settingsResult = { kind: "idle", text: "" };
    scheduleRender();
  } catch (error) {
    setSettingsResult("error", String(error?.message ?? error));
  }
});

elements.settings.startAddress.addEventListener("input", () => {
  const value = parseUnsignedSetting(elements.settings.startAddress, 1, 300);
  if (value === null) {
    return;
  }
  model = updateConfigDraft(model, (draft) => {
    draft.fixtures ??= {};
    draft.fixtures.start_address = value;
  });
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.artnetUniverse.addEventListener("input", () => {
  const value = parseUnsignedSetting(
    elements.settings.artnetUniverse,
    0,
    32767,
  );
  if (value === null) {
    return;
  }
  model = updateConfigDraft(model, (draft) => {
    draft.artnet ??= {};
    draft.artnet.universe = value;
  });
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.resetButton.addEventListener("click", () => {
  if (pendingConfigSet) {
    return;
  }
  model = resetConfigDraft(model);
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.applyButton.addEventListener("click", () => {
  publishConfigDraft();
});

elements.sceneCreateButton.addEventListener("click", () => {
  if (!canPublishCommands()) {
    return;
  }

  const requestId = makeSceneRequestId("create");
  publishSceneLifecycle(
    MQTT_SCENE_CREATE_TOPIC,
    {
      request_id: requestId,
      name: elements.sceneCreateName.value,
    },
    "create",
  );
});

elements.sceneCreateName.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !elements.sceneCreateButton.disabled) {
    event.preventDefault();
    elements.sceneCreateButton.click();
  }
});

elements.sceneList.addEventListener("change", (event) => {
  const name = event.target.closest("[data-scene-name]");
  if (!name || !canPublishCommands()) {
    return;
  }

  const card = name.closest("[data-scene-id]");
  if (!card) {
    return;
  }
  const sceneId = Number(card.dataset.sceneId);

  if (publishCommand(sceneCommandTopic(sceneId, "name"), name.value)) {
    setPendingSceneRename(sceneId, name.value);
    sceneResult = {
      kind: "pending",
      text: "Ожидание подтверждения нового имени…",
    };
    scheduleRender();
  }
});

elements.sceneList.addEventListener("click", (event) => {
  const button = event.target.closest("[data-scene-action]");
  if (!button || button.disabled || !canPublishCommands()) {
    return;
  }

  const card = button.closest("[data-scene-id]");
  if (!card) {
    return;
  }

  const sceneId = Number(card.dataset.sceneId);
  const scene = sceneViewModels(model).find((item) => item.id === sceneId);
  if (!scene) {
    return;
  }

  const action = button.dataset.sceneAction;
  if (action === "apply") {
    if (publishCommand(sceneCommandTopic(sceneId, "apply"), "1")) {
      pendingSceneApply = {
        scene: {
          ...scene,
          fixtures: scene.fixtures.map((fixture) => ({ ...fixture })),
        },
      };
      sceneResult = {
        kind: "pending",
        text: "Ожидание подтверждения runtime state…",
      };
      scheduleRender();
    }
    return;
  }

  if (action === "overwrite") {
    if (!window.confirm(`Перезаписать сцену «${scene.name}» текущим состоянием?`)) {
      return;
    }
    const requestId = makeSceneRequestId("overwrite");
    publishSceneLifecycle(
      sceneLifecycleTopic(sceneId, "overwrite"),
      { request_id: requestId },
      "overwrite",
    );
    return;
  }

  if (action === "delete") {
    if (!window.confirm(`Удалить сцену «${scene.name}»?`)) {
      return;
    }
    const requestId = makeSceneRequestId("delete");
    publishSceneLifecycle(
      sceneLifecycleTopic(sceneId, "delete"),
      { request_id: requestId },
      "delete",
    );
  }
});

elements.groupList.addEventListener("pointerdown", (event) => {
  const input = event.target.closest("[data-group-control]");
  if (input) {
    input.dataset.interacting = "1";
  }
});

elements.groupList.addEventListener("pointercancel", (event) => {
  const input = event.target.closest("[data-group-control]");
  if (input) {
    delete input.dataset.interacting;
  }
});

elements.groupList.addEventListener("input", (event) => {
  const input = event.target.closest("[data-group-control]");
  if (!input || input.disabled) {
    return;
  }

  const card = input.closest("[data-group-id]");
  if (!card) {
    return;
  }
  const groupId = Number(card.dataset.groupId);
  const control = input.dataset.groupControl;

  if (control === "color") {
    const rgb = hexRgb(input.value);
    if (!rgb) {
      return;
    }
    groupPublisher(groupId, control).schedule(
      `${rgb.red};${rgb.green};${rgb.blue}`,
    );
    return;
  }

  const output = card.querySelector(
    `[data-group-control-value="${control}"]`,
  );
  if (output) {
    output.textContent = input.value;
  }
  groupPublisher(groupId, control).schedule(input.value);
});

elements.groupList.addEventListener("change", (event) => {
  const card = event.target.closest("[data-group-id]");
  if (!card || !canPublishCommands()) {
    return;
  }
  const groupId = Number(card.dataset.groupId);

  const liveInput = event.target.closest("[data-group-control]");
  if (liveInput) {
    const control = liveInput.dataset.groupControl;
    delete liveInput.dataset.interacting;

    if (control === "color") {
      const rgb = hexRgb(liveInput.value);
      if (rgb) {
        const expected = `${rgb.red};${rgb.green};${rgb.blue}`;
        if (groupPublisher(groupId, control).final(expected)) {
          setPendingConfirmation("group", groupId, "color", liveInput.value);
          setPendingConfirmation("group", groupId, "red", rgb.red);
          setPendingConfirmation("group", groupId, "green", rgb.green);
          setPendingConfirmation("group", groupId, "blue", rgb.blue);
        }
      }
    } else {
      if (groupPublisher(groupId, control).final(liveInput.value)) {
        setPendingConfirmation(
          "group",
          groupId,
          control,
          liveInput.value,
        );
      }
    }
    return;
  }

  const power = event.target.closest("[data-group-power]");
  if (power) {
    const expected = power.checked ? "1" : "0";
    if (publishCommand(groupCommandTopic(groupId, "power"), expected)) {
      setPendingConfirmation("group", groupId, "power", expected);
    }
    return;
  }

  const name = event.target.closest("[data-group-name]");
  if (name) {
    publishCommand(groupCommandTopic(groupId, "name"), name.value);
  }
});

elements.groupList.addEventListener(
  "blur",
  (event) => {
    const liveInput = event.target.closest?.("[data-group-control]");
    if (liveInput) {
      delete liveInput.dataset.interacting;
    }
  },
  true,
);

elements.groupList.addEventListener("click", (event) => {
  const reset = event.target.closest("[data-group-reset]");
  if (!reset || reset.disabled || !canPublishCommands()) {
    return;
  }

  const card = reset.closest("[data-group-id]");
  if (!card) {
    return;
  }

  publishCommand(
    groupCommandTopic(Number(card.dataset.groupId), "reset"),
    "1",
  );
});

elements.fixtureList.addEventListener("pointerdown", (event) => {
  const input = event.target.closest("[data-fixture-control]");
  if (input) {
    input.dataset.interacting = "1";
  }
});

elements.fixtureList.addEventListener("pointercancel", (event) => {
  const input = event.target.closest("[data-fixture-control]");
  if (input) {
    delete input.dataset.interacting;
  }
});

elements.fixtureList.addEventListener("input", (event) => {
  const input = event.target.closest("[data-fixture-control]");
  if (!input || input.disabled) {
    return;
  }

  const card = input.closest("[data-fixture-id]");
  if (!card) {
    return;
  }
  const fixtureId = Number(card.dataset.fixtureId);
  const control = input.dataset.fixtureControl;

  if (control === "color") {
    const rgb = hexRgb(input.value);
    if (!rgb) {
      return;
    }
    fixturePublisher(fixtureId, control).schedule(
      `${rgb.red};${rgb.green};${rgb.blue}`,
    );
    return;
  }

  const output = card.querySelector(`[data-control-value="${control}"]`);
  if (output) {
    output.textContent = input.value;
  }
  fixturePublisher(fixtureId, control).schedule(input.value);
});

elements.fixtureList.addEventListener("change", (event) => {
  const card = event.target.closest("[data-fixture-id]");
  if (!card || !canPublishCommands()) {
    return;
  }
  const fixtureId = Number(card.dataset.fixtureId);

  const liveInput = event.target.closest("[data-fixture-control]");
  if (liveInput) {
    const control = liveInput.dataset.fixtureControl;
    delete liveInput.dataset.interacting;

    if (control === "color") {
      const rgb = hexRgb(liveInput.value);
      if (rgb) {
        const expected = `${rgb.red};${rgb.green};${rgb.blue}`;
        if (fixturePublisher(fixtureId, control).final(expected)) {
          setPendingConfirmation(
            "fixture",
            fixtureId,
            "color",
            liveInput.value,
          );
          setPendingConfirmation("fixture", fixtureId, "red", rgb.red);
          setPendingConfirmation("fixture", fixtureId, "green", rgb.green);
          setPendingConfirmation("fixture", fixtureId, "blue", rgb.blue);
        }
      }
    } else {
      if (fixturePublisher(fixtureId, control).final(liveInput.value)) {
        setPendingConfirmation(
          "fixture",
          fixtureId,
          control,
          liveInput.value,
        );
      }
    }
    return;
  }

  const power = event.target.closest("[data-fixture-power]");
  if (power) {
    const expected = power.checked ? "1" : "0";
    if (publishCommand(fixtureCommandTopic(fixtureId, "power"), expected)) {
      setPendingConfirmation("fixture", fixtureId, "power", expected);
    }
    return;
  }

  const name = event.target.closest("[data-fixture-name]");
  if (name) {
    publishCommand(fixtureCommandTopic(fixtureId, "name"), name.value);
  }
});

elements.fixtureList.addEventListener("blur", (event) => {
  const liveInput = event.target.closest?.("[data-fixture-control]");
  if (liveInput) {
    delete liveInput.dataset.interacting;
  }
}, true);

elements.fixtureList.addEventListener("click", (event) => {
  const reset = event.target.closest("[data-fixture-reset]");
  if (!reset || reset.disabled || !canPublishCommands()) {
    return;
  }

  const card = reset.closest("[data-fixture-id]");
  if (!card) {
    return;
  }

  publishCommand(
    fixtureCommandTopic(Number(card.dataset.fixtureId), "reset"),
    "1",
  );
});

const descriptor = mqttTransportDescriptor(window.location);
const mqttClient = new MqttWebSocketClient({
  url: descriptor.url,
  onConnectionChange(connection) {
    model = setConnection(model, connection);
    if (!connection.connected) {
      clearPendingConfirmations();
      clearPendingConfigSet(
        "Изменение конфигурации не подтверждено: связь потеряна.",
      );
      clearScenePending("Команда сцены не подтверждена: связь потеряна.");
    }
    scheduleRender();
  },
  onMessage(topic, payload) {
    try {
      applyMqttMessage(topic, payload);
    } catch (error) {
      console.error(`DMXWB ignored invalid MQTT snapshot from ${topic}`, error);
    }
  },
  onProtocolError(error) {
    console.error("DMXWB MQTT transport error", error);
  },
});

mqttClient.subscribe([
  MQTT_CONFIG_TOPIC,
  MQTT_CONFIG_RESULT_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
]);

window.addEventListener(
  "beforeunload",
  () => {
    mqttClient.stop();
  },
  { once: true },
);

render();
mqttClient.start();
