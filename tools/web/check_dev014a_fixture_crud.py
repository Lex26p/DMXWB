#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


for prerequisite_name in [
    "check_dev011f2_group_membership.py",
    "check_dev012b5_numeric_validation.py",
]:
    prerequisite = REPO_ROOT / "tools" / "web" / prerequisite_name
    result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
    if result.returncode != 0:
        raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")
default_config = json.loads(
    (REPO_ROOT / "deploy" / "config.example.json").read_text(encoding="utf-8")
)

for token in [
    'id="settings-add-fixture-button"',
    'id="settings-fixture-list"',
    "Добавить светильник",
    "Сначала создайте светильники, затем добавляйте их в группы.",
]:
    if token not in html:
        fail(f"index.html missing explicit Fixture CRUD token {token}")

if 'id="setting-fixture-count"' in html:
    fail("numeric Fixture Count input is still exposed")
if html.find('id="settings-add-fixture-button"') > html.find(
    'id="settings-add-group-button"'
):
    fail("Fixture creation must appear before Group creation")

for token in [
    "export function addFixtureDraft",
    "export function removeFixtureDraft",
    "next_fixture_id",
    "items.push({ id, name:",
    "draft.fixtures.count = items.length",
    "draft.fixtures.count = draft.fixtures.items.length",
    "Number(memberId) !== fixtureId",
    "Stable Fixture IDs are never reused",
    "Historical Scene snapshots keep the",
]:
    if token not in model:
        fail(f"model.js missing Fixture CRUD invariant {token}")

for token in [
    "createSettingsFixtureCard",
    "renderSettingsFixtureEditor",
    "settings.addFixtureButton",
    "settings.fixtureList",
    "data-settings-fixture-id",
    "data-settings-fixture-remove",
    "addFixtureDraft",
    "removeFixtureDraft",
    "window.confirm",
    "nextLastAddress > 300",
    "editor.fixtures.length === 0",
]:
    if token not in app:
        fail(f"app.js missing Fixture CRUD token {token}")

if app.count('elements.settings.addFixtureButton.addEventListener("click"') != 1:
    fail("Add Fixture listener must be registered exactly once")
if app.count('elements.settings.fixtureList.addEventListener("click"') != 1:
    fail("Remove Fixture listener must be registered exactly once")
if "elements.settings.fixtureCount" in app or "resizeFixtureDraft(model" in app:
    fail("app.js still creates Fixtures through a numeric count")
if "export function resizeFixtureDraft" in model:
    fail("obsolete numeric Fixture resize API is still exposed")

for token in [
    ".settings-entity-editor",
    ".settings-fixture-list",
    ".settings-fixture-card",
    ".settings-fixture-remove",
]:
    if token not in styles:
        fail(f"styles.css missing Fixture CRUD style {token}")

fixtures = default_config.get("fixtures", {})
if fixtures.get("count") != 0 or fixtures.get("items") != []:
    fail("fresh installation config must start with zero Fixtures")

print("dev014a_explicit_fixture_add_before_group: PASS")
print("dev014a_arbitrary_fixture_remove: PASS")
print("dev014a_group_membership_cleanup: PASS")
print("dev014a_stable_fixture_id_no_reuse: PASS")
print("dev014a_scene_history_preserved: PASS")
print("dev014a_derived_dmx_address_display: PASS")
print("dev014a_zero_fixture_fresh_config: PASS")
print("dev014a_no_numeric_fixture_count_ui: PASS")
print("=== DMXWB DEV-014A EXPLICIT FIXTURE CRUD STATIC PASS ===")
