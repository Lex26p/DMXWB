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
} from "./model.js?v=011b2";
import {
  MQTT_CONFIG_TOPIC,
  MQTT_STATE_TOPIC,
  MQTT_STATUS_TOPIC,
  MqttWebSocketClient,
  mqttTransportDescriptor,
} from "./mqtt-client.js?v=011b2";

let model = createInitialModel();

const elements = {
  navButtons: [...document.querySelectorAll("[data-section-target]")],
  sections: [...document.querySelectorAll("[data-section]")],
  mqttEndpoint: document.querySelector("#mqtt-endpoint"),
  configRevision: document.querySelector("#config-revision"),
  stateStatus: document.querySelector("#state-status"),
  sourceBadge: document.querySelector("#source-badge"),
  fixtureCountBadge: document.querySelector("#fixture-count-badge"),
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
