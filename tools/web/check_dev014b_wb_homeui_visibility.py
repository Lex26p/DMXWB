#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MQTT_CONTRACT = REPO_ROOT / "src" / "mqtt_contract.cpp"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


prerequisite = REPO_ROOT / "tools" / "web" / "check_dev014a_fixture_crud.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

source = MQTT_CONTRACT.read_text(encoding="utf-8")


def function_block(name: str, next_name: str) -> str:
    start = source.find(name)
    end = source.find(next_name, start + len(name))
    if start < 0 or end < 0:
        fail(f"cannot locate MQTT metadata function {name}")
    return source[start:end]


blocks = {
    "Fixture": function_block(
        "build_fixture_metadata_publications",
        "build_fixture_state_publications",
    ),
    "Group": function_block(
        "build_group_metadata_publications",
        "build_group_state_publications",
    ),
    "Scene": function_block(
        "build_scene_metadata_publications",
        "build_scene_state_publications",
    ),
}

for label, block in blocks.items():
    if '"hidden":true' in block:
        fail(f"{label} contains literal hidden metadata")
    calls = re.findall(r"make_control_meta\([^;]+?\)", block, re.S)
    if not calls:
        fail(f"{label} metadata contains no controls")
    for call in calls:
        if not re.search(r"make_control_meta\(\s*\"[^\"]+\",\s*(?:true|false),\s*false,", call):
            fail(f"{label} contains a control that is not explicitly visible")

for token in [
    'build_fixture_metadata_publications',
    'build_group_metadata_publications',
    'build_scene_metadata_publications',
    '"pushbutton", false, false, "Apply", "Применить"',
]:
    if token not in source:
        fail(f"missing WB HomeUI publication contract {token}")

print("dev014b_fixture_controls_visible_in_standard_wb_homeui: PASS")
print("dev014b_group_controls_visible_in_standard_wb_homeui: PASS")
print("dev014b_scene_controls_visible_in_standard_wb_homeui: PASS")
print("dev014b_structural_crud_remains_dedicated_web: PASS")
print("=== DMXWB DEV-014B STANDARD WB HOMEUI VISIBILITY STATIC PASS ===")
