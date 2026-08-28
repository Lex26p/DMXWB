#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

base_checker = REPO_ROOT / "tools" / "web" / "check_dev011a_static.py"
result = subprocess.run(
    [sys.executable, str(base_checker)],
    cwd=REPO_ROOT,
    text=True,
)
if result.returncode != 0:
    raise SystemExit(result.returncode)

mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")

for token in [
    'MQTT_WEBSOCKET_PATH = "/mqtt"',
    'MQTT_PROTOCOL_NAME = "mqtt"',
    'new WebSocket',
    'binaryType = "arraybuffer"',
    'encodeConnectPacket',
    'encodeSubscribePacket',
    'encodePublishPacket',
    'decodeMqttPackets',
    'MqttWebSocketClient',
    '#scheduleReconnect',
    'encodePacket(0xc0)',
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing {token}")

for topic in [
    "/dmxwb/config",
    "/dmxwb/state",
    "/dmxwb/status",
]:
    if topic not in mqtt:
        fail(f"mqtt-client.js missing retained topic {topic}")

for token in [
    "setConfigSnapshot",
    "setStateSnapshot",
    "setStatusSnapshot",
    "onConnectionChange",
    "mqttClient.subscribe",
    "mqttClient.start",
]:
    if token not in app:
        fail(f"app.js missing {token}")

for forbidden in [
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "systemctl",
]:
    if forbidden in mqtt or forbidden in app:
        fail(f"forbidden direct/runtime API found: {forbidden}")

if 'state: "offline"' not in model:
    fail("model initial connection is not offline")

print("dev011b_mqtt_wire_codec_contract: PASS")
print("dev011b_retained_snapshot_subscriptions: PASS")
print("dev011b_automatic_reconnect_contract: PASS")
print("dev011b_no_command_replay_storage: PASS")
print("=== DMXWB DEV-011B MQTT WEBSOCKET FOUNDATION STATIC PASS ===")
