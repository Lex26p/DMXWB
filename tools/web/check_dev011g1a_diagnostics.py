#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011f2_group_membership.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")
mqtt = (WEB_ROOT / "mqtt-client.js").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for field in [
    "application",
    "dmx",
    "mqtt",
    "artnet",
    "configuration",
    "last_error",
]:
    if f'data-status-field="{field}"' not in html:
        fail(f"minimal status view missing {field}")

for token in [
    "statusSummary",
    "renderStatus",
    "MQTT_STATUS_TOPIC",
]:
    if token not in app and token not in model and token not in mqtt:
        fail(f"minimal status path missing {token}")

# DEV-011 needs a small status view, not a monitoring dashboard. Detailed
# operational counters remain internal/acceptance concerns and are not part of
# the browser model/API contract.
for forbidden in [
    "frames_sent",
    "missed_deadlines",
    "successful_connections",
    "commands_processed",
    "datagrams_received",
    "poll_replies_sent",
    "receive_errors",
    "send_errors",
    "metric-card",
    "diagnostic-grid",
    "mqtt-endpoint",
    "config-revision",
    "state-status",
    "source-badge",
    "source-note",
    "page-subtitle",
    "settings-group-card__id",
    "scene-card__meta",
    "mqttTransportDescriptor",
    "snapshotCount",
]:
    if forbidden in html or forbidden in app or forbidden in model or forbidden in styles:
        fail(f"DEV-011 scope audit found unnecessary browser feature {forbidden}")

for forbidden_ui_text in [
    "нет snapshot",
    "Runtime state",
    "Scene lifecycle",
    "Group membership",
    "Сбросить draft",
    "Ожидание backend",
    "подтверждено backend",
    "подтверждена runtime state",
]:
    if forbidden_ui_text in html or forbidden_ui_text in app:
        fail(f"development-facing UI copy remains: {forbidden_ui_text}")

if 'maxlength="120"' in html:
    fail("Scene name must not invent a browser-only length restriction")

for required in [
    'id="settings-reset-button"',
    "Отменить изменения",
    "Конфигурация изменилась",
    "revision_conflict",
]:
    if required not in html and required not in app:
        fail(f"revision conflict recovery contract missing {required}")

for forbidden in [
    "localStorage",
    "sessionStorage",
    "fetch(",
    "XMLHttpRequest",
    "systemctl",
]:
    if forbidden in app or forbidden in mqtt:
        fail(f"forbidden direct/runtime API found: {forbidden}")

print("dev011_revision_required_features_preserved: PASS")
print("dev011_revision_status_is_minimal: PASS")
print("dev011_revision_extended_monitoring_removed: PASS")
print("dev011_revision_development_ui_copy_removed: PASS")
print("dev011_revision_scene_browser_only_limit_removed: PASS")
print("dev011_revision_revision_conflict_recovery_preserved: PASS")
print("dev011_revision_mqtt_only_boundary_preserved: PASS")
print("=== DMXWB DEV-011 SCOPE REVISION MINIMAL WEB PASS ===")
