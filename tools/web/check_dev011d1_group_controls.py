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
    REPO_ROOT / "tools" / "web" / "check_dev011c1_fixture_controls.py",
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
    'id="group-list"',
    'class="group-list"',
]:
    if token not in html:
        fail(f"index.html missing {token}")

for token in [
    "groupCommandTopic",
    "groupStateTopics",
    "GROUP_STATE_CONTROLS",
    "parseGroupStateTopic",
    "/devices/${prefix}_${id}/controls/${control}/on",
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing Group MQTT token {token}")

for token in [
    "groupStates: {}",
    "setGroupControlState",
    "groupViewModels",
    "actualPower",
    "memberNames",
]:
    if token not in model:
        fail(f"model.js missing factual Group state token {token}")

# Group factual Power/state must come from retained /devices group controls.
# Do not synthesize group power by OR-ing Fixture runtime state in the browser.
group_view_start = model.find("export function groupViewModels")
if group_view_start < 0:
    fail("groupViewModels not found")
group_view = model[group_view_start:]
if "model.groupStates" not in group_view:
    fail("groupViewModels does not use factual Group MQTT states")
if "fixtureRuntimeItems(model)" in group_view.split("export function sceneItems", 1)[0]:
    fail("Group runtime state is incorrectly synthesized from Fixture runtime")

if "dmxwb_group_+" in mqtt or "MQTT_GROUP_STATE_TOPIC_FILTER" in mqtt:
    fail("invalid partial-level MQTT wildcard for Group topics")

for token in [
    "const added = [];",
    "this.subscriptions.has(normalized)",
    "this.#sendSubscriptions(added)",
    "#sendSubscriptions(topics = [...this.subscriptions])",
]:
    if token not in mqtt:
        fail(f"MQTT client missing incremental subscribe safeguard {token}")

for token in [
    "groupPublishers",
    "groupPublisher",
    "createGroupCard",
    "renderGroupControls",
    "data-group-control",
    "data-group-power",
    "data-group-name",
    "data-group-reset",
    "groupRuntimeComplete",
    "groupStateTopics",
    "setGroupControlState",
    "parseGroupStateTopic",
    "LIVE_PUBLISH_INTERVAL_MS = 40",
    ".schedule(",
    ".final(",
]:
    if token not in app:
        fail(f"app.js missing Group UI/control token {token}")

for forbidden in [
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "systemctl",
]:
    if forbidden in app or forbidden in mqtt:
        fail(f"Group web path contains forbidden API/reference {forbidden}")

if ".group-list" not in styles or ".group-members" not in styles:
    fail("styles.css missing Group layout")

render_start = app.find("function render() {")
render_end = app.find("\nfunction parseSnapshot(", render_start)
if render_start < 0 or render_end < 0:
    fail("cannot locate render() for listener regression check")
if "addEventListener" in app[render_start:render_end]:
    fail("event listeners must never be registered inside render()")

for token in [
    "scheduleRender",
    "requestAnimationFrame",
    "LIVE_CONFIRMATION_TIMEOUT_MS",
    "pendingConfirmations",
    "setPendingConfirmation",
    "pendingAllowsFactualValue",
    "clearPendingConfirmations",
]:
    if token not in app:
        fail(f"app.js missing live interaction stability token {token}")

if app.count('elements.groupList.addEventListener("input"') != 1:
    fail("Group input listener must be registered exactly once")
if app.count('elements.groupList.addEventListener("change"') != 1:
    fail("Group change listener must be registered exactly once")
if app.count('elements.fixtureList.addEventListener("input"') != 1:
    fail("Fixture input listener must be registered exactly once")
if app.count('elements.fixtureList.addEventListener("change"') != 1:
    fail("Fixture change listener must be registered exactly once")

if ".final(liveInput.value)" not in app:
    fail("final slider publication contract disappeared")

print("dev011d1_group_command_contract: PASS")
print("dev011d1_group_factual_state_subscription: PASS")
print("dev011d1_incremental_subscription_no_retained_loop: PASS")
print("dev011d1_group_power_not_synthesized_in_browser: PASS")
print("dev011d1_group_member_mapping: PASS")
print("dev011d1_group_slider_throttle_25hz_and_final_publish: PASS")
print("dev011d1_event_listeners_registered_once: PASS")
print("dev011d1_render_burst_coalescing: PASS")
print("dev011d1_pending_factual_confirmation_guard: PASS")
print("dev011d1_group_no_command_replay_storage: PASS")
print("dev011d1_group_mqtt_only_controls: PASS")
print("=== DMXWB DEV-011D1 GROUP WEB CONTROLS STATIC PASS ===")
