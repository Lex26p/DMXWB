#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 user@wb8-host" >&2
    exit 2
fi

TARGET="$1"

if [[ "${TARGET}" == *"<"* || "${TARGET}" == *">"* ]]; then
    echo "Replace the placeholder with the real controller address, for example:" >&2
    echo "  WB_HOST=root@10.200.200.1" >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
PROBE="${SCRIPT_DIR}/probe_target.sh"
REPORT="${REPO_ROOT}/docs/DEV003A_TARGET_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev003a"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

for command_name in ssh scp sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing command: ${command_name}" >&2
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
    -o "ControlPersist=120"
    -o "StrictHostKeyChecking=accept-new"
)
SCP_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=120"
    -o "StrictHostKeyChecking=accept-new"
)

cleanup() {
    ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    rmdir "${CONTROL_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "Opening one reusable SSH connection to ${TARGET}."
echo "Enter the controller password once when SSH asks for it."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"

ssh "${SSH_OPTS[@]}" "${TARGET}" "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${PROBE}" "${TARGET}:${REMOTE_DIR}/"
ssh "${SSH_OPTS[@]}" "${TARGET}" "chmod 0755 '${REMOTE_DIR}/dmxwb' '${REMOTE_DIR}/probe_target.sh'"

{
    echo "=== DMXWB DEV-003A target report ==="
    echo "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    if command -v git >/dev/null 2>&1 && git -C "${REPO_ROOT}" rev-parse HEAD >/dev/null 2>&1; then
        echo "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
        if [[ -n "$(git -C "${REPO_ROOT}" status --short)" ]]; then
            echo "source_worktree: modified (DEV-003A package applied, not committed yet)"
        else
            echo "source_worktree: clean"
        fi
    fi
    echo "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
    echo "target: ${TARGET}"
    echo
    ssh "${SSH_OPTS[@]}" "${TARGET}" "
        set -e
        '${REMOTE_DIR}/probe_target.sh'
        echo '--- target binary ---'
        if command -v file >/dev/null 2>&1; then file '${REMOTE_DIR}/dmxwb'; fi
        if command -v ldd >/dev/null 2>&1; then ldd '${REMOTE_DIR}/dmxwb' || true; fi
        echo '--- dmxwb --version ---'
        '${REMOTE_DIR}/dmxwb' --version
        echo '--- dmxwb --help ---'
        '${REMOTE_DIR}/dmxwb' --help
    "
    echo "=== DEV-003A target execution PASS ==="
} | tee "${REPORT}"

echo
echo "Report saved to: ${REPORT}"
echo "Do not proceed to physical DMX patterns until this report ends with DEV-003A target execution PASS."
