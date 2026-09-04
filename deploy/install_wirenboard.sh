#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Usage:
  sudo bash install.sh

Run install.sh from the extracted DMXWB offline bundle root. The same command is
used for the first installation and for an in-place update. Existing config.json
and state.json are preserved.
USAGE
}

if [[ $# -gt 1 ]] || [[ $# -eq 1 && "$1" != "--help" && "$1" != "-h" ]]; then
    usage >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="${SCRIPT_DIR}/MANIFEST.txt"
CHECKSUMS="${SCRIPT_DIR}/SHA256SUMS"
PAYLOAD="${SCRIPT_DIR}/payload"
DEST_ROOT="${DESTDIR:-}"
STAGING_MODE=0

for command_name in awk cat cp dirname grep install mkdir mktemp mv realpath rm rmdir sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing required command: ${command_name}" >&2
        exit 1
    fi
done

if [[ -n "${DEST_ROOT}" ]]; then
    if [[ "${DEST_ROOT}" != /* || "${DEST_ROOT}" == "/" ]]; then
        echo "DESTDIR must be an absolute non-root staging directory." >&2
        exit 2
    fi
    if [[ -L "${DEST_ROOT}" ]]; then
        echo "DESTDIR must not be a symbolic link." >&2
        exit 2
    fi
    mkdir -p -- "${DEST_ROOT}"
    DEST_ROOT="$(realpath -m -- "${DEST_ROOT}")"
    if [[ "${DEST_ROOT}" == "/" ]]; then
        echo "DESTDIR must not resolve to the system root." >&2
        exit 2
    fi
    STAGING_MODE=1
elif [[ ${EUID} -ne 0 ]]; then
    echo "Production installation must be run as root." >&2
    exit 1
fi

required_bundle_files=(
    "install.sh"
    "MANIFEST.txt"
    "payload/usr/local/bin/dmxwb"
    "payload/etc/dmxwb/config.example.json"
    "payload/etc/systemd/system/dmxwb.service"
    "payload/var/www/dmxwb/index.html"
    "payload/var/www/dmxwb/app.js"
    "payload/var/www/dmxwb/model.js"
    "payload/var/www/dmxwb/mqtt-client.js"
    "payload/var/www/dmxwb/styles.css"
)

if [[ ! -f "${MANIFEST}" || ! -f "${CHECKSUMS}" ]]; then
    echo "Bundle metadata is missing." >&2
    exit 1
fi
for relative_path in "${required_bundle_files[@]}"; do
    if [[ ! -f "${SCRIPT_DIR}/${relative_path}" ]]; then
        echo "Required bundle file is missing: ${relative_path}" >&2
        exit 1
    fi
    checksum_count="$(awk -v path="${relative_path}" '$2 == path { count += 1 } END { print count + 0 }' "${CHECKSUMS}")"
    if [[ "${checksum_count}" != "1" ]]; then
        echo "SHA256SUMS must cover exactly once: ${relative_path}" >&2
        exit 1
    fi
done
(
    cd "${SCRIPT_DIR}"
    sha256sum --check --strict SHA256SUMS
) >/dev/null

manifest_value() {
    local key="$1"
    awk -F= -v key="${key}" '
        $1 == key { count += 1; value = substr($0, length(key) + 2) }
        END {
            if (count != 1 || value == "") exit 1
            print value
        }
    ' "${MANIFEST}"
}

if [[ "$(manifest_value bundle_format)" != "1" ]] ||
   [[ "$(manifest_value product)" != "DMXWB" ]] ||
   [[ "$(manifest_value target)" != "wb8-bullseye-arm64" ]] ||
   [[ "$(manifest_value architecture)" != "AArch64" ]]; then
    echo "Bundle manifest is not compatible with this installer." >&2
    exit 1
fi

PAYLOAD_BINARY="${PAYLOAD}/usr/local/bin/dmxwb"
PAYLOAD_BINARY_SHA="$(sha256sum "${PAYLOAD_BINARY}" | awk '{print $1}')"
if [[ "$(manifest_value artifact_sha256)" != "${PAYLOAD_BINARY_SHA}" ]]; then
    echo "Manifest artifact SHA256 does not match the payload binary." >&2
    exit 1
fi

if (( STAGING_MODE == 0 )); then
    for command_name in ldd systemctl uname; do
        if ! command -v "${command_name}" >/dev/null 2>&1; then
            echo "Missing required WB8 command: ${command_name}" >&2
            exit 1
        fi
    done
    case "$(uname -m)" in
        aarch64|arm64) ;;
        *)
            echo "Unsupported target architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac
    if [[ ! -f /etc/os-release ]] ||
       ! grep -Eq '^VERSION_ID="?11"?$' /etc/os-release; then
        echo "This bundle requires the tested Debian 11 Bullseye WB8 runtime." >&2
        exit 1
    fi
    LDD_OUTPUT="$(ldd "${PAYLOAD_BINARY}" 2>&1)" || {
        echo "Cannot inspect payload runtime dependencies." >&2
        printf '%s\n' "${LDD_OUTPUT}" >&2
        exit 1
    }
    if grep -q 'not found' <<<"${LDD_OUTPUT}"; then
        echo "A required runtime dependency is missing." >&2
        printf '%s\n' "${LDD_OUTPUT}" >&2
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
WEB_PARENT="$(target_path /var/www)"
WEB_DIR="$(target_path /var/www/dmxwb)"
TARGET_BINARY="${BIN_DIR}/dmxwb"
TARGET_CONFIG="${CONFIG_DIR}/config.json"
TARGET_STATE="${STATE_DIR}/state.json"
TARGET_UNIT="${UNIT_DIR}/dmxwb.service"

for directory in "${BIN_DIR}" "${CONFIG_DIR}" "${STATE_DIR}" "${UNIT_DIR}" "${WEB_PARENT}" "${WEB_DIR}"; do
    if [[ -L "${directory}" ]]; then
        echo "Managed directory must not be a symbolic link: ${directory}" >&2
        exit 1
    fi
    if [[ -e "${directory}" && ! -d "${directory}" ]]; then
        echo "Managed directory path is not a directory: ${directory}" >&2
        exit 1
    fi
done

managed_targets=(
    "${TARGET_BINARY}"
    "${TARGET_UNIT}"
    "${WEB_DIR}/index.html"
    "${WEB_DIR}/app.js"
    "${WEB_DIR}/model.js"
    "${WEB_DIR}/mqtt-client.js"
    "${WEB_DIR}/styles.css"
)
managed_sources=(
    "${PAYLOAD}/usr/local/bin/dmxwb"
    "${PAYLOAD}/etc/systemd/system/dmxwb.service"
    "${PAYLOAD}/var/www/dmxwb/index.html"
    "${PAYLOAD}/var/www/dmxwb/app.js"
    "${PAYLOAD}/var/www/dmxwb/model.js"
    "${PAYLOAD}/var/www/dmxwb/mqtt-client.js"
    "${PAYLOAD}/var/www/dmxwb/styles.css"
)
managed_modes=(0755 0644 0644 0644 0644 0644 0644)

for target in "${managed_targets[@]}" "${TARGET_CONFIG}" "${TARGET_STATE}"; do
    if [[ -L "${target}" ]]; then
        echo "Managed file must not be a symbolic link: ${target}" >&2
        exit 1
    fi
    if [[ -e "${target}" && ! -f "${target}" ]]; then
        echo "Managed file path is not a regular file: ${target}" >&2
        exit 1
    fi
done

INSTALL_MODE="fresh"
if [[ -f "${TARGET_BINARY}" || -f "${TARGET_UNIT}" ]]; then
    INSTALL_MODE="update"
fi

BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dmxwb-install.XXXXXX")"
MUTATION_STARTED=0
COMMITTED=0
SERVICE_WAS_ACTIVE=0
SERVICE_WAS_ENABLED=0
declare -a backup_targets=()
declare -a backup_files=()
declare -a backup_present=()
declare -a temporary_files=()
declare -a created_directories=()

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
        systemctl restart dmxwb.service >/dev/null 2>&1 || true
    else
        systemctl stop dmxwb.service >/dev/null 2>&1 || true
    fi
}

rollback() {
    local index
    for ((index=${#backup_targets[@]} - 1; index>=0; --index)); do
        if [[ "${backup_present[index]}" == "1" ]]; then
            cp -a -- "${backup_files[index]}" "${backup_targets[index]}"
        else
            rm -f -- "${backup_targets[index]}"
        fi
    done
    restore_service_state
    for ((index=${#created_directories[@]} - 1; index>=0; --index)); do
        rmdir -- "${created_directories[index]}" >/dev/null 2>&1 || true
    done
}

finish() {
    local result=$?
    trap - EXIT
    set +e
    if (( result != 0 && MUTATION_STARTED == 1 && COMMITTED == 0 )); then
        echo "Installation failed; restoring the previous managed files." >&2
        rollback
    fi
    for temporary in "${temporary_files[@]}"; do
        rm -f -- "${temporary}"
    done
    rm -rf -- "${BACKUP_DIR}"
    exit "${result}"
}
trap finish EXIT

ensure_directory() {
    local directory="$1"
    if [[ ! -d "${directory}" ]]; then
        install -d -m 0755 -- "${directory}"
        created_directories+=("${directory}")
    fi
}

for index in "${!managed_targets[@]}"; do
    backup_target "${managed_targets[index]}" "managed-${index}"
done
if [[ ! -f "${TARGET_CONFIG}" ]]; then
    backup_target "${TARGET_CONFIG}" "new-config"
fi

MUTATION_STARTED=1
for directory in "${BIN_DIR}" "${CONFIG_DIR}" "${STATE_DIR}" "${UNIT_DIR}" "${WEB_PARENT}" "${WEB_DIR}"; do
    ensure_directory "${directory}"
done

if (( STAGING_MODE == 0 )); then
    if systemctl is-active --quiet dmxwb.service; then
        SERVICE_WAS_ACTIVE=1
    fi
    if systemctl is-enabled --quiet dmxwb.service; then
        SERVICE_WAS_ENABLED=1
    fi
    if [[ -f "${TARGET_UNIT}" ]]; then
        systemctl stop dmxwb.service
    fi
fi

install_atomic() {
    local source="$1"
    local target="$2"
    local mode="$3"
    local target_directory
    local temporary
    target_directory="$(dirname -- "${target}")"
    temporary="$(mktemp "${target_directory}/.dmxwb-install.XXXXXX")"
    temporary_files+=("${temporary}")
    install -m "${mode}" -- "${source}" "${temporary}"
    mv -f -- "${temporary}" "${target}"
}

maybe_fail_for_regression() {
    local stage="$1"
    if (( STAGING_MODE == 1 )) &&
       [[ "${DMXWB_INSTALL_TEST_FAIL_AFTER:-}" == "${stage}" ]]; then
        echo "Injected isolated-root installation failure after ${stage}." >&2
        return 1
    fi
}

install_atomic "${managed_sources[0]}" "${managed_targets[0]}" "${managed_modes[0]}"
maybe_fail_for_regression binary
install_atomic "${managed_sources[1]}" "${managed_targets[1]}" "${managed_modes[1]}"
maybe_fail_for_regression unit
for index in 2 3 4 5 6; do
    install_atomic "${managed_sources[index]}" "${managed_targets[index]}" "${managed_modes[index]}"
done
maybe_fail_for_regression web

if [[ ! -f "${TARGET_CONFIG}" ]]; then
    install_atomic \
        "${PAYLOAD}/etc/dmxwb/config.example.json" \
        "${TARGET_CONFIG}" \
        0644
fi
maybe_fail_for_regression config

if (( STAGING_MODE == 0 )); then
    systemctl daemon-reload
    systemctl enable dmxwb.service
    systemctl restart dmxwb.service
fi

COMMITTED=1
echo "DMXWB ${INSTALL_MODE} installation completed."
echo "config_path=${TARGET_CONFIG}"
echo "state_path=${TARGET_STATE}"
if (( STAGING_MODE == 0 )); then
    echo "service=enabled_and_started"
else
    echo "service=not_started_in_DESTDIR_staging_mode"
fi
