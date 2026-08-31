#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        fail(f"{label}: missing {token!r}")


service = (REPO_ROOT / "deploy" / "dmxwb.service").read_text(encoding="utf-8")
production = (REPO_ROOT / "src" / "production_main.cpp").read_text(encoding="utf-8")
integrated = (REPO_ROOT / "src" / "integrated_runtime.cpp").read_text(encoding="utf-8")
mqtt_contract = (REPO_ROOT / "src" / "mqtt_contract.cpp").read_text(encoding="utf-8")

for token in [
    "Type=simple",
    "ExecStart=/usr/local/bin/dmxwb",
    "Restart=on-failure",
    "RestartSec=2s",
    "KillSignal=SIGTERM",
    "StandardOutput=journal",
    "StandardError=journal",
    "WantedBy=multi-user.target",
]:
    require(service, token, "systemd service")

for forbidden in ["Requires=mosquitto", "Wants=mosquitto", "After=mosquitto"]:
    if forbidden in service:
        fail(f"systemd service must not make Mosquitto a startup dependency: {forbidden}")

for token in [
    "event=startup",
    "event=shutdown",
    "event=stopped",
    "event=mqtt_connected",
    "mqtt_recovered",
    "event=mqtt_lost",
    "event=dmx_error",
    "dmx_recovered",
    "event=artnet_source",
    "event=artnet_error",
    "artnet_recovered",
    "event=config_applied",
    "event=fatal",
]:
    require(production, token, "production journald event contract")

# Production logs are event based. Per-frame logging would directly contradict the
# fixed DMX timing path and is forbidden here.
for forbidden in ["event=dmx_frame", "event=artnet_packet", "event=mqtt_command"]:
    if forbidden in production:
        fail(f"high-rate logging leaked into production main: {forbidden}")

for token in [
    "kOperationalStatusPublishInterval = std::chrono::seconds{1}",
    "kOperationalStatusOfflineRetryInterval",
    "publish_operational_status_if_due",
    "publish_operational_status_now",
    "mqtt.publish_all(publications)",
    r'\"diagnostics\"',
    r'\"selected_source\"',
    r'\"frames_sent\"',
    r'\"recovery_state\"',
    r'\"active_source_ip\"',
    r'\"packets_received\"',
    r'\"snapshots_superseded\"',
    r'\"revision\"',
    "std::lock_guard lock{artnet_mutex}",
]:
    require(integrated, token, "operational status contract")

# Existing Web status fields stay intact, and detailed diagnostics are additive.
for token in [
    '\\"application\\"',
    '\\"dmx\\"',
    '\\"mqtt\\"',
    '\\"artnet\\"',
    '\\"configuration\\"',
    '\\"last_error\\"',
]:
    require(integrated, token, "top-level /dmxwb/status compatibility")

# The standard WB HomeUI contract remains Status/Source only. Fixture/Group/Scene
# controls are still published for the dedicated Web client but hidden from the
# stock WB controls surface.
def function_block(name: str, next_name: str | None) -> str:
    start = mqtt_contract.find(name)
    if start < 0:
        fail(f"MQTT contract function not found: {name}")
    if next_name is None:
        return mqtt_contract[start:]
    end = mqtt_contract.find(next_name, start + len(name))
    if end < 0:
        fail(f"MQTT contract next function not found: {next_name}")
    return mqtt_contract[start:end]

fixture_block = function_block(
    "build_fixture_metadata_publications",
    "build_fixture_state_publications",
)
group_block = function_block(
    "build_group_metadata_publications",
    "build_group_state_publications",
)
scene_block = function_block(
    "build_scene_metadata_publications",
    "build_scene_state_publications",
)

for label, block in [
    ("Fixture", fixture_block),
    ("Group", group_block),
    ("Scene", scene_block),
]:
    if not re.search(r"make_control_meta\([^;]*?false,\s*true,", block, re.S):
        fail(f"{label} controls are no longer hidden from standard WB HomeUI")

system_block = function_block(
    "build_system_metadata_publications",
    "build_system_state_publications",
)
if len(re.findall(r"make_control_meta\([^;]*?false,\s*false,", system_block, re.S)) < 1:
    fail("DMXWB system Status/Source surface is unexpectedly hidden")

for forbidden in [
    "prometheus",
    "telemetry_database",
    "metrics_server",
    "monitoring_dashboard",
]:
    if forbidden in (production + integrated).lower():
        fail(f"out-of-scope monitoring subsystem found: {forbidden}")

print("dev012b_systemd_unit_contract: PASS")
print("dev012b_no_mosquitto_systemd_dependency: PASS")
print("dev012b_bounded_journald_event_contract: PASS")
print("dev012b_structured_retained_status_contract: PASS")
print("dev012b_artnet_diagnostics_thread_guard: PASS")
print("dev012b_standard_wb_homeui_hidden_controls_contract: PASS")
print("dev012b_no_monitoring_subsystem: PASS")
print("=== DMXWB DEV-012B STATIC OPERATIONAL CONTRACT PASS ===")
