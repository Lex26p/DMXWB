#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-linux-dev014c"
REPORT="${REPO_ROOT}/docs/DEV014C_FINAL_HOST_REPORT.txt"
BUILD_JOBS="${DMXWB_BUILD_JOBS:-2}"
SOURCE_ID="${1:-dev014c-final}"

if [[ $# -gt 1 ]]; then
    echo "Usage: bash tools/wb8/run_dev014c_final_host_acceptance.sh [SOURCE_ID]" >&2
    exit 2
fi
if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "DMXWB_BUILD_JOBS должен быть положительным целым числом." >&2
    exit 2
fi
if [[ ! "${SOURCE_ID}" =~ ^[A-Za-z0-9._+-]{1,128}$ ]]; then
    echo "SOURCE_ID должен содержать 1..128 безопасных символов." >&2
    exit 2
fi

for command_name in bash cmake ctest grep python3 rm tee; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    fi
done

: > "${REPORT}"
exec > >(tee -a "${REPORT}") 2>&1

echo "=== DMXWB DEV-014C FINAL HOST + ARM64 ACCEPTANCE ==="
echo "source_id: ${SOURCE_ID}"

echo
echo "=== DEV-014C acceptance script syntax ==="
bash -n "${SCRIPT_DIR}/run_dev014c_clean_state_acceptance.sh"
bash -n "${SCRIPT_DIR}/run_dev012d3_reboot_runtime_acceptance.sh"
echo "dev014c_acceptance_script_syntax: PASS"

echo
echo "=== Clean native Linux warnings-as-errors build ==="
rm -rf -- "${BUILD_DIR}"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_TESTING=ON \
    -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

echo
echo "=== Complete registered CTest suite ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure
"${BUILD_DIR}/dmxwb" --version
"${BUILD_DIR}/dmxwb" --help | grep -Fq '/etc/dmxwb/config.json'
"${BUILD_DIR}/dmxwb" --help | grep -Fq '/var/lib/dmxwb/state.json'
echo "dev014c_production_cli_contract: PASS"

echo
echo "=== Corrected entity-management Web and WB metadata contracts ==="
python3 "${REPO_ROOT}/tools/web/check_dev014b_wb_homeui_visibility.py"

echo
echo "=== Bullseye ARM64 production build and replacement offline bundle ==="
bash "${SCRIPT_DIR}/run_dev012c4_final_bundle_regression.sh" "${SOURCE_ID}"

echo
echo "dev014c_host_result: PASS"
echo "=== DMXWB DEV-014C FINAL HOST + ARM64 ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"
