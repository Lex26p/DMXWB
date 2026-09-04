#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Usage:
  sudo bash uninstall.sh
  sudo bash uninstall.sh --purge

Without options, removes only the DMXWB application, systemd unit and static Web
files. Existing /etc/dmxwb/config.json and /var/lib/dmxwb/state.json are preserved.

--purge additionally removes those two DMXWB user-data files. Other files in the
same directories are never removed.

DESTDIR=/absolute/staging/root may be used for an isolated non-system test. In
DESTDIR mode no systemd commands are executed.
USAGE
}

PURGE=0
if [[ $# -gt 1 ]]; then
    usage >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    case "$1" in
        --purge)
            PURGE=1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
fi

for command_name in cp dirname mktemp realpath rm rmdir; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing required command: ${command_name}" >&2
        exit 1
    fi
done

DEST_ROOT="${DESTDIR:-}"
STAGING_MODE=0
if [[ -n "${DEST_ROOT}" ]]; then
    if [[ "${DEST_ROOT}" != /* || "${DEST_ROOT}" == "/" ]]; then
        echo "DESTDIR must be an absolute non-root staging directory." >&2
        exit 2
    fi
    if [[ -L "${DEST_ROOT}" ]]; then
        echo "DESTDIR must not be a symbolic link." >&2
        exit 2
    fi
    if [[ ! -d "${DEST_ROOT}" ]]; then
        echo "DESTDIR staging directory does not exist." >&2
        exit 2
    fi
    DEST_ROOT="$(realpath -m -- "${DEST_ROOT}")"
    if [[ "${DEST_ROOT}" == "/" ]]; then
        echo "DESTDIR must not resolve to the system root." >&2
        exit 2
    fi
    STAGING_MODE=1
elif [[ ${EUID} -ne 0 ]]; then
    echo "Production removal must be run as root." >&2
    exit 1
fi

if (( STAGING_MODE == 0 )); then
    if ! command -v systemctl >/dev/null 2>&1; then
        echo "Missing required command: systemctl" >&2
        exit 1
    fi
fi

target_path() {
    printf '%s%s' "${DEST_ROOT}" "$1"
}

BIN_DIR="$(target_path /usr/local/bin)"
CONFIG_DIR="$(target_path /etc/dmxwb)"
STATE_DIR="$(target_path /var/lib/dmxwb)"
UNIT_DIR="$(target_path /etc/systemd/system)"
WEB_DIR="$(target_path /var/www/dmxwb)"
TARGET_BINARY="${BIN_DIR}/dmxwb"
TARGET_CONFIG="${CONFIG_DIR}/config.json"
TARGET_STATE="${STATE_DIR}/state.json"
TARGET_UNIT="${UNIT_DIR}/dmxwb.service"

managed_targets=(
    "${TARGET_BINARY}"
    "${TARGET_UNIT}"
    "${WEB_DIR}/index.html"
    "${WEB_DIR}/app.js"
    "${WEB_DIR}/model.js"
    "${WEB_DIR}/mqtt-client.js"
    "${WEB_DIR}/styles.css"
)

removal_targets=("${managed_targets[@]}")
if (( PURGE == 1 )); then
    removal_targets+=("${TARGET_CONFIG}" "${TARGET_STATE}")
fi

assert_no_symlink_components() {
    local path="$1"
    local relative
    local current
    local component
    local -a components=()

    if (( STAGING_MODE == 1 )); then
        case "${path}" in
            "${DEST_ROOT}"/*) ;;
            *)
                echo "Refusing path outside DESTDIR: ${path}" >&2
                exit 1
                ;;
        esac
        relative="${path#"${DEST_ROOT}"/}"
        current="${DEST_ROOT}"
    else
        case "${path}" in
            /usr/local/bin/*|/etc/systemd/system/*|/var/www/dmxwb/*|/etc/dmxwb/*|/var/lib/dmxwb/*) ;;
            *)
                echo "Refusing path outside the fixed DMXWB locations: ${path}" >&2
                exit 1
                ;;
        esac
        relative="${path#/}"
        current=""
    fi

    IFS='/' read -r -a components <<<"${relative}"
    for component in "${components[@]}"; do
        [[ -n "${component}" ]] || continue
        current="${current}/${component}"
        if [[ -L "${current}" ]]; then
            echo "Refusing symbolic-link path component: ${current}" >&2
            exit 1
        fi
    done
}

for target in "${removal_targets[@]}"; do
    assert_no_symlink_components "${target}"
    if [[ -e "${target}" && ! -f "${target}" ]]; then
        echo "Removal target is not a regular file: ${target}" >&2
        exit 1
    fi
done

BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dmxwb-uninstall.XXXXXX")"
MUTATION_STARTED=0
COMMITTED=0
SERVICE_WAS_ACTIVE=0
SERVICE_WAS_ENABLED=0
declare -a backup_targets=()
declare -a backup_files=()
declare -a backup_present=()

backup_target() {
    local target="$1"
    local key="$2"
    backup_targets+=("${target}")
    backup_files+=("${BACKUP_DIR}/${key}")
    if [[ -f "${target}" ]]; then
        cp -a -- "${target}" "${BACKUP_DIR}/${key}"
        backup_present+=(1)
    else
        backup_present+=(0)
    fi
}

restore_service_state() {
    if (( STAGING_MODE == 1 )); then
        return
    fi
    systemctl daemon-reload >/dev/null 2>&1 || true
    if (( SERVICE_WAS_ENABLED == 1 )); then
        systemctl enable dmxwb.service >/dev/null 2>&1 || true
    else
        systemctl disable dmxwb.service >/dev/null 2>&1 || true
    fi
    if (( SERVICE_WAS_ACTIVE == 1 )); then
        systemctl start dmxwb.service >/dev/null 2>&1 || true
    else
        systemctl stop dmxwb.service >/dev/null 2>&1 || true
    fi
}

rollback() {
    local index
    local target_directory
    for ((index=${#backup_targets[@]} - 1; index>=0; --index)); do
        if [[ "${backup_present[index]}" == "1" ]]; then
            target_directory="$(dirname -- "${backup_targets[index]}")"
            if [[ ! -d "${target_directory}" ]]; then
                echo "Cannot restore missing target directory: ${target_directory}" >&2
                continue
            fi
            cp -a -- "${backup_files[index]}" "${backup_targets[index]}"
        else
            rm -f -- "${backup_targets[index]}"
        fi
    done
    restore_service_state
}

finish() {
    local result=$?
    trap - EXIT
    set +e
    if (( result != 0 && MUTATION_STARTED == 1 && COMMITTED == 0 )); then
        echo "Removal failed; restoring the previous managed files." >&2
        rollback
    fi
    rm -rf -- "${BACKUP_DIR}"
    exit "${result}"
}
trap finish EXIT

for index in "${!removal_targets[@]}"; do
    backup_target "${removal_targets[index]}" "removed-${index}"
done

if (( STAGING_MODE == 0 )); then
    if systemctl is-active --quiet dmxwb.service; then
        SERVICE_WAS_ACTIVE=1
    fi
    if systemctl is-enabled --quiet dmxwb.service; then
        SERVICE_WAS_ENABLED=1
    fi
fi

MUTATION_STARTED=1
if (( STAGING_MODE == 0 )); then
    if (( SERVICE_WAS_ACTIVE == 1 )); then
        systemctl stop dmxwb.service
    fi
    if (( SERVICE_WAS_ENABLED == 1 )); then
        systemctl disable dmxwb.service
    fi
fi

for target in "${managed_targets[@]}"; do
    rm -f -- "${target}"
done

if (( STAGING_MODE == 1 )) &&
   [[ "${DMXWB_UNINSTALL_TEST_FAIL_AFTER:-}" == "managed" ]]; then
    echo "Injected isolated-root removal failure after managed files." >&2
    exit 1
fi

if (( PURGE == 1 )); then
    rm -f -- "${TARGET_CONFIG}" "${TARGET_STATE}"
fi

if (( STAGING_MODE == 0 )); then
    systemctl daemon-reload
fi

COMMITTED=1
rmdir -- "${WEB_DIR}" >/dev/null 2>&1 || true
if (( PURGE == 1 )); then
    rmdir -- "${CONFIG_DIR}" >/dev/null 2>&1 || true
    rmdir -- "${STATE_DIR}" >/dev/null 2>&1 || true
fi

if (( PURGE == 1 )); then
    echo "DMXWB application and user data purge completed."
else
    echo "DMXWB application removal completed; config and state were preserved."
    echo "config_path=${TARGET_CONFIG}"
    echo "state_path=${TARGET_STATE}"
fi
