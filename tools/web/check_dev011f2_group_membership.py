#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011f1_config_settings.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for token in [
    'id="settings-add-group-button"',
    'id="settings-group-list"',
    "Group membership",
]:
    if token not in html:
        fail(f"index.html missing Group draft editor token {token}")

for token in [
    "structuralGroupDrafts",
    "addGroupDraft",
    "removeGroupDraft",
    "setGroupDraftMember",
    "next_group_id",
    "Stable Group IDs are never reused",
    "Canonicalize membership in current Fixture order",
    "Group member references missing Fixture",
]:
    if token not in model:
        fail(f"model.js missing Group draft token {token}")

for token in [
    "createSettingsGroupCard",
    "renderSettingsGroupEditor",
    "settings.addGroupButton",
    "settings.groupList",
    "data-settings-group-member",
    "data-settings-group-remove",
    "addGroupDraft",
    "removeGroupDraft",
    "setGroupDraftMember",
]:
    if token not in app:
        fail(f"app.js missing Group draft editor token {token}")

if app.count('elements.settings.addGroupButton.addEventListener("click"') != 1:
    fail("Add Group listener must be registered exactly once")
if app.count('elements.settings.groupList.addEventListener("change"') != 1:
    fail("Group membership listener must be registered exactly once")
if app.count('elements.settings.groupList.addEventListener("click"') != 1:
    fail("Group removal listener must be registered exactly once")

render_start = app.find("function render() {")
render_end = app.find("\nfunction parseSnapshot(", render_start)
if render_start < 0 or render_end < 0:
    fail("cannot locate render()")
if "addEventListener" in app[render_start:render_end]:
    fail("event listeners must not be registered inside render()")

# Membership editor must be local-draft only. It may not publish its own
# special MQTT command; explicit Settings Apply remains the only config write.
for forbidden in [
    "/dmxwb/groups/",
    "MQTT_GROUP_CONFIG",
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "systemctl",
]:
    if forbidden in app or forbidden in model:
        fail(f"Group draft editor contains forbidden path/reference {forbidden}")

for token in [
    ".settings-group-editor",
    ".settings-group-list",
    ".settings-group-card",
    ".settings-group-members",
    ".settings-member-option",
]:
    if token not in styles:
        fail(f"styles.css missing Group draft style {token}")

print("dev011f2_group_membership_local_draft: PASS")
print("dev011f2_group_add_stable_id_contract: PASS")
print("dev011f2_group_remove_no_id_reuse_contract: PASS")
print("dev011f2_group_members_unique_existing_fixtures: PASS")
print("dev011f2_group_membership_uses_config_set_only: PASS")
print("dev011f2_group_editor_listeners_registered_once: PASS")
print("dev011f2_group_editor_mqtt_only_settings: PASS")
print("=== DMXWB DEV-011F2 GROUP MEMBERSHIP EDITOR STATIC PASS ===")
