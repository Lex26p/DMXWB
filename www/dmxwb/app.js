import {
  addFixtureDraft,
  addGroupDraft,
  configDraftInfo,
  createInitialModel,
  fixtureViewModels,
  groupViewModels,
  isLegacyArtNetUniverse,
  sceneSnapshotMatchesState,
  sceneViewModels,
  selectedSource,
  removeFixtureDraft,
  removeGroupDraft,
  resetConfigDraft,
  setConfigSnapshot,
  setConnection,
  setDaemonStatus,
  setGroupControlState,
  setStateSnapshot,
  setStatusSnapshot,
  setGroupDraftMember,
  statusSummary,
  structuralGroupDrafts,
  structuralSettings,
  updateConfigDraft,
} from "./model.js?v=014a";
import {
  MQTT_CONFIG_RESULT_TOPIC,
  MQTT_CONFIG_SET_TOPIC,
  MQTT_CONFIG_TOPIC,
  MQTT_SCENE_CREATE_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
  MQTT_SYSTEM_STATUS_TOPIC,
  MQTT_SYSTEM_SOURCE_COMMAND_TOPIC,
  MqttWebSocketClient,
  fixtureCommandTopic,
  groupCommandTopic,
  groupStateTopics,
  buildMqttWebSocketUrl,
  parseGroupStateTopic,
  sceneCommandTopic,
  sceneLifecycleTopic,
} from "./mqtt-client.js?v=014a";

let model = createInitialModel();
let fixtureStructureKey = "";
let groupStructureKey = "";
let sceneStructureKey = "";
const fixturePublishers = new Map();
const groupPublishers = new Map();
const LIVE_PUBLISH_INTERVAL_MS = 40; // 25/s, inside the required 20–30/s window.
const LIVE_CONFIRMATION_TIMEOUT_MS = 2000;
const COMMAND_RESULT_TIMEOUT_MS = 5000;
const pendingConfirmations = new Map();
let renderFramePending = false;
let sceneRequestCounter = 0;
let configRequestCounter = 0;
let pendingConfigSet = null;
let uncertainConfigSet = null;
let settingsResult = { kind: "idle", text: "" };
const invalidNumericSettings = new Set();
const pendingSceneLifecycle = new Map();
let uncertainSceneCreate = null;
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
  sourceButtons: [...document.querySelectorAll("[data-source-command]")],
  fixtureCountBadge: document.querySelector("#fixture-count-badge"),
  fixtureList: document.querySelector("#fixture-list"),
  groupList: document.querySelector("#group-list"),
  sceneCountBadge: document.querySelector("#scene-count-badge"),
  sceneList: document.querySelector("#scene-list"),
  sceneCreateName: document.querySelector("#scene-create-name"),
  sceneCreateButton: document.querySelector("#scene-create-button"),
  sceneResult: document.querySelector("#scene-result"),
  pageTitle: document.querySelector("#page-title"),
  statusFields: Object.fromEntries(
    [...document.querySelectorAll("[data-status-field]")].map((element) => [
      element.dataset.statusField,
      element,
    ]),
  ),
  connectionIndicator: document.querySelector("#connection-indicator"),
  connectionTitle: document.querySelector("#connection-title"),
  settings: {
    dmxPort: document.querySelector("#setting-dmx-port"),
    startAddress: document.querySelector("#setting-start-address"),
    artnetUniverse: document.querySelector("#setting-artnet-universe"),
    draftBadge: document.querySelector("#draft-badge"),
    legacyNote: document.querySelector("#legacy-universe-note"),
    result: document.querySelector("#settings-result"),
    fixtureList: document.querySelector("#settings-fixture-list"),
    addFixtureButton: document.querySelector("#settings-add-fixture-button"),
    groupList: document.querySelector("#settings-group-list"),
    addGroupButton: document.querySelector("#settings-add-group-button"),
    resetButton: document.querySelector("#settings-reset-button"),
    applyButton: document.querySelector("#settings-apply-button"),
  },
};

const sectionTitles = {
  control: "Управление",
  fixtures: "Светильники и группы",
  scenes: "Сцены",
  settings: "Настройки",
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

  elements.pageTitle.textContent =
    sectionTitles[sectionName] ?? sectionTitles.control;
}

function renderConnection() {
  const connection = model.connection;
  const labels = {
    connected: "Связь установлена",
    connecting: "Подключение…",
    reconnecting: "Переподключение…",
    offline: "Нет связи",
  };

  if (connection.connected && !model.daemon.known) {
    elements.connectionTitle.textContent = "Ожидание статуса DMXWB…";
  } else if (connection.connected && !model.daemon.available) {
    elements.connectionTitle.textContent = "Служба DMXWB недоступна";
  } else {
    elements.connectionTitle.textContent =
      labels[connection.state] ?? labels.offline;
  }

  elements.connectionIndicator.classList.remove(
    "status-dot--online",
    "status-dot--connecting",
    "status-dot--offline",
  );

  if (connection.connected && model.daemon.available) {
    elements.connectionIndicator.classList.add("status-dot--online");
  } else if (connection.connected && !model.daemon.known) {
    elements.connectionIndicator.classList.add("status-dot--connecting");
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
  return (
    model.connection.connected === true &&
    model.daemon.available === true
  );
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
  powerText.textContent = "Питание";
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
  powerText.textContent = "Питание";
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
  if (!pendingConfigSet) {
    return;
  }

  const transaction = pendingConfigSet;
  if (transaction.timer) {
    window.clearTimeout(transaction.timer);
  }
  pendingConfigSet = null;
  uncertainConfigSet = {
    requestId: transaction.requestId,
    proposal: transaction.proposal,
    baseRevision: transaction.baseRevision,
  };
  if (reason) {
    settingsResult = { kind: "error", text: reason };
  }

  if (transaction.lastFactualSnapshot) {
    maybeConfirmConfigSet(transaction.lastFactualSnapshot);
  } else if (model.connection.connected) {
    mqttClient.refreshSubscription(MQTT_CONFIG_TOPIC);
  }
}

function configErrorText(errorCode) {
  const messages = {
    revision_conflict:
      "Конфигурация изменилась в другой вкладке. Отмените изменения или повторите их после обновления.",
    json_syntax: "Некорректный формат конфигурации.",
    schema: "Структура конфигурации некорректна.",
    version: "Версия конфигурации не поддерживается.",
    validation: "Параметры конфигурации не прошли проверку.",
  };
  return messages[errorCode] ?? "Не удалось применить конфигурацию.";
}

function handleConfigSetResult(result) {
  const pendingMatches = pendingConfigSet?.requestId === result.request_id;
  const uncertainMatches = uncertainConfigSet?.requestId === result.request_id;
  if (!pendingMatches && !uncertainMatches) {
    return false;
  }

  const transaction = pendingMatches ? pendingConfigSet : uncertainConfigSet;
  if (pendingConfigSet?.timer) {
    window.clearTimeout(pendingConfigSet.timer);
  }
  pendingConfigSet = null;
  uncertainConfigSet = null;
  if (!result.ok) {
    if (result.error_code === "revision_conflict") {
      uncertainConfigSet = transaction;
      mqttClient.refreshSubscription(MQTT_CONFIG_TOPIC);
    }
    settingsResult = {
      kind: "error",
      text: configErrorText(result.error_code),
    };
    scheduleRender();
    return true;
  }

  const confirmedConfig = {
    ...transaction.proposal,
    revision: result.revision,
  };
  model = setConfigSnapshot(model, confirmedConfig);
  model = resetConfigDraft(model);
  clearNumericSettingsValidation();
  settingsResult = {
    kind: "success",
    text: "Конфигурация сохранена.",
  };
  scheduleRender();
  return true;
}

function maybeConfirmConfigSet(snapshot) {
  const transaction = pendingConfigSet ?? uncertainConfigSet;
  if (!transaction?.proposal) {
    return;
  }

  const factualConfig = { ...snapshot };
  const proposedConfig = { ...transaction.proposal };
  delete factualConfig.revision;
  delete proposedConfig.revision;
  if (JSON.stringify(factualConfig) !== JSON.stringify(proposedConfig)) {
    if (pendingConfigSet) {
      pendingConfigSet.lastFactualSnapshot = snapshot;
      return;
    }

    uncertainConfigSet = null;
    const factualRevision = Number(snapshot.revision);
    if (factualRevision === transaction.baseRevision) {
      settingsResult = {
        kind: "error",
        text: "Изменение не применилось. Черновик можно отправить повторно.",
      };
    } else {
      settingsResult = {
        kind: "error",
        text: "Конфигурация изменилась. Отмените черновик и повторите изменения.",
      };
    }
    return;
  }

  if (pendingConfigSet?.timer) {
    window.clearTimeout(pendingConfigSet.timer);
  }
  pendingConfigSet = null;
  uncertainConfigSet = null;
  model = resetConfigDraft(model);
  clearNumericSettingsValidation();
  settingsResult = {
    kind: "success",
    text: "Конфигурация сохранена.",
  };
}

function parseUnsignedSetting(input, minimum, maximum) {
  if (!/^[0-9]+$/.test(input.value.trim())) {
    return null;
  }
  const value = Number(input.value.trim());
  if (
    !Number.isSafeInteger(value) ||
    value < minimum ||
    value > maximum
  ) {
    return null;
  }
  return value;
}

function clearNumericSettingsValidation() {
  invalidNumericSettings.clear();
  for (const input of [
    elements.settings.startAddress,
    elements.settings.artnetUniverse,
  ]) {
    input.setAttribute("aria-invalid", "false");
  }
}

function validateNumericSettingsForm() {
  const specifications = [
    {
      key: "startAddress",
      label: "Начальный адрес",
      minimum: 1,
      maximum: 300,
    },
    {
      key: "artnetUniverse",
      label: "Вселенная Art-Net",
      minimum: 0,
      maximum: 32767,
    },
  ];
  const values = {
    fixtureCount: structuralSettings(model)?.fixtureCount ?? 0,
  };
  const errors = [];

  invalidNumericSettings.clear();
  for (const specification of specifications) {
    const input = elements.settings[specification.key];
    const value = parseUnsignedSetting(
      input,
      specification.minimum,
      specification.maximum,
    );
    if (value === null) {
      invalidNumericSettings.add(specification.key);
      input.setAttribute("aria-invalid", "true");
      errors.push(
        `${specification.label}: введите целое число от ` +
          `${specification.minimum} до ${specification.maximum}.`,
      );
      continue;
    }

    input.setAttribute("aria-invalid", "false");
    values[specification.key] = value;
  }

  if (
    Number.isSafeInteger(values.fixtureCount) &&
    Number.isSafeInteger(values.startAddress) &&
    values.fixtureCount > 0
  ) {
    const lastAddress =
      values.startAddress + values.fixtureCount * 4 - 1;
    if (lastAddress > 300) {
      elements.settings.startAddress.setAttribute("aria-invalid", "true");
      errors.push(
        `Диапазон светильников заканчивается на адресе ${lastAddress}; ` +
          "максимальный физический адрес — 300.",
      );
    }
  }

  return {
    valid: errors.length === 0,
    values,
    error: errors[0] ?? "",
  };
}

function publishConfigDraft() {
  const validation = validateNumericSettingsForm();
  if (!validation.valid) {
    settingsResult = { kind: "error", text: validation.error };
    scheduleRender();
    return false;
  }

  const info = configDraftInfo(model);
  if (
    !info?.dirty ||
    info.stale ||
    pendingConfigSet ||
    uncertainConfigSet ||
    !canPublishCommands()
  ) {
    return false;
  }

  if (
    info.proposal.fixtures?.count !== validation.values.fixtureCount ||
    info.proposal.fixtures?.start_address !== validation.values.startAddress ||
    info.proposal.artnet?.universe !== validation.values.artnetUniverse
  ) {
    settingsResult = {
      kind: "error",
      text: "Значения формы изменились. Проверьте поля перед применением.",
    };
    scheduleRender();
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

  const timer = window.setTimeout(() => {
    if (pendingConfigSet?.requestId !== requestId || pendingConfigSet.timer !== timer) {
      return;
    }
    clearPendingConfigSet(
      "Результат не получен. Проверяем фактическую конфигурацию DMXWB.",
    );
    scheduleRender();
  }, COMMAND_RESULT_TIMEOUT_MS);
  pendingConfigSet = {
    requestId,
    timer,
    proposal: info.proposal,
    baseRevision: info.baseRevision,
    lastFactualSnapshot: null,
  };
  settingsResult = {
    kind: "pending",
    text: "Применение…",
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

function clearScenePending(reason = "") {
  const hadPending =
    pendingSceneRenames.size > 0 ||
    pendingSceneLifecycle.size > 0 ||
    pendingSceneApply !== null ||
    uncertainSceneCreate !== null;

  for (const pending of pendingSceneRenames.values()) {
    window.clearTimeout(pending.timer);
  }
  for (const pending of pendingSceneLifecycle.values()) {
    window.clearTimeout(pending.timer);
    if (pending.operation === "create") {
      uncertainSceneCreate = {
        requestId: pending.requestId,
        payload: pending.payload,
      };
    }
  }
  if (pendingSceneApply?.timer) {
    window.clearTimeout(pendingSceneApply.timer);
  }
  pendingSceneRenames.clear();
  pendingSceneLifecycle.clear();
  pendingSceneApply = null;

  if (reason && hadPending) {
    sceneResult = { kind: "error", text: reason };
  }
}

function setPendingSceneRename(requestId, sceneId, expected) {
  const timer = window.setTimeout(() => {
    const current = pendingSceneRenames.get(requestId);
    if (current?.timer !== timer) {
      return;
    }
    pendingSceneRenames.delete(requestId);
    sceneResult = {
      kind: "error",
      text: "Нет ответа от DMXWB на переименование сцены.",
    };
    scheduleRender();
  }, COMMAND_RESULT_TIMEOUT_MS);

  pendingSceneRenames.set(requestId, {
    requestId,
    sceneId,
    expected,
    timer,
  });
}

function confirmSceneRenames(scenes) {
  const byId = new Map(scenes.map((scene) => [scene.id, scene]));
  for (const [requestId, pending] of [...pendingSceneRenames.entries()]) {
    const factual = byId.get(pending.sceneId);
    if (!factual || factual.name !== pending.expected) {
      continue;
    }
    window.clearTimeout(pending.timer);
    pendingSceneRenames.delete(requestId);
    sceneResult = {
      kind: "success",
      text: "Имя сцены сохранено.",
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
    throw new TypeError("Некорректный ответ результата конфигурации.");
  }
  return result;
}

function handleSceneConfigResult(result) {
  const sceneErrors = {
    not_found: "Сцена не найдена.",
    validation: "Параметры сцены некорректны.",
    idempotency_conflict:
      "Этот запрос создания уже использован с другим именем сцены.",
    operation_failed: "Не удалось выполнить операцию со сценой.",
  };

  if (pendingSceneApply?.requestId === result.request_id) {
    window.clearTimeout(pendingSceneApply.timer);
    pendingSceneApply = null;
    sceneResult = result.ok
      ? { kind: "success", text: "Сцена применена." }
      : {
          kind: "error",
          text: sceneErrors[result.error_code] ?? "Применение сцены отклонено.",
        };
    scheduleRender();
    return true;
  }

  const pendingRename = pendingSceneRenames.get(result.request_id);
  if (pendingRename) {
    window.clearTimeout(pendingRename.timer);
    pendingSceneRenames.delete(result.request_id);
    sceneResult = result.ok
      ? { kind: "success", text: "Имя сцены сохранено." }
      : {
          kind: "error",
          text: sceneErrors[result.error_code] ?? "Переименование сцены отклонено.",
        };
    scheduleRender();
    return true;
  }

  const pending = pendingSceneLifecycle.get(result.request_id) ??
    (uncertainSceneCreate?.requestId === result.request_id
      ? { operation: "create", ...uncertainSceneCreate }
      : null);
  if (!pending) {
    return false;
  }

  if (pending.timer) {
    window.clearTimeout(pending.timer);
  }
  pendingSceneLifecycle.delete(result.request_id);
  if (uncertainSceneCreate?.requestId === result.request_id) {
    uncertainSceneCreate = null;
  }
  if (!result.ok) {
    sceneResult = {
      kind: "error",
      text: sceneErrors[result.error_code] ?? "Операция сцены отклонена.",
    };
    scheduleRender();
    return true;
  }

  if (pending.operation === "create") {
    elements.sceneCreateName.value = "";
  }

  const successText = {
    create: "Сцена создана.",
    overwrite: "Сцена перезаписана.",
    delete: "Сцена удалена.",
  };
  sceneResult = {
    kind: "success",
    text: successText[pending.operation] ?? "Готово.",
  };
  scheduleRender();
  return true;
}

function publishSceneLifecycle(topic, payload, operation) {
  if (!canPublishCommands()) {
    return false;
  }

  const requestId = payload.request_id;
  if (!publishCommand(topic, JSON.stringify(payload))) {
    return false;
  }

  const timer = window.setTimeout(() => {
    const current = pendingSceneLifecycle.get(requestId);
    if (current?.timer !== timer) {
      return;
    }
    pendingSceneLifecycle.delete(requestId);
    if (operation === "create") {
      uncertainSceneCreate = {
        requestId,
        payload: { ...payload },
      };
      sceneResult = {
        kind: "error",
        text: "Ответ потерян. Повторное создание использует тот же запрос.",
      };
    } else {
      sceneResult = {
        kind: "error",
        text: "Нет ответа от DMXWB. Проверьте фактическое состояние.",
      };
    }
    scheduleRender();
  }, COMMAND_RESULT_TIMEOUT_MS);
  pendingSceneLifecycle.set(requestId, {
    requestId,
    operation,
    payload: { ...payload },
    timer,
  });
  if (operation === "create" &&
      uncertainSceneCreate?.requestId === requestId) {
    uncertainSceneCreate = null;
  }
  sceneResult = {
    kind: "pending",
    text: "Выполнение…",
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

  identity.append(name);

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
    const pendingRename = [...pendingSceneRenames.values()].find(
      (pending) => pending.sceneId === scene.id,
    );

    if (document.activeElement !== name) {
      name.value = pendingRename?.expected ?? scene.name;
    }
    name.disabled = !connected || lifecycleBusy;

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
  elements.sceneCreateName.disabled =
    !connected || pendingSceneLifecycle.size > 0 || uncertainSceneCreate !== null;
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

  window.clearTimeout(pendingSceneApply.timer);
  pendingSceneApply = null;
  sceneResult = {
    kind: "success",
    text: "Сцена применена.",
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
}

function renderStatus() {
  const summary = statusSummary(model);
  for (const [field, element] of Object.entries(elements.statusFields)) {
    element.textContent = summary[field] ?? "—";
  }
}

function createSettingsFixtureCard(fixture, index, startAddress) {
  const card = document.createElement("article");
  card.className = "settings-fixture-card";
  card.dataset.settingsFixtureId = String(fixture.id);

  const identity = document.createElement("div");
  const name = document.createElement("strong");
  name.textContent = fixture.name;
  const address = document.createElement("span");
  address.className = "field-note";
  const firstAddress = startAddress + index * 4;
  address.textContent = `ID ${fixture.id} · DMX ${firstAddress}–${firstAddress + 3}`;
  identity.append(name, address);

  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "secondary-button settings-fixture-remove";
  remove.dataset.settingsFixtureRemove = "";
  remove.textContent = "Удалить";

  card.append(identity, remove);
  return card;
}

function renderSettingsFixtureEditor() {
  const editor = structuralGroupDrafts(model);
  const settings = structuralSettings(model);
  const disabled = pendingConfigSet !== null || uncertainConfigSet !== null;
  const fixtures = editor?.fixtures ?? [];
  const startAddress = settings?.startAddress ?? 1;
  const nextLastAddress = startAddress + (fixtures.length + 1) * 4 - 1;

  elements.settings.addFixtureButton.disabled =
    !editor ||
    disabled ||
    invalidNumericSettings.has("startAddress") ||
    fixtures.length >= 75 ||
    nextLastAddress > 300;
  elements.settings.fixtureList.replaceChildren();

  if (!editor || fixtures.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    const title = document.createElement("strong");
    title.textContent = "Нет светильников";
    const text = document.createElement("span");
    text.textContent = "Добавьте первый светильник.";
    empty.append(title, text);
    elements.settings.fixtureList.append(empty);
    return;
  }

  fixtures.forEach((fixture, index) => {
    const card = createSettingsFixtureCard(fixture, index, startAddress);
    card.querySelector("[data-settings-fixture-remove]").disabled = disabled;
    elements.settings.fixtureList.append(card);
  });
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
  identity.append(name);

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
    empty.textContent = "Нет светильников.";
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
  const disabled = pendingConfigSet !== null || uncertainConfigSet !== null;

  elements.settings.addGroupButton.disabled =
    !editor || disabled || editor.fixtures.length === 0;
  elements.settings.groupList.replaceChildren();

  if (!editor || editor.groups.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    const title = document.createElement("strong");
    title.textContent = "Нет групп";
    const text = document.createElement("span");
    text.textContent = "Добавьте группу.";
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
    clearNumericSettingsValidation();
    renderSettingsFixtureEditor();
    renderSettingsGroupEditor();
    fields.dmxPort.value = "/dev/ttyRS485-1";
    fields.startAddress.value = "";
    fields.artnetUniverse.value = "";
    fields.draftBadge.textContent = "Нет конфигурации";
    fields.legacyNote.textContent = "";
    fields.result.textContent = "";
    fields.applyButton.disabled = true;
    fields.resetButton.disabled = true;
    for (const input of [
      fields.dmxPort,
      fields.startAddress,
      fields.artnetUniverse,
    ]) {
      input.disabled = true;
    }
    return;
  }

  renderSettingsFixtureEditor();
  renderSettingsGroupEditor();

  fields.dmxPort.value = settings.dmxPort;
  if (!invalidNumericSettings.has("startAddress")) {
    fields.startAddress.value = String(settings.startAddress);
  }
  if (!invalidNumericSettings.has("artnetUniverse")) {
    fields.artnetUniverse.value = String(settings.artnetUniverse);
  }

  const numericValidation = validateNumericSettingsForm();

  for (const input of [
    fields.dmxPort,
    fields.startAddress,
    fields.artnetUniverse,
  ]) {
    input.disabled = pendingConfigSet !== null || uncertainConfigSet !== null;
  }

  if (!numericValidation.valid) {
    fields.draftBadge.textContent = "Некорректное значение";
  } else if (!info.dirty) {
    fields.draftBadge.textContent = "Сохранено";
  } else if (info.stale) {
    fields.draftBadge.textContent = "Конфигурация изменилась";
  } else {
    fields.draftBadge.textContent = "Есть изменения";
  }

  fields.legacyNote.textContent = isLegacyArtNetUniverse(settings.artnetUniverse)
    ? "Вселенная Art-Net 0 — режим совместимости с нумерацией с нуля."
    : "";

  fields.result.classList.toggle(
    "settings-result--error",
    !numericValidation.valid || settingsResult.kind === "error",
  );
  fields.result.classList.toggle(
    "settings-result--success",
    numericValidation.valid && settingsResult.kind === "success",
  );
  fields.result.textContent = numericValidation.valid
    ? settingsResult.text
    : numericValidation.error;

  fields.applyButton.disabled =
    !numericValidation.valid ||
    !info.dirty ||
    info.stale ||
    pendingConfigSet !== null ||
    uncertainConfigSet !== null ||
    !canPublishCommands();
  fields.resetButton.disabled =
    (!info.dirty && numericValidation.valid) ||
    pendingConfigSet !== null ||
    uncertainConfigSet !== null;
}

function render() {
  const source = selectedSource(model);
  const fixtures = fixtureViewModels(model);
  const groups = groupViewModels(model);
  const scenes = sceneViewModels(model);

  elements.fixtureCountBadge.textContent =
    `${fixtures.length} светильников · ${groups.length} групп`;
  elements.sceneCountBadge.textContent = `${scenes.length} сцен`;

  renderConnection();
  renderSourceControls(source);
  renderFixtureControls(fixtures);
  renderGroupControls(groups);
  renderSceneControls(scenes);
  renderStatus();
  renderSettings();
}

function parseSnapshot(payload) {
  const value = JSON.parse(payload);
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError("Некорректный снимок состояния MQTT.");
  }
  return value;
}

function applyMqttMessage(topic, payload) {
  if (topic === MQTT_SYSTEM_STATUS_TOPIC) {
    model = setDaemonStatus(model, payload);
    if (!model.daemon.available) {
      clearPendingConfirmations();
      clearPendingConfigSet(
        "Изменение конфигурации не подтверждено: служба DMXWB недоступна.",
      );
      clearScenePending(
        "Команда сцены не подтверждена: служба DMXWB недоступна.",
      );
    }
    scheduleRender();
    return;
  }

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
      maybeConfirmConfigSet(snapshot);
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

elements.settings.addFixtureButton.addEventListener("click", () => {
  if (pendingConfigSet || uncertainConfigSet) {
    return;
  }

  try {
    model = addFixtureDraft(model);
    settingsResult = { kind: "idle", text: "" };
    scheduleRender();
  } catch (error) {
    setSettingsResult("error", String(error?.message ?? error));
  }
});

elements.settings.fixtureList.addEventListener("click", (event) => {
  const remove = event.target.closest("[data-settings-fixture-remove]");
  if (!remove || remove.disabled || pendingConfigSet || uncertainConfigSet) {
    return;
  }

  const card = remove.closest("[data-settings-fixture-id]");
  if (!card) {
    return;
  }

  const fixtureId = Number(card.dataset.settingsFixtureId);
  const editor = structuralGroupDrafts(model);
  const fixture = editor?.fixtures.find(
    (candidate) => Number(candidate.id) === fixtureId,
  );
  const fixtureName = fixture?.name ?? `Светильник ${fixtureId}`;
  if (!window.confirm(`Удалить «${fixtureName}»?`)) {
    return;
  }

  model = removeFixtureDraft(model, fixtureId);
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.addGroupButton.addEventListener("click", () => {
  const editor = structuralGroupDrafts(model);
  if (
    pendingConfigSet ||
    uncertainConfigSet ||
    !editor ||
    editor.fixtures.length === 0
  ) {
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
  if (!checkbox || pendingConfigSet || uncertainConfigSet) {
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
  if (!remove || remove.disabled || pendingConfigSet || uncertainConfigSet) {
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

elements.settings.startAddress.addEventListener("input", () => {
  const validation = validateNumericSettingsForm();
  if (!Number.isSafeInteger(validation.values.startAddress)) {
    scheduleRender();
    return;
  }
  model = updateConfigDraft(model, (draft) => {
    draft.fixtures ??= {};
    draft.fixtures.start_address = validation.values.startAddress;
  });
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.artnetUniverse.addEventListener("input", () => {
  const validation = validateNumericSettingsForm();
  if (!Number.isSafeInteger(validation.values.artnetUniverse)) {
    scheduleRender();
    return;
  }
  model = updateConfigDraft(model, (draft) => {
    draft.artnet ??= {};
    draft.artnet.universe = validation.values.artnetUniverse;
  });
  settingsResult = { kind: "idle", text: "" };
  scheduleRender();
});

elements.settings.resetButton.addEventListener("click", () => {
  if (pendingConfigSet || uncertainConfigSet) {
    return;
  }
  clearNumericSettingsValidation();
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

  const payload = uncertainSceneCreate?.payload ?? {
    request_id: makeSceneRequestId("create"),
    name: elements.sceneCreateName.value,
  };
  publishSceneLifecycle(
    MQTT_SCENE_CREATE_TOPIC,
    payload,
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

  const requestId = makeSceneRequestId("rename");
  if (
    publishCommand(
      sceneLifecycleTopic(sceneId, "rename"),
      JSON.stringify({ request_id: requestId, name: name.value }),
    )
  ) {
    setPendingSceneRename(requestId, sceneId, name.value);
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
    const requestId = makeSceneRequestId("apply");
    if (
      publishCommand(
        sceneLifecycleTopic(sceneId, "apply"),
        JSON.stringify({ request_id: requestId }),
      )
    ) {
      const timer = window.setTimeout(() => {
        if (
          pendingSceneApply?.requestId !== requestId ||
          pendingSceneApply.timer !== timer
        ) {
          return;
        }
        pendingSceneApply = null;
        sceneResult = {
          kind: "error",
          text: "Нет ответа от DMXWB на применение сцены.",
        };
        scheduleRender();
      }, COMMAND_RESULT_TIMEOUT_MS);
      pendingSceneApply = {
        requestId,
        timer,
        scene: {
          ...scene,
          fixtures: scene.fixtures.map((fixture) => ({ ...fixture })),
        },
      };
      sceneResult = {
        kind: "pending",
        text: "Применение сцены…",
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

const mqttClient = new MqttWebSocketClient({
  url: buildMqttWebSocketUrl(window.location),
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
      console.error(`DMXWB: отклонён некорректный MQTT-снимок из ${topic}`, error);
    }
  },
  onProtocolError(error) {
    console.error("DMXWB: ошибка транспорта MQTT", error);
  },
});

mqttClient.subscribe([
  MQTT_CONFIG_TOPIC,
  MQTT_CONFIG_RESULT_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
  MQTT_SYSTEM_STATUS_TOPIC,
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
