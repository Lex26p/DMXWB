#!/usr/bin/env python3
from __future__ import annotations

import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")

for token in ['./styles.css?v=014a', './app.js?v=014a']:
    if token not in html:
        fail(f"index.html missing DEV-012B5.5 cache version: {token}")

for token in ['./model.js?v=014a', './mqtt-client.js?v=014a']:
    if token not in app:
        fail(f"app.js missing DEV-012B5.5 cache version: {token}")

for token in [
    "refreshSubscription(topic)",
    "this.#sendSubscriptions([normalized])",
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing factual config refresh contract: {token}")

for token in [
    "explicit stale state",
    "stale: Boolean(model.configDraftDirty) && baseRevision !== currentRevision",
]:
    if token not in model:
        fail(f"model.js missing explicit stale draft contract: {token}")

for token in [
    "let uncertainConfigSet = null",
    "requestId: transaction.requestId",
    "proposal: transaction.proposal",
    "baseRevision: transaction.baseRevision",
    "mqttClient.refreshSubscription(MQTT_CONFIG_TOPIC)",
    "const transaction = pendingConfigSet ?? uncertainConfigSet",
    "delete factualConfig.revision",
    "delete proposedConfig.revision",
    "factualRevision === transaction.baseRevision",
    "Черновик можно отправить повторно",
    "Отмените черновик и повторите изменения",
    "info.stale ||",
    "uncertainConfigSet ||",
    "lastFactualSnapshot: null",
    "pendingConfigSet.lastFactualSnapshot = snapshot",
    "confirmedConfig",
]:
    if token not in app:
        fail(f"app.js missing uncertain Config reconciliation token: {token}")

if app.count("publishCommand(MQTT_CONFIG_SET_TOPIC") != 1:
    fail("Config Set must only be published by the explicit Apply path")

print("dev012b5_config_uncertain_outcome_reconciliation: PASS")
