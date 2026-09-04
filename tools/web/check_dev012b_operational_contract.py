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
mqtt_controller = (REPO_ROOT / "src" / "mqtt_controller.cpp").read_text(encoding="utf-8")
cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
status_test = (REPO_ROOT / "tests" / "test_operational_status.cpp").read_text(encoding="utf-8")
systemd_acceptance = (
    REPO_ROOT / "tools" / "wb8" / "run_dev012b_systemd_acceptance.sh"
).read_text(encoding="utf-8")
counter_regression_wrapper = (
    REPO_ROOT / "tools" / "wb8" / "run_dev012b3_counter_isolation_regression.sh"
).read_text(encoding="utf-8")

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
    "runtime.operational_state()",
]:
    require(production, token, "production journald event contract")

# Production logs are event based. Per-frame logging would directly contradict the
# fixed DMX timing path and is forbidden here.
for forbidden in ["event=dmx_frame", "event=artnet_packet", "event=mqtt_command"]:
    if forbidden in production:
        fail(f"high-rate logging leaked into production main: {forbidden}")

for forbidden in [
    "runtime.diagnostics()",
    " connections=",
    " disconnects=",
    " open_failures=",
    " send_failures=",
    " recoveries=",
    " bind_failures=",
    " receive_errors=",
]:
    if forbidden in production:
        fail(f"counter-based production logging remains: {forbidden}")

builder_start = integrated.find("OperationalStatusPayload build_operational_status_payload(")
builder_end = integrated.find("class IntegratedRuntime::Impl", builder_start)
if builder_start < 0 or builder_end < 0:
    fail("operational status payload builder not found")
status_builder = integrated[builder_start:builder_end]

for token in [
    "kOperationalStatusPublishInterval = std::chrono::seconds{1}",
    "kOperationalStatusOfflineRetryInterval",
    "publish_operational_status_if_due",
    "publish_operational_status_now",
    "mqtt.publish_all(publications)",
    r'\"diagnostics\"',
    r'\"selected_source\"',
    r'\"recovery_state\"',
    r'\"slot_count\"',
    r'\"refresh_hz\"',
    r'\"physical_slot_limit\"',
    r'\"active_generation\"',
    r'\"connected\"',
    r'\"active_source_ip\"',
    r'\"active_source_physical\"',
    r'\"last_packet_age_ms\"',
    r'\"last_sequence\"',
    r'\"sync_mode\"',
    r'\"last_sync_age_ms\"',
    r'\"conflicting_source_ip\"',
    r'\"conflicting_source_physical\"',
    r'\"output_mode\"',
    r'\"transport_open\"',
    r'\"committed_revision\"',
    r'\"revision\"',
    "std::lock_guard lock{artnet_mutex}",
]:
    require(integrated, token, "operational status contract")

for forbidden in ["kMqttStatusTopic", '"/devices/dmxwb/controls/status"', '"controller"']:
    if forbidden in mqtt_controller:
        fail(f"Controller still owns operational status publication: {forbidden}")

for forbidden in [
    "frames_sent",
    "deadlines_missed",
    "packets_received",
    "datagrams_received",
    "commands_processed",
    "commands_rejected",
    "publications",
    "republishes",
    "snapshots_published",
    "snapshots_routed",
    "snapshots_superseded",
    "source_switches",
    "successful_connections",
    "disconnects",
    "open_failures",
    "send_failures",
    "publish_failures",
    "recoveries",
]:
    if f'\\\"{forbidden}\\\"' in status_builder:
        fail(f"cumulative telemetry field leaked into production status: {forbidden}")

for token in [
    "add_executable(dmxwb_operational_status_tests",
    "tests/test_operational_status.cpp",
    "add_test(NAME dmxwb.operational_status COMMAND dmxwb_operational_status_tests)",
]:
    require(cmake, token, "build-level production status assertion")

for token in [
    "build_operational_status_payload",
    "expect_no_cumulative_telemetry",
    "test_running_mqtt_status_is_factual",
    "test_artnet_conflict_status_identifies_sources",
    "test_stopping_status_is_off",
]:
    require(status_test, token, "production status behavioral contract test")

for token in [
    '"slot_count", "refresh_hz", "physical_slot_limit"',
    '"last_packet_age_ms", "last_sequence", "sync_mode", "last_sync_age_ms"',
    "reject_cumulative_fields(s)",
    "mqtt_reconnect_operational_state: PASS",
]:
    require(systemd_acceptance, token, "corrected WB8 operational acceptance")

for forbidden in [
    "status_value diagnostics.mqtt.disconnects",
    "status_value diagnostics.mqtt.successful_connections",
    'd["dmx"]["frames_sent"]',
]:
    if forbidden in systemd_acceptance:
        fail(f"WB8 acceptance still depends on cumulative status telemetry: {forbidden}")

for token in [
    "send_artdmx 0 0 255 0",
    "diagnostics.artnet.state ACTIVE",
    "diagnostics.artnet.output_mode live",
    "diagnostics.selected_source mqtt",
    "explicit_source_switch_same_process: PASS",
    "journald_bounded_event_count: PASS",
    "final_state_flush: PASS",
    "final_serial_port_release: PASS",
]:
    require(systemd_acceptance, token, "DEV-012B3 WB8 regression contract")

require(
    counter_regression_wrapper,
    "DMXWB_SYSTEMD_ACCEPTANCE_VARIANT=dev012b3",
    "DEV-012B3 acceptance wrapper",
)

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

# DEV-014B exposes the existing Fixture/Group/Scene controls in standard WB
# HomeUI. Structural creation and deletion remain exclusive to dedicated Web.
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
    if not re.search(r"make_control_meta\([^;]*?false,\s*false,", block, re.S):
        fail(f"{label} controls are not visible in standard WB HomeUI")
    if re.search(r"make_control_meta\([^;]*?false,\s*true,", block, re.S):
        fail(f"{label} still contains hidden writable controls")

system_block = function_block(
    "build_system_metadata_publications",
    "build_system_source_publications",
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
print("dev012b_no_cumulative_status_telemetry_contract: PASS")
print("dev012b_status_build_assertion_registered: PASS")
print("dev012b_corrected_wb8_acceptance_contract: PASS")
print("dev012b3_counter_isolation_regression_contract: PASS")
print("dev012b_artnet_diagnostics_thread_guard: PASS")
print("dev014b_standard_wb_homeui_visible_entity_controls_contract: PASS")
print("dev012b_no_monitoring_subsystem: PASS")
print("=== DMXWB DEV-012B STATIC OPERATIONAL CONTRACT PASS ===")
