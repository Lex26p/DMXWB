#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

web_check = REPO_ROOT / "tools" / "web" / "check_dev011g1a_diagnostics.py"
result = subprocess.run([sys.executable, str(web_check)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

router_h = (REPO_ROOT / "include/dmxwb/dmx_source_router.hpp").read_text(encoding="utf-8")
router_cpp = (REPO_ROOT / "src/dmx_source_router.cpp").read_text(encoding="utf-8")
router_test = (REPO_ROOT / "tests/test_dmx_source_router.cpp").read_text(encoding="utf-8")
shared_runtime = (REPO_ROOT / "src/integrated_runtime.cpp").read_text(encoding="utf-8")
physical_sink_h = (
    REPO_ROOT / "include/dmxwb/dmx_output_physical_sink.hpp"
).read_text(encoding="utf-8")
physical_sink_cpp = (
    REPO_ROOT / "src/dmx_output_physical_sink.cpp"
).read_text(encoding="utf-8")
physical_sink_test = (
    REPO_ROOT / "tests/test_dmx_output_physical_sink.cpp"
).read_text(encoding="utf-8")
acceptance_frontend = (
    REPO_ROOT / "src/dev010_source_acceptance_main.cpp"
).read_text(encoding="utf-8")

for token in [
    "clear_artnet_snapshot",
    "latest_artnet_.reset()",
    "update_artnet_output_active_locked()",
]:
    if token not in router_h + router_cpp:
        fail(f"Art-Net old-universe invalidation missing {token}")

for token in [
    "test_clear_artnet_snapshot_invalidates_old_universe_without_blackout",
    "old-universe Art-Net data is never replayed after reconfiguration",
]:
    if token not in router_test:
        fail(f"router reconfiguration test missing {token}")

# DEV-012A moves the already-proven structural transport application from the
# DEV-010 acceptance executable into the shared integrated runtime. Check the
# actual owner of that behavior rather than requiring production orchestration
# to remain embedded in an acceptance main().
for token in [
    "physical_sink.reconfigure_port(current_config.dmx_port)",
    "current_config.dmx_port != applied_dmx_port",
    "current_config.artnet_universe != applied_artnet_universe",
    "router.clear_artnet_snapshot()",
    "ArtNetRuntime::create(",
]:
    if token not in shared_runtime:
        fail(f"shared runtime structural apply missing {token}")

# Port replacement and replay of the latest completed whole snapshot are owned by
# the physical sink. They were moved out of IntegratedRuntime when publication and
# reconfiguration were serialized behind the same sink mutex.
for token in [
    "bool DmxOutputPhysicalSink::reconfigure_port(std::string port)",
    "latest_snapshot_ = snapshot",
    "replacement->publish_snapshot(*latest_snapshot_)",
]:
    if token not in physical_sink_h + physical_sink_cpp:
        fail(f"physical sink reconfiguration missing {token}")

for token in [
    "test_publish_and_reconfigure_are_serialized",
    "replacement receives the latest completed whole snapshot without loss",
]:
    if token not in physical_sink_test:
        fail(f"physical sink reconfiguration test missing {token}")

# The acceptance frontend keeps the stable diagnostic markers consumed by the
# existing WB8 helpers, but no longer owns the transport reconfiguration logic.
for token in [
    "runtime_dmx_port_applied",
    "runtime_artnet_port_address_applied",
    "final_dmx_port_reconfigure_failures",
    "final_artnet_universe_reconfigure_failures",
]:
    if token not in acceptance_frontend:
        fail(f"acceptance runtime marker missing {token}")

# Transport-specific config application belongs in the C++ runtime, never Web.
web_app = (REPO_ROOT / "www/dmxwb/app.js").read_text(encoding="utf-8")
for forbidden in [
    "ArtNetRuntime",
    "DmxOutput",
    "systemctl",
]:
    if forbidden in web_app:
        fail(f"browser improperly owns runtime reconfiguration: {forbidden}")

print("dev011f4_dmx_port_runtime_apply_contract: PASS")
print("dev011f4_dmx_port_preserves_latest_whole_snapshot: PASS")
print("dev011f4_artnet_universe_runtime_recreate_contract: PASS")
print("dev011f4_old_artnet_universe_cache_invalidated: PASS")
print("dev011f4_no_blackout_on_artnet_universe_change: PASS")
print("dev011f4_runtime_reconfigure_failures_are_fatal: PASS")
print("dev011f4_web_remains_mqtt_only: PASS")
print("=== DMXWB DEV-011F4 TRANSPORT STRUCTURAL APPLY STATIC PASS ===")
