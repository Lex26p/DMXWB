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

for token in [
    './styles.css?v=014a',
    './app.js?v=014a',
]:
    if token not in html:
        fail(f"index.html missing DEV-012B5.3 cache version: {token}")

for token in [
    './model.js?v=014a',
    './mqtt-client.js?v=014a',
]:
    if token not in app:
        fail(f"app.js missing DEV-012B5.3 cache version: {token}")

for token in [
    'MQTT_SYSTEM_STATUS_TOPIC = "/devices/dmxwb/controls/status"',
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing factual daemon status topic: {token}")

for token in [
    "daemon: { ...EMPTY_DAEMON }",
    "daemon: connected ? model.daemon : { ...EMPTY_DAEMON }",
    "export function setDaemonStatus(model, payload)",
    'available: status === "running" || status === "error"',
]:
    if token not in model:
        fail(f"model.js missing daemon availability contract: {token}")

for token in [
    "model.connection.connected === true &&",
    "model.daemon.available === true",
    "if (topic === MQTT_SYSTEM_STATUS_TOPIC)",
    "model = setDaemonStatus(model, payload)",
    "if (!model.daemon.available)",
    "clearPendingConfirmations()",
    "clearPendingConfigSet(",
    "clearScenePending(",
    'elements.connectionTitle.textContent = "Служба DMXWB недоступна"',
]:
    if token not in app:
        fail(f"app.js missing factual daemon command gate: {token}")

subscription = app.rsplit("mqttClient.subscribe([", maxsplit=1)[-1]
if "MQTT_SYSTEM_STATUS_TOPIC" not in subscription:
    fail("app.js does not subscribe to factual daemon status")

print("dev012b5_factual_daemon_availability_gate: PASS")
