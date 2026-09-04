#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011d1_group_controls.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for token in [
    'id="scene-create-name"',
    'id="scene-create-button"',
    'id="scene-result"',
    'id="scene-list"',
]:
    if token not in html:
        fail(f"index.html missing {token}")

for token in [
    'MQTT_CONFIG_RESULT_TOPIC = "/dmxwb/config/result"',
    'MQTT_SCENE_CREATE_TOPIC = "/dmxwb/scenes/create"',
    "sceneCommandTopic",
    "sceneLifecycleTopic",
    "/devices/dmxwb_scene_${sceneId}/controls/${control}/on",
    "/dmxwb/scenes/${sceneId}/${action}",
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing Scene MQTT contract token {token}")

for token in [
    "sceneViewModels",
    "sceneSnapshotMatchesState",
    "requested_power",
    "fixture_id",
    "brightness",
]:
    if token not in model:
        fail(f"model.js missing Scene factual state token {token}")

for token in [
    "const hadPending =",
    "pendingSceneRenames.size > 0",
    "pendingSceneLifecycle.size > 0",
    "pendingSceneApply !== null",
    "if (reason && hadPending)",
]:
    if token not in app:
        fail(f"Scene disconnect handling missing pending-only error guard {token}")

if 'if (reason) {\n    sceneResult = { kind: "error", text: reason };' in app:
    fail("Scene disconnect must not show a command error without a pending command")

for token in [
    "makeSceneRequestId",
    "pendingSceneLifecycle",
    "pendingSceneRenames",
    "pendingSceneApply",
    "parseConfigResult",
    "handleSceneConfigResult",
    "publishSceneLifecycle",
    "renderSceneControls",
    "maybeConfirmSceneApply",
    'operation === "create"',
    'sceneLifecycleTopic(sceneId, "rename")',
    'sceneLifecycleTopic(sceneId, "apply")',
    'sceneLifecycleTopic(sceneId, "overwrite")',
    'sceneLifecycleTopic(sceneId, "delete")',
    "request_id: requestId",
    "JSON.stringify(payload)",
    "MQTT_CONFIG_RESULT_TOPIC",
    "COMMAND_RESULT_TIMEOUT_MS",
    "window.clearTimeout(pendingSceneApply.timer)",
    "pendingSceneApply?.requestId === result.request_id",
    "pendingSceneRenames.get(result.request_id)",
]:
    if token not in app:
        fail(f"app.js missing Scene lifecycle token {token}")

if app.count('elements.sceneList.addEventListener("click"') != 1:
    fail("Scene click handler must be registered exactly once")
if app.count('elements.sceneList.addEventListener("change"') != 1:
    fail("Scene change handler must be registered exactly once")

for obsolete in [
    'sceneCommandTopic(sceneId, "apply")',
    'sceneCommandTopic(sceneId, "name")',
]:
    if obsolete in app:
        fail(f"Web Scene operation lacks request correlation: {obsolete}")

render_start = app.find("function render() {")
render_end = app.find("\nfunction parseSnapshot(", render_start)
if render_start < 0 or render_end < 0:
    fail("cannot locate render()")
if "addEventListener" in app[render_start:render_end]:
    fail("event listener registration reappeared inside render()")

for forbidden in [
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "systemctl",
]:
    if forbidden in app or forbidden in mqtt:
        fail(f"Scene web path contains forbidden API/reference {forbidden}")

for token in [
    ".scene-create",
    ".scene-list",
    ".scene-card",
    ".scene-actions",
    ".scene-result--error",
]:
    if token not in styles:
        fail(f"styles.css missing {token}")

print("dev011e1_scene_create_contract: PASS")
print("dev011e1_scene_apply_contract: PASS")
print("dev011e1_scene_overwrite_contract: PASS")
print("dev011e1_scene_rename_contract: PASS")
print("dev011e1_scene_delete_contract: PASS")
print("dev011e1_scene_request_id_result_matching: PASS")
print("dev012b4_scene_rename_apply_result_matching: PASS")
print("dev012b4_scene_pending_timeout_contract: PASS")
print("dev011e1_scene_apply_factual_state_confirmation: PASS")
print("dev011e1_scene_no_false_disconnect_error_without_pending: PASS")
print("dev011e1_scene_no_command_replay_storage: PASS")
print("dev011e1_scene_mqtt_only_controls: PASS")
print("=== DMXWB DEV-011E1 SCENE WEB CONTROLS STATIC PASS ===")
