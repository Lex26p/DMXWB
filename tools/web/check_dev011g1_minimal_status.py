#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
INTEGRATED_SOURCE = REPO_ROOT / "src" / "integrated_runtime.cpp"
CONTROLLER_SOURCE = REPO_ROOT / "src" / "mqtt_controller.cpp"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011ru1_russian_ui.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

integrated = INTEGRATED_SOURCE.read_text(encoding="utf-8")
controller = CONTROLLER_SOURCE.read_text(encoding="utf-8")
marker = "OperationalStatusPayload build_operational_status_payload"
start = integrated.find(marker)
if start < 0:
    fail("integrated operational status builder is missing")
end = integrated.find("class IntegratedRuntime::Impl", start)
if end < 0:
    fail("integrated operational status builder boundary is missing")

block = integrated[start:end]

for token in [
    '\\"application\\"',
    '\\"dmx\\"',
    '\\"mqtt\\"',
    '\\"artnet\\"',
    '\\"configuration\\"',
    '\\"last_error\\"',
]:
    if token not in block:
        fail(f"minimal /dmxwb/status field is missing: {token}")

for forbidden in ["kMqttStatusTopic", '"/devices/dmxwb/controls/status"', '"controller"']:
    if forbidden in controller:
        fail(f"Controller still publishes or synthesizes operational status: {forbidden}")

for forbidden in [
    "frames_sent",
    "commands_processed",
    "successful_connections",
    "datagrams_received",
    "poll_replies_sent",
    "missed_deadlines",
    "receive_errors",
    "send_errors",
]:
    if forbidden in block:
        fail(f"extended monitoring leaked into minimal status contract: {forbidden}")

print("dev011g1_required_six_status_fields: PASS")
print("dev011g1_integrated_factual_status_owner: PASS")
print("dev011g1_no_extended_monitoring: PASS")
print("dev011g1_controller_status_publication_absent: PASS")
print("=== DMXWB DEV-011 MINIMAL STATUS CONTRACT PASS ===")
