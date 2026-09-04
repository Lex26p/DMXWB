#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-linux"
BUILD_JOBS="${DMXWB_BUILD_JOBS:-2}"

if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "DMXWB_BUILD_JOBS должен быть положительным целым числом." >&2
    exit 2
fi

echo "=== DEV-012B4 native Linux warnings-as-errors build ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_TESTING=ON \
    -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

echo "=== DEV-012B4 full host CTest suite ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "=== DEV-012B4 Web timeout/correlation/reconnect contracts ==="
python3 "${REPO_ROOT}/tools/web/check_dev011f1_config_settings.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b_operational_contract.py"

echo "=== DEV-012B4 Bullseye ARM64 production build ==="
bash "${SCRIPT_DIR}/build_bullseye_arm64.sh"

echo "=== DMXWB DEV-012B4 HOST + ARM64 REGRESSION PASS ==="
