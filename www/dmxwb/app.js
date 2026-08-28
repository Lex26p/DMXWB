import {
  configRevision,
  createInitialModel,
  diagnosticSummary,
  fixtureViewModels,
  groupItems,
  isLegacyArtNetUniverse,
  sceneItems,
  selectedSource,
  setConfigSnapshot,
  setConnection,
  setStateSnapshot,
  setStatusSnapshot,
  structuralSettings,
} from "./model.js?v=011c1";
import {
  MQTT_CONFIG_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
  MQTT_SYSTEM_SOURCE_COMMAND_TOPIC,
  MqttWebSocketClient,
  fixtureCommandTopic,
  mqttTransportDescriptor,
} from "./mqtt-client.js?v=011c1";

let model = createInitialModel();
let fixtureStructureKey = "";
const fixturePublishers = new Map();
const LIVE_PUBLISH_INTERVAL_MS = 40; // 25/s, inside the required 20–30/s window.

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
  sceneCountBadge: document.querySelector("#scene-count-badge"),
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
    send(value);
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

function setInteractiveValue(input, value) {
  if (input.dataset.interacting === "1" || document.activeElement === input) {
    return;
  }
  input.value = String(value);
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

    power.checked = runtime.requestedPower;

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
      setInteractiveValue(input, value);
      output.textContent = String(value);
    }

    const color = card.querySelector('[data-fixture-control="color"]');
    setInteractiveValue(
      color,
      rgbHex(runtime.red, runtime.green, runtime.blue),
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
  const values = [
    ["Application", diagnostics.application],
    ["DMX", diagnostics.dmx],
    ["MQTT", diagnostics.mqtt],
    ["Art-Net", diagnostics.artnet],
  ];

  elements.diagnosticGrid.replaceChildren(
    ...values.map(([label, value]) => {
      const card = document.createElement("article");
      card.className = "metric-card";

      const caption = document.createElement("span");
      caption.textContent = label;

      const strong = document.createElement("strong");
      strong.textContent =
        typeof value === "object" ? "snapshot" : String(value);

      card.append(caption, strong);
      return card;
    }),
  );
}

function renderSettings() {
  const settings = structuralSettings(model);
  const fields = elements.settings;

  if (!settings) {
    fields.dmxPort.value = "";
    fields.fixtureCount.value = "";
    fields.startAddress.value = "";
    fields.artnetUniverse.value = "";
    fields.draftBadge.textContent = "нет конфигурации";
    fields.legacyNote.textContent = "";
    return;
  }

  fields.dmxPort.value = settings.dmxPort;
  fields.fixtureCount.value = String(settings.fixtureCount);
  fields.startAddress.value = String(settings.startAddress);
  fields.artnetUniverse.value = String(settings.artnetUniverse);
  fields.draftBadge.textContent = "локальный draft";
  fields.legacyNote.textContent = isLegacyArtNetUniverse(settings.artnetUniverse)
    ? "Art-Net Universe 0 — legacy compatibility."
    : "";
}

function render() {
  const descriptor = mqttTransportDescriptor(window.location);
  const revision = configRevision(model);
  const source = selectedSource(model);
  const fixtures = fixtureViewModels(model);
  const groups = groupItems(model);
  const scenes = sceneItems(model);

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

function applyMqttSnapshot(topic, payload) {
  const snapshot = parseSnapshot(payload);

  if (topic === MQTT_CONFIG_TOPIC) {
    model = setConfigSnapshot(model, snapshot);
  } else if (topic === MQTT_STATE_TOPIC) {
    model = setStateSnapshot(model, snapshot);
  } else if (topic === MQTT_STATUS_TOPIC) {
    model = setStatusSnapshot(model, snapshot);
  } else {
    return;
  }

  render();
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
        fixturePublisher(fixtureId, control).final(
          `${rgb.red};${rgb.green};${rgb.blue}`,
        );
      }
    } else {
      fixturePublisher(fixtureId, control).final(liveInput.value);
    }
    return;
  }

  const power = event.target.closest("[data-fixture-power]");
  if (power) {
    publishCommand(
      fixtureCommandTopic(fixtureId, "power"),
      power.checked ? "1" : "0",
    );
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
    render();
  },
  onMessage(topic, payload) {
    try {
      applyMqttSnapshot(topic, payload);
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
