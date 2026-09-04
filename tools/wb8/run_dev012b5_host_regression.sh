#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

echo "=== DEV-012B5 native + full CTest + Bullseye ARM64 regression ==="
bash "${SCRIPT_DIR}/build_bullseye_arm64.sh"

echo "=== DEV-012B5 affected Web contracts ==="
python3 "${REPO_ROOT}/tools/web/check_dev012b5_daemon_availability.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_config_reconciliation.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_scene_create_idempotency.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_numeric_validation.py"

echo "=== DMXWB DEV-012B5 HOST + ARM64 REGRESSION PASS ==="
