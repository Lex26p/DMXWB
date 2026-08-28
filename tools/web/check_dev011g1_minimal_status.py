#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = REPO_ROOT / "src" / "mqtt_controller.cpp"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011ru1_russian_ui.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

source = SOURCE.read_text(encoding="utf-8")
marker = "std::string MqttController::build_status_json"
start = source.find(marker)
if start < 0:
    fail("MqttController::build_status_json is missing")

block = source[start:]

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

for token in [
    '\\"dmx\\":\\"controller\\"',
    '\\"mqtt\\":\\"controller\\"',
    '\\"artnet\\":\\"controller\\"',
]:
    if token not in block:
        fail(f"minimal subsystem status is missing: {token}")

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

# MqttController intentionally must not be coupled to transport/runtime diagnostic
# objects just to satisfy the Web status contract.
for forbidden in [
    "DmxOutput",
    "DmxOutputDiagnostics",
    "ArtNetRuntime",
    "ArtNetRuntimeDiagnostics",
    "MqttClientDiagnostics",
]:
    if forbidden in block:
        fail(f"status builder gained forbidden runtime coupling: {forbidden}")

model = (REPO_ROOT / "www" / "dmxwb" / "model.js").read_text(encoding="utf-8")
if 'controller: "Работает"' not in model:
    fail("Russian Web no longer maps minimal controller status")

print("dev011g1_required_six_status_fields: PASS")
print("dev011g1_minimal_subsystem_status_only: PASS")
print("dev011g1_no_extended_monitoring: PASS")
print("dev011g1_no_runtime_diagnostics_coupling: PASS")
print("dev011g1_russian_web_status_mapping_preserved: PASS")
print("=== DMXWB DEV-011 MINIMAL STATUS CONTRACT PASS ===")
