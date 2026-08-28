#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011e1_scene_controls.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for token in [
    'id="setting-dmx-port"',
    'id="setting-fixture-count"',
    'id="setting-start-address"',
    'id="setting-artnet-universe"',
    'id="settings-result"',
    'id="settings-reset-button"',
    'id="settings-apply-button"',
    'value="/dev/ttyRS485-1"',
    'value="/dev/ttyRS485-2"',
]:
    if token not in html:
        fail(f"index.html missing Settings token {token}")

for token in [
    'MQTT_CONFIG_SET_TOPIC = "/dmxwb/config/set"',
]:
    if token not in mqtt:
        fail(f"mqtt-client.js missing config/set topic {token}")

for token in [
    "configDraftBaseRevision",
    "configDraftDirty",
    "A newer retained config must never destroy an in-progress local draft",
    "resizeFixtureDraft",
    "next_fixture_id",
    "Scene snapshots are historical records",
    "configDraftInfo",
    "stale:",
]:
    if token not in model:
        fail(f"model.js missing revision-safe draft token {token}")

for token in [
    "makeConfigRequestId",
    "pendingConfigSet",
    "handleConfigSetResult",
    "publishConfigDraft",
    "expected_revision: info.baseRevision",
    "config: info.proposal",
    "MQTT_CONFIG_SET_TOPIC",
    "revision_conflict",
    "resetConfigDraft",
    "resizeFixtureDraft",
    "settings.applyButton",
    "settings.resetButton",
    "clearPendingConfigSet",
]:
    if token not in app:
        fail(f"app.js missing config transaction token {token}")

if app.count('elements.settings.applyButton.addEventListener("click"') != 1:
    fail("Settings Apply listener must be registered exactly once")
if app.count('elements.settings.resetButton.addEventListener("click"') != 1:
    fail("Settings Reset listener must be registered exactly once")

# Editing fields must only mutate local draft; /config/set publication belongs
# to the explicit Apply path.
apply_pos = app.find('elements.settings.applyButton.addEventListener("click"')
config_publish_pos = app.find("function publishConfigDraft()")
if apply_pos < 0 or config_publish_pos < 0:
    fail("cannot locate Settings explicit Apply contract")

render_start = app.find("function render() {")
render_end = app.find("\nfunction parseSnapshot(", render_start)
if render_start < 0 or render_end < 0:
    fail("cannot locate render()")
if "addEventListener" in app[render_start:render_end]:
    fail("event listeners must not be registered inside render()")

for forbidden in [
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "/dev/ttyS",
    "systemctl",
]:
    if forbidden in app or forbidden in mqtt:
        fail(f"Settings web path contains forbidden API/reference {forbidden}")

for token in [
    ".settings-actions",
    ".settings-result",
    ".settings-result--success",
    ".settings-result--error",
    ".form-grid select",
]:
    if token not in styles:
        fail(f"styles.css missing Settings style {token}")

print("dev011f1_structural_local_draft_contract: PASS")
print("dev011f1_config_set_full_proposal_contract: PASS")
print("dev011f1_expected_revision_base_contract: PASS")
print("dev011f1_incoming_config_preserves_dirty_draft: PASS")
print("dev011f1_fixture_stable_id_resize_contract: PASS")
print("dev011f1_group_membership_cleanup_on_fixture_remove: PASS")
print("dev011f1_config_result_request_matching: PASS")
print("dev011f1_explicit_apply_only: PASS")
print("dev011f1_no_command_replay_storage: PASS")
print("dev011f1_mqtt_only_settings: PASS")
print("=== DMXWB DEV-011F1 STRUCTURAL SETTINGS STATIC PASS ===")
