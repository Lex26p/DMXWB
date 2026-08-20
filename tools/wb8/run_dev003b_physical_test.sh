#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run_dev003b_physical_test.sh user@wb8-host [start-channel]

Runs the DEV-003B physical RGBW DMX acceptance test on /dev/ttyRS485-1.
The controller does not need Internet access. The target binary is copied from
artifacts/wb8-bullseye-arm64/dmxwb over the local SSH connection.

start-channel defaults to 1 and must be in 1..509.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
START_CHANNEL="${2:-1}"
PORT="/dev/ttyRS485-1"
FRAMES="${DMXWB_DEV003B_FRAMES:-120}"

if [[ "${TARGET}" == *"<"* || "${TARGET}" == *">"* ]]; then
    echo "Use a real controller address, for example root@10.200.200.1" >&2
    exit 2
fi
if [[ ! "${START_CHANNEL}" =~ ^[0-9]+$ ]] || (( START_CHANNEL < 1 || START_CHANNEL > 509 )); then
    echo "start-channel must be an integer in range 1..509" >&2
    exit 2
fi
if [[ ! "${FRAMES}" =~ ^[0-9]+$ ]] || (( FRAMES < 1 || FRAMES > 100000 )); then
    echo "DMXWB_DEV003B_FRAMES must be an integer in range 1..100000" >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV003B_HARDWARE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev003b"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev003b-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

for command_name in ssh scp sha256sum grep; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Target binary not found: ${BINARY}" >&2
    echo "Run tools/wb8/build_bullseye_arm64.sh first." >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=180"
    -o "StrictHostKeyChecking=accept-new"
)
SCP_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=180"
    -o "StrictHostKeyChecking=accept-new"
)

SSH_OPENED=0
SERVICE_WAS_ACTIVE=0
SERVICE_STOPPED=0
SERVICE_RESTORED=0
PORT_DECLARED_FREE=0
REMOTE_READY=0
TEST_SUCCEEDED=0

remote() {
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$@"
}

restore_service() {
    if (( SERVICE_WAS_ACTIVE == 1 && SERVICE_STOPPED == 1 && SERVICE_RESTORED == 0 )); then
        echo "Restoring wb-mqtt-serial..."
        if remote "systemctl start wb-mqtt-serial"; then
            SERVICE_RESTORED=1
            echo "wb-mqtt-serial restored."
        else
            echo "WARNING: failed to restart wb-mqtt-serial; restart it manually on the controller." >&2
            return 1
        fi
    fi
    return 0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM

    if (( SSH_OPENED == 1 )); then
        if (( TEST_SUCCEEDED == 0 && SERVICE_STOPPED == 1 && REMOTE_READY == 1 )); then
            echo
            echo "Best-effort safety all-off before leaving DEV-003B..."
            remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --frames 8" >/dev/null 2>&1 || true
        fi

        restore_service || status=1
        remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi

    rmdir "${CONTROL_DIR}" >/dev/null 2>&1 || true
    exit "${status}"
}
trap cleanup EXIT INT TERM

record() {
    printf '%s\n' "$*" | tee -a "${REPORT}"
}

run_pattern() {
    local pattern="$1"
    local expected="$2"
    local output
    local answer

    echo
    echo "Pattern: ${pattern}"
    echo "Expected: ${expected}"
    echo "Watch the fixture during the ~3 second burst and check for visible flicker."
    read -r -p "Press Enter to start this pattern... " _

    record ""
    record "--- pattern: ${pattern} ---"
    record "expected: ${expected}"

    if ! output="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-test '${pattern}' --port '${PORT}' --start-channel '${START_CHANNEL}' --frames '${FRAMES}'" 2>&1)"; then
        printf '%s\n' "${output}" | tee -a "${REPORT}"
        record "transport_result: FAIL"
        record "=== DEV-003B hardware execution FAIL ==="
        echo "Transport failed for pattern ${pattern}. Stay on DEV-003." >&2
        return 1
    fi

    printf '%s\n' "${output}" | tee -a "${REPORT}"
    if ! grep -Fq "DMX test completed; serial port closed cleanly." <<<"${output}"; then
        record "transport_result: FAIL (clean serial close marker missing)"
        record "=== DEV-003B hardware execution FAIL ==="
        echo "Clean serial close marker was not observed." >&2
        return 1
    fi

    read -r -p "Was the physical result correct AND free of visible flicker? [y/N] " answer
    case "${answer}" in
        y|Y|yes|YES|Yes)
            record "transport_result: PASS"
            record "user_observation: PASS"
            ;;
        *)
            record "transport_result: PASS"
            record "user_observation: FAIL"
            record "=== DEV-003B hardware execution FAIL ==="
            echo "Physical observation failed for pattern ${pattern}. Stay on DEV-003." >&2
            return 1
            ;;
    esac
}

echo "DMXWB DEV-003B physical test"
echo "Target: ${TARGET}"
echo "Port: ${PORT}"
echo "RGBW start channel: ${START_CHANNEL}"
echo "Frames per pattern: ${FRAMES}"
echo
echo "The WB8 needs no Internet access for this test."
echo "If wb-mqtt-serial is active, this script can stop it temporarily."
echo "Stopping the service temporarily affects ALL serial devices managed by wb-mqtt-serial."
echo "If that is not acceptable, disable only ${PORT} in the WB Serial Device Driver Configuration first."
echo

read -r -p "Is the DMX fixture safely wired to the selected RS-485 port and ready for the test? [y/N] " READY
case "${READY}" in
    y|Y|yes|YES|Yes) ;;
    *) echo "Cancelled before touching the controller."; exit 2 ;;
esac

echo
echo "Opening one reusable SSH connection to ${TARGET}."
echo "Enter the controller password when SSH asks for it."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_DIR}/dmxwb"
remote "chmod 0755 '${REMOTE_DIR}/dmxwb'"
REMOTE_READY=1

SERVICE_STATE="$(remote "systemctl is-active wb-mqtt-serial 2>/dev/null || true" | tr -d '\r')"
if [[ "${SERVICE_STATE}" == "active" ]]; then
    SERVICE_WAS_ACTIVE=1
    echo
    echo "wb-mqtt-serial is active. Choose how /dev/ttyRS485-1 is freed:"
    echo "  s - stop the whole wb-mqtt-serial service temporarily (affects all serial devices)"
    echo "  p - the selected port is already disabled in WB Serial Device Driver Configuration"
    echo "  q - cancel"
    read -r -p "Choice [s/p/q]: " FREE_ANSWER
    case "${FREE_ANSWER}" in
        s|S)
            remote "systemctl stop wb-mqtt-serial"
            SERVICE_STOPPED=1
            ;;
        p|P)
            PORT_DECLARED_FREE=1
            echo "Continuing with wb-mqtt-serial active because the user confirmed ${PORT} is disabled in its configuration."
            ;;
        *)
            echo "Cancelled: direct RS-485 testing requires the selected port to be free." >&2
            exit 2
            ;;
    esac
else
    echo "wb-mqtt-serial state: ${SERVICE_STATE:-unknown}; service will be left unchanged."
    PORT_DECLARED_FREE=1
fi

if (( PORT_DECLARED_FREE == 0 )) && [[ "$(remote "systemctl is-active wb-mqtt-serial 2>/dev/null || true" | tr -d '\r')" == "active" ]]; then
    echo "wb-mqtt-serial is still active; refusing to run direct DMX." >&2
    exit 1
fi

mkdir -p "$(dirname -- "${REPORT}")"
: > "${REPORT}"
record "=== DMXWB DEV-003B physical hardware report ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if command -v git >/dev/null 2>&1 && git -C "${REPO_ROOT}" rev-parse HEAD >/dev/null 2>&1; then
    record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
fi
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "port: ${PORT}"
record "start_channel: ${START_CHANNEL}"
record "frames_per_pattern: ${FRAMES}"
record "wb_mqtt_serial_initial_state: ${SERVICE_STATE:-unknown}"
record "port_declared_free_in_driver_config: ${PORT_DECLARED_FREE}"
record ""
record "--- target identity ---"
remote "printf 'model: '; tr -d '\\0' </proc/device-tree/model 2>/dev/null || true; echo; uname -a; if [ -r /etc/wb-release ]; then cat /etc/wb-release; fi" | tee -a "${REPORT}"

run_pattern all-off "all four RGBW channels OFF"
run_pattern red "R active only; G/B/W OFF"
run_pattern green "G active only; R/B/W OFF"
run_pattern blue "B active only; R/G/W OFF"
run_pattern white "W active only; R/G/B OFF"
run_pattern all-on "R/G/B/W all active at 255"

record ""
record "--- final safe all-off / reopen check ---"
FINAL_OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --frames 20" 2>&1)"
printf '%s\n' "${FINAL_OUTPUT}" | tee -a "${REPORT}"
if ! grep -Fq "DMX test completed; serial port closed cleanly." <<<"${FINAL_OUTPUT}"; then
    record "final_all_off_reopen_check: FAIL"
    record "=== DEV-003B hardware execution FAIL ==="
    exit 1
fi
record "final_all_off_reopen_check: PASS"
record "serial_reopen_across_separate_runs: PASS"

if (( SERVICE_WAS_ACTIVE == 1 )); then
    restore_service
    record "wb_mqtt_serial_restore: PASS"
else
    record "wb_mqtt_serial_restore: not-required"
fi

record "kernel_or_wbec_patch_required: NO"
record "=== DEV-003B hardware execution PASS ==="
TEST_SUCCEEDED=1

echo
echo "Report saved to: ${REPORT}"
echo "DEV-003B PASS. Leave the generated report in the repository for the final step commit."
