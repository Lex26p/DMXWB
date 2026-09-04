#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-linux-dev013a"
REPORT="${REPO_ROOT}/docs/DEV013A_FINAL_HOST_REPORT.txt"
BUILD_JOBS="${DMXWB_BUILD_JOBS:-2}"
SOURCE_ID="${1:-dev013a-final}"

if [[ $# -gt 1 ]]; then
    echo "Usage: bash tools/wb8/run_dev013a_final_host_acceptance.sh [SOURCE_ID]" >&2
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

echo "=== DMXWB DEV-013A FINAL HOST + ARM64 ACCEPTANCE ==="
echo "source_id: ${SOURCE_ID}"

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
echo "dev013a_production_cli_contract: PASS"

echo
echo "=== Current Web functional contracts ==="
# These entry points include the earlier Web prerequisites transitively. Running
# every historical checker separately would repeat the same prerequisite chain.
python3 "${REPO_ROOT}/tools/web/check_dev011g1_minimal_status.py"
python3 "${REPO_ROOT}/tools/web/check_dev011f4_runtime_config_apply.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b_operational_contract.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_daemon_availability.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_config_reconciliation.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_scene_create_idempotency.py"
python3 "${REPO_ROOT}/tools/web/check_dev012b5_numeric_validation.py"

echo
echo "=== Bullseye ARM64 production build and final offline bundle ==="
bash "${SCRIPT_DIR}/run_dev012c4_final_bundle_regression.sh" "${SOURCE_ID}"

echo
echo "dev013a_result: PASS"
echo "=== DMXWB DEV-013A FINAL HOST + ARM64 ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"
