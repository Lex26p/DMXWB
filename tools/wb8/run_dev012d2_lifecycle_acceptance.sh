#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

TARGET="${1:-root@10.200.200.1}"
START_ADDRESS="${2:-1}"
TARGET_HOST="${TARGET#*@}"
DMX_PORT="/dev/ttyRS485-1"
ARTNET_PORT_ADDRESS=0

if [[ $# -gt 2 ]]; then
    echo "Usage: bash tools/wb8/run_dev012d2_lifecycle_acceptance.sh [SSH_TARGET] [FIXTURE_START_ADDRESS]" >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] ||
   (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi
if [[ ! "${TARGET_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "SSH_TARGET должен содержать IPv4, например root@10.200.200.1." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
ARCHIVE="${REPO_ROOT}/artifacts/offline/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64.tar.gz"
REPORT="${REPO_ROOT}/docs/DEV012D2_LIFECYCLE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev012d2-lifecycle"
REMOTE_ARCHIVE="${REMOTE_DIR}/dmxwb-offline.tar.gz"
REMOTE_BUNDLE="${REMOTE_DIR}/bundle/dmxwb-wb8-bullseye-arm64"
LOCAL_TMP="$(mktemp -d)"
CONTROL_DIR="${LOCAL_TMP}/ssh"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

SSH_OPTS=(
    -o ControlMaster=auto
    -o "ControlPath=${CONTROL_PATH}"
    -o ControlPersist=1200
    -o StrictHostKeyChecking=accept-new
)
SCP_OPTS=(
    -o ControlMaster=auto
    -o "ControlPath=${CONTROL_PATH}"
    -o ControlPersist=1200
    -o StrictHostKeyChecking=accept-new
)

for command_name in awk cat date grep mkdir mktemp rm scp sed seq sha256sum sleep ssh tar tee tr; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Не найдена локальная команда: ${command_name}" >&2
        exit 1
    fi
done
if [[ ! -f "${ARCHIVE}" ]]; then
    echo "Не найден принятый final bundle: ${ARCHIVE}" >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
ARCHIVE_SHA256="$(sha256sum "${ARCHIVE}" | awk '{print $1}')"
ARTIFACT_SHA256="$(
    tar -xOf "${ARCHIVE}" \
        dmxwb-wb8-bullseye-arm64/MANIFEST.txt |
        sed -n 's/^artifact_sha256=//p'
)"
SOURCE_ID="$(
    tar -xOf "${ARCHIVE}" \
        dmxwb-wb8-bullseye-arm64/MANIFEST.txt |
        sed -n 's/^source_id=//p'
)"
if [[ -z "${ARTIFACT_SHA256}" || -z "${SOURCE_ID}" ]]; then
    echo "Final bundle manifest не содержит обязательную identity." >&2
    exit 1
fi

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${DMX_PORT}"},"artnet":{"universe":${ARTNET_PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV012D2 Lifecycle Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

: > "${REPORT}"
record() {
    printf '%s\n' "$1" | tee -a "${REPORT}"
}

ask_yes_no() {
    local prompt="$1"
    local answer
    while true; do
        read -r -p "${prompt} (y/n): " answer
        case "${answer}" in
            y|Y) return 0 ;;
            n|N) return 1 ;;
            *) echo "Введите y или n." ;;
        esac
    done
}

SSH_OPENED=0
REMOTE_BACKUP_READY=0
RETAINED_CAPTURED=0
CLEANED=0
PREV_SERVICE_ACTIVE=0
PREV_SERVICE_ENABLED=0

remote() {
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"
}

backup_remote_path() {
    local path="$1"
    local key="$2"
    remote "
if test -e '${path}' || test -L '${path}'; then
    cp -a -- '${path}' '${REMOTE_DIR}/backup/${key}'
    printf '1\n' > '${REMOTE_DIR}/backup/${key}.present'
else
    printf '0\n' > '${REMOTE_DIR}/backup/${key}.present'
fi
"
}

restore_remote_path() {
    local path="$1"
    local key="$2"
    remote "
rm -rf -- '${path}'
if test \"\$(cat '${REMOTE_DIR}/backup/${key}.present' 2>/dev/null)\" = 1; then
    cp -a -- '${REMOTE_DIR}/backup/${key}' '${path}'
fi
"
}

RETAINED_TOPICS=(
    "/dmxwb/config"
    "/dmxwb/state"
    "/dmxwb/status"
    "/devices/dmxwb/meta"
    "/devices/dmxwb/controls/status/meta"
    "/devices/dmxwb/controls/source/meta"
    "/devices/dmxwb/controls/status"
    "/devices/dmxwb/controls/source"
    "/devices/dmxwb_fixture_1/meta"
    "/devices/dmxwb_fixture_1/controls/name/meta"
    "/devices/dmxwb_fixture_1/controls/name"
    "/devices/dmxwb_fixture_1/controls/power/meta"
    "/devices/dmxwb_fixture_1/controls/power"
    "/devices/dmxwb_fixture_1/controls/red/meta"
    "/devices/dmxwb_fixture_1/controls/red"
    "/devices/dmxwb_fixture_1/controls/green/meta"
    "/devices/dmxwb_fixture_1/controls/green"
    "/devices/dmxwb_fixture_1/controls/blue/meta"
    "/devices/dmxwb_fixture_1/controls/blue"
    "/devices/dmxwb_fixture_1/controls/color/meta"
    "/devices/dmxwb_fixture_1/controls/color"
    "/devices/dmxwb_fixture_1/controls/brightness/meta"
    "/devices/dmxwb_fixture_1/controls/brightness"
    "/devices/dmxwb_fixture_1/controls/temperature/meta"
    "/devices/dmxwb_fixture_1/controls/temperature"
    "/devices/dmxwb_fixture_1/controls/reset/meta"
)

capture_retained() {
    remote "mkdir -p '${REMOTE_DIR}/retained-backup'"
    local index=0
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "
set +e
if timeout 0.6 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '${topic}' > '${REMOTE_DIR}/retained-backup/${index}.payload' 2>/dev/null; then
    printf '1\n' > '${REMOTE_DIR}/retained-backup/${index}.present'
else
    : > '${REMOTE_DIR}/retained-backup/${index}.payload'
    printf '0\n' > '${REMOTE_DIR}/retained-backup/${index}.present'
fi
"
        index=$((index + 1))
    done
    RETAINED_CAPTURED=1
}

clear_retained() {
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "mosquitto_pub -h 127.0.0.1 -p 1883 -r -n -t '${topic}'" >/dev/null
    done
}

restore_retained() {
    if (( RETAINED_CAPTURED == 0 )); then
        return 0
    fi
    if ! remote "systemctl is-active --quiet mosquitto"; then
        remote "systemctl start mosquitto" >/dev/null 2>&1 || true
        sleep 1
    fi
    local index=0
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "
if test \"\$(cat '${REMOTE_DIR}/retained-backup/${index}.present' 2>/dev/null)\" = 1; then
    mosquitto_pub -h 127.0.0.1 -p 1883 -r -t '${topic}' -f '${REMOTE_DIR}/retained-backup/${index}.payload'
else
    mosquitto_pub -h 127.0.0.1 -p 1883 -r -n -t '${topic}'
fi
" >/dev/null
        index=$((index + 1))
    done
    RETAINED_CAPTURED=0
}

restore_remote_environment() {
    if (( REMOTE_BACKUP_READY == 0 )); then
        return 0
    fi
    remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
    restore_retained >/dev/null 2>&1 || true
    restore_remote_path "/usr/local/bin/dmxwb" binary
    restore_remote_path "/etc/systemd/system/dmxwb.service" unit
    restore_remote_path "/etc/dmxwb" config_dir
    restore_remote_path "/var/lib/dmxwb" state_dir
    restore_remote_path "/var/www/dmxwb" web_dir
    remote "systemctl daemon-reload"
    if (( PREV_SERVICE_ENABLED == 1 )); then
        remote "systemctl enable dmxwb.service >/dev/null 2>&1 || true"
    else
        remote "systemctl disable dmxwb.service >/dev/null 2>&1 || true"
    fi
    if (( PREV_SERVICE_ACTIVE == 1 )); then
        remote "systemctl start dmxwb.service >/dev/null 2>&1 || true"
    else
        remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
    fi
    REMOTE_BACKUP_READY=0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( CLEANED == 0 )); then
        if (( SSH_OPENED == 1 )); then
            remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
            restore_remote_environment >/dev/null 2>&1 || true
            remote "rm -rf -- '${REMOTE_DIR}'" >/dev/null 2>&1 || true
            ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
        fi
        rm -rf -- "${LOCAL_TMP}" >/dev/null 2>&1 || true
        CLEANED=1
    fi
    exit "${status}"
}
trap cleanup EXIT INT TERM

wait_topic() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value
    for _ in $(seq 1 "${attempts}"); do
        value="$(remote "timeout 1 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '${topic}' 2>/dev/null || true" | tr -d '\r\n')"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался MQTT ${topic}='${expected}', получено '${value:-<empty>}'" >&2
    return 1
}

service_pid() {
    remote "systemctl show -p MainPID --value dmxwb.service" | tr -d '\r\n'
}

wait_service_active() {
    local pid
    for _ in $(seq 1 80); do
        if remote "systemctl is-active --quiet dmxwb.service"; then
            pid="$(service_pid)"
            if [[ "${pid}" =~ ^[1-9][0-9]*$ ]]; then
                printf '%s' "${pid}"
                return 0
            fi
        fi
        sleep 0.25
    done
    echo "dmxwb.service не стал active." >&2
    remote "systemctl status dmxwb.service --no-pager -l || true" >&2
    return 1
}

serial_port_is_released() {
    remote "
for fd in /proc/[0-9]*/fd/*; do
    target=\$(readlink \"\${fd}\" 2>/dev/null || true)
    if test \"\${target}\" = '${DMX_PORT}'; then
        exit 1
    fi
done
exit 0
"
}

assert_user_data_matches_baseline() {
    remote "cmp -s '${REMOTE_DIR}/baseline/config.json' /etc/dmxwb/config.json; cmp -s '${REMOTE_DIR}/baseline/state.json' /var/lib/dmxwb/state.json"
}

assert_unrelated_files_present() {
    remote "test \"\$(cat /etc/dmxwb/dev012d2-unrelated.txt)\" = config-unrelated; test \"\$(cat /var/lib/dmxwb/dev012d2-unrelated.txt)\" = state-unrelated; test \"\$(cat /var/www/dmxwb/dev012d2-unrelated.txt)\" = web-unrelated"
}

assert_application_files_absent() {
    remote "
for path in /usr/local/bin/dmxwb /etc/systemd/system/dmxwb.service /var/www/dmxwb/index.html /var/www/dmxwb/app.js /var/www/dmxwb/model.js /var/www/dmxwb/mqtt-client.js /var/www/dmxwb/styles.css; do
    if test -e \"\${path}\" || test -L \"\${path}\"; then
        echo \"managed file remains: \${path}\" >&2
        exit 1
    fi
done
"
}

record "=== DMXWB DEV-012D2 UPDATE/REMOVE/REINSTALL/PURGE ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "target: ${TARGET}"
record "bundle_sha256: ${ARCHIVE_SHA256}"
record "source_id: ${SOURCE_ID}"
record "artifact_sha256: ${ARTIFACT_SHA256}"
record "dmx_port: ${DMX_PORT}"
record "fixture_start_address: ${START_ADDRESS}"

echo "DMXWB — DEV-012D2 lifecycle acceptance"
echo "WB8:           ${TARGET}"
echo "DMX port:      ${DMX_PORT}"
echo "Fixture Start: ${START_ADDRESS}"
if ! ask_yes_no "RGBW-светильник подключён к ${DMX_PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    exit 2
fi
if ! ask_yes_no "Внешний Интернет для WB8 по-прежнему недоступен, но локальная LAN/SSH работает?"; then
    exit 2
fi

echo
echo "=== WB8 preflight and backup ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1
remote "for c in awk bash cat cmp cp find grep install ldd mkdir mosquitto_pub mosquitto_sub readlink rm sha256sum stat systemctl tar timeout tr uname; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto"
remote "test -e '${DMX_PORT}'"
remote "test \"\$(uname -m)\" = aarch64"
remote "grep -Eq '^VERSION_ID=\"?11\"?$' /etc/os-release"

RUNNING_DMXWB="$(remote '
for exe in /proc/[0-9]*/exe; do
    target=$(readlink "${exe}" 2>/dev/null || true)
    case "${target##*/}" in
        dmxwb|dmxwb-dev010-source-acceptance|dmxwb-mqtt-acceptance)
            pid=${exe#/proc/}; pid=${pid%/exe}
            printf "%s %s\n" "${pid}" "${target}"
            ;;
    esac
done
' | tr -d '\r')"
if [[ -n "${RUNNING_DMXWB}" ]]; then
    echo "На WB8 уже запущен DMXWB. Остановите его перед lifecycle acceptance:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
if ! serial_port_is_released; then
    echo "Порт ${DMX_PORT} уже используется другим процессом." >&2
    exit 1
fi
record "lifecycle_preflight: PASS"

remote "rm -rf -- '${REMOTE_DIR}'; mkdir -p '${REMOTE_DIR}/backup' '${REMOTE_DIR}/baseline'"
PREV_SERVICE_ACTIVE="$(remote "systemctl is-active --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
PREV_SERVICE_ENABLED="$(remote "systemctl is-enabled --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
backup_remote_path "/usr/local/bin/dmxwb" binary
backup_remote_path "/etc/systemd/system/dmxwb.service" unit
backup_remote_path "/etc/dmxwb" config_dir
backup_remote_path "/var/lib/dmxwb" state_dir
backup_remote_path "/var/www/dmxwb" web_dir
REMOTE_BACKUP_READY=1
capture_retained
record "original_environment_captured: PASS"

remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true; systemctl disable dmxwb.service >/dev/null 2>&1 || true; rm -f -- /usr/local/bin/dmxwb /etc/systemd/system/dmxwb.service; rm -rf -- /etc/dmxwb /var/lib/dmxwb /var/www/dmxwb; systemctl daemon-reload"
clear_retained
scp "${SCP_OPTS[@]}" "${ARCHIVE}" "${TARGET}:${REMOTE_ARCHIVE}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/config.json" "${TARGET}:${REMOTE_DIR}/acceptance-config.json" >/dev/null
remote "test \"\$(sha256sum '${REMOTE_ARCHIVE}' | awk '{print \$1}')\" = '${ARCHIVE_SHA256}'; mkdir -p '${REMOTE_DIR}/bundle'; tar -C '${REMOTE_DIR}/bundle' -xzf '${REMOTE_ARCHIVE}'"

echo
echo "=== Prepare stable acceptance data ==="
remote "cd '${REMOTE_BUNDLE}' && bash ./install.sh" | tee -a "${REPORT}"
wait_service_active >/dev/null
remote "systemctl stop dmxwb.service; install -m 0644 '${REMOTE_DIR}/acceptance-config.json' /etc/dmxwb/config.json; rm -f -- /var/lib/dmxwb/state.json"
clear_retained
remote "systemctl start dmxwb.service"
wait_service_active >/dev/null
wait_topic "/devices/dmxwb/controls/status" running 60
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 1"
wait_topic "/devices/dmxwb_fixture_1/controls/red" 255 60
wait_topic "/devices/dmxwb_fixture_1/controls/power" 1 60
remote "systemctl stop dmxwb.service; test -s /var/lib/dmxwb/state.json; cp -a /etc/dmxwb/config.json '${REMOTE_DIR}/baseline/config.json'; cp -a /var/lib/dmxwb/state.json '${REMOTE_DIR}/baseline/state.json'; printf 'config-unrelated\n' > /etc/dmxwb/dev012d2-unrelated.txt; printf 'state-unrelated\n' > /var/lib/dmxwb/dev012d2-unrelated.txt; printf 'web-unrelated\n' > /var/www/dmxwb/dev012d2-unrelated.txt"
assert_user_data_matches_baseline
assert_unrelated_files_present
record "stable_acceptance_config_state_and_unrelated_files: PASS"

echo
echo "=== In-place update ==="
remote "systemctl start dmxwb.service"
PID_BEFORE_UPDATE="$(wait_service_active)"
remote "printf '\n/* DEV-012D2 stale managed file */\n' >> /var/www/dmxwb/styles.css; ! cmp -s '${REMOTE_BUNDLE}/payload/var/www/dmxwb/styles.css' /var/www/dmxwb/styles.css"
UPDATE_OUTPUT="$(remote "cd '${REMOTE_BUNDLE}' && bash ./install.sh")"
printf '%s\n' "${UPDATE_OUTPUT}" | tee -a "${REPORT}"
if [[ "${UPDATE_OUTPUT}" != *"update installation completed"* ]]; then
    echo "Installer не сообщил update mode." >&2
    exit 1
fi
PID_AFTER_UPDATE="$(wait_service_active)"
if [[ "${PID_AFTER_UPDATE}" == "${PID_BEFORE_UPDATE}" ]]; then
    echo "Update не перезапустил службу." >&2
    exit 1
fi
remote "cmp -s '${REMOTE_BUNDLE}/payload/var/www/dmxwb/styles.css' /var/www/dmxwb/styles.css"
assert_user_data_matches_baseline
assert_unrelated_files_present
record "active_update_replaced_managed_file_preserved_user_data: PASS (${PID_BEFORE_UPDATE} -> ${PID_AFTER_UPDATE})"

echo
echo "=== Normal removal and idempotency ==="
REMOVE_OUTPUT="$(remote "cd '${REMOTE_BUNDLE}' && bash ./uninstall.sh")"
printf '%s\n' "${REMOVE_OUTPUT}" | tee -a "${REPORT}"
if [[ "${REMOVE_OUTPUT}" != *"config and state were preserved"* ]]; then
    echo "Uninstaller не подтвердил сохранение config/state." >&2
    exit 1
fi
assert_application_files_absent
assert_user_data_matches_baseline
assert_unrelated_files_present
remote "! systemctl is-active --quiet dmxwb.service; ! systemctl is-enabled --quiet dmxwb.service >/dev/null 2>&1"
remote "cd '${REMOTE_BUNDLE}' && bash ./uninstall.sh" >/dev/null
assert_application_files_absent
assert_user_data_matches_baseline
assert_unrelated_files_present
record "normal_removal_idempotent_preserved_user_data: PASS"

echo
echo "=== Reinstall with preserved data ==="
REINSTALL_OUTPUT="$(remote "cd '${REMOTE_BUNDLE}' && bash ./install.sh")"
printf '%s\n' "${REINSTALL_OUTPUT}" | tee -a "${REPORT}"
PID_REINSTALL="$(wait_service_active)"
remote "systemctl is-enabled --quiet dmxwb.service"
assert_user_data_matches_baseline
assert_unrelated_files_present
wait_topic "/devices/dmxwb_fixture_1/controls/red" 255 60
wait_topic "/devices/dmxwb_fixture_1/controls/power" 1 60
if ! ask_yes_no "После reinstall сохранённое состояние восстановило стабильный КРАСНЫЙ свет?"; then
    exit 1
fi
record "reinstall_restored_preserved_state_user: PASS (pid=${PID_REINSTALL})"

echo
echo "=== Explicit purge of acceptance data ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" 0 60
if ! ask_yes_no "Перед purge физический светильник полностью выключен?"; then
    exit 1
fi
PURGE_OUTPUT="$(remote "cd '${REMOTE_BUNDLE}' && bash ./uninstall.sh --purge")"
printf '%s\n' "${PURGE_OUTPUT}" | tee -a "${REPORT}"
if [[ "${PURGE_OUTPUT}" != *"user data purge completed"* ]]; then
    echo "Uninstaller не подтвердил explicit purge." >&2
    exit 1
fi
assert_application_files_absent
remote "test ! -e /etc/dmxwb/config.json; test ! -L /etc/dmxwb/config.json; test ! -e /var/lib/dmxwb/state.json; test ! -L /var/lib/dmxwb/state.json"
assert_unrelated_files_present
remote "cd '${REMOTE_BUNDLE}' && bash ./uninstall.sh --purge" >/dev/null
assert_application_files_absent
assert_unrelated_files_present
if ! serial_port_is_released; then
    echo "После purge порт ${DMX_PORT} остался открыт." >&2
    exit 1
fi
record "explicit_purge_idempotent_removed_only_known_data: PASS"

restore_remote_environment
record "original_environment_restored: PASS"
record "dev012d2_result: PASS"
record "=== DMXWB DEV-012D2 LIFECYCLE ACCEPTANCE PASS ==="

echo
echo "=== DMXWB DEV-012D2 LIFECYCLE ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf -- '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf -- "${LOCAL_TMP}"
CLEANED=1
trap - EXIT INT TERM
