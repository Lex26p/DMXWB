import {
  configRevision,
  createInitialModel,
  diagnosticSummary,
  fixtureViewModels,
  groupItems,
  isLegacyArtNetUniverse,
  sceneItems,
  selectedSource,
  structuralSettings,
} from "./model.js";
import { mqttTransportDescriptor } from "./mqtt-client.js";

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

  renderDiagnostics();
  renderSettings();
}

for (const button of elements.navButtons) {
  button.addEventListener("click", () => {
    activateSection(button.dataset.sectionTarget);
  });
}

render();
