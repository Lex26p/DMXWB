#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

for prerequisite in [
    REPO_ROOT / "tools" / "web" / "check_dev011a_static.py",
    REPO_ROOT / "tools" / "web" / "check_dev011b_mqtt.py",
]:
    result = subprocess.run(
        [sys.executable, str(prerequisite)],
        cwd=REPO_ROOT,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for token in [
    'data-source-command="mqtt"',
    'data-source-command="artnet"',
    'id="fixture-list"',
]:
    if token not in html:
        fail(f"index.html missing {token}")

for token in [
    'MQTT_SYSTEM_SOURCE_COMMAND_TOPIC',
    '/devices/dmxwb/controls/source/on',
    'fixtureCommandTopic',
    '/devices/dmxwb_fixture_${id}/controls/${control}/on',
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing command contract token {token}")

for control in [
    '"name"',
    '"power"',
    '"red"',
    '"green"',
    '"blue"',
    '"color"',
    '"brightness"',
    '"temperature"',
    '"reset"',
]:
    if control not in mqtt:
        fail(f"Fixture MQTT control missing {control}")

for token in [
    "LIVE_PUBLISH_INTERVAL_MS = 40",
    "createThrottledPublisher",
    "fixturePublisher",
    ".schedule(",
    ".final(",
    "retain: false",
    "data-fixture-control",
    "data-fixture-power",
    "data-fixture-name",
    "data-fixture-reset",
    "requestedPower",
]:
    if token not in app:
        fail(f"app.js missing control/throttle token {token}")

for token in [
    "requested_power",
    "brightness",
    "temperature",
    "white",
]:
    if token not in model:
        fail(f"model.js missing persisted state field {token}")

if "localStorage" in app or "sessionStorage" in app:
    fail("web command path must not persist/replay commands")

if "fetch(" in app or "XMLHttpRequest" in app:
    fail("web controls must remain MQTT-only")

for token in [
    ".fixture-card",
    ".fixture-controls",
    ".fixture-range",
    ".segmented button.is-selected",
]:
    if token not in styles:
        fail(f"styles.css missing {token}")

print("dev011c1_source_command_contract: PASS")
print("dev011c1_fixture_command_contract: PASS")
print("dev011c1_fixture_state_mapping: PASS")
print("dev011c1_slider_throttle_25hz_and_final_publish: PASS")
print("dev011c1_no_command_replay_storage: PASS")
print("dev011c1_mqtt_only_controls: PASS")
print("=== DMXWB DEV-011C1 SOURCE FIXTURE WEB CONTROLS STATIC PASS ===")
