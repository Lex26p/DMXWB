#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

TARGET="${1:-root@10.200.200.1}"
START_ADDRESS="${2:-1}"
TARGET_HOST="${TARGET#*@}"
DMX_PORT="/dev/ttyRS485-1"
ARTNET_PORT_ADDRESS=0
ACCEPTANCE_VARIANT="${DMXWB_REBOOT_ACCEPTANCE_VARIANT:-dev012d3}"

case "${ACCEPTANCE_VARIANT}" in
    dev012d3)
        ACCEPTANCE_ID="DEV-012D3"
        ACCEPTANCE_TITLE="REBOOT AND RUNTIME RECOVERY"
        REPORT_NAME="DEV012D3_REBOOT_RUNTIME_REPORT.txt"
        REMOTE_SUFFIX="dev012d3-acceptance"
        FIXTURE_NAME="DEV012D3 Reboot Fixture"
        EXPECTED_SOURCE_ID=""
        EXPECTED_ARCHIVE_SHA256=""
        RUN_EXTENDED_RECOVERY=1
        NATURAL_ENTITY_WORKFLOW=0
        ACTIVE_FIXTURE_ID=1
        ;;
    dev013d)
        ACCEPTANCE_ID="DEV-013D"
        ACCEPTANCE_TITLE="FINAL OFFLINE INSTALL/REBOOT CLOSEOUT"
        REPORT_NAME="DEV013D_FINAL_OFFLINE_REBOOT_REPORT.txt"
        REMOTE_SUFFIX="dev013d-final"
        FIXTURE_NAME="DEV013D Final Fixture"
        EXPECTED_SOURCE_ID="dev013a-final"
        EXPECTED_ARCHIVE_SHA256="f94fe7a4504310c524197854528d9aca2c43814d78657390f702f40a26a688bd"
        RUN_EXTENDED_RECOVERY=0
        NATURAL_ENTITY_WORKFLOW=0
        ACTIVE_FIXTURE_ID=1
        ;;
    dev014c)
        ACCEPTANCE_ID="DEV-014C"
        ACCEPTANCE_TITLE="CLEAN ENTITY WORKFLOW/REBOOT FINAL"
        REPORT_NAME="DEV014C_CLEAN_STATE_REBOOT_REPORT.txt"
        REMOTE_SUFFIX="dev014c-final"
        FIXTURE_NAME="Светильник 2"
        EXPECTED_SOURCE_ID="dev014c-final"
        EXPECTED_ARCHIVE_SHA256="42e440034dcacdfdb1cfff5ed3fb54bc2cc367b5a70200aaaba9a5924c09ef3d"
        RUN_EXTENDED_RECOVERY=0
        NATURAL_ENTITY_WORKFLOW=1
        ACTIVE_FIXTURE_ID=2
        ;;
    *)
        echo "Неизвестный reboot acceptance variant: ${ACCEPTANCE_VARIANT}" >&2
        exit 2
        ;;
esac

if [[ $# -gt 2 ]]; then
    echo "Usage: bash tools/wb8/run_dev012d3_reboot_runtime_acceptance.sh [SSH_TARGET] [FIXTURE_START_ADDRESS]" >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] ||
   (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi
if (( NATURAL_ENTITY_WORKFLOW == 1 && START_ADDRESS > 293 )); then
    echo "Для DEV-014C двум RGBW-светильникам нужен Start Address 1..293." >&2
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
REPORT="${REPO_ROOT}/docs/${REPORT_NAME}"
REMOTE_DIR="/var/tmp/dmxwb-${REMOTE_SUFFIX}"
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

for command_name in awk bash cat date grep mkdir mktemp python3 rm scp sed seq sha256sum sleep ssh tar tee timeout tr; do
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
if [[ -n "${EXPECTED_SOURCE_ID}" && "${SOURCE_ID}" != "${EXPECTED_SOURCE_ID}" ]]; then
    echo "Ожидался source_id=${EXPECTED_SOURCE_ID}, получено '${SOURCE_ID}'." >&2
    exit 1
fi
if [[ -n "${EXPECTED_ARCHIVE_SHA256}" && "${ARCHIVE_SHA256}" != "${EXPECTED_ARCHIVE_SHA256}" ]]; then
    echo "Архив не совпадает с принятым в DEV-013A: ${ARCHIVE_SHA256}." >&2
    exit 1
fi

if (( NATURAL_ENTITY_WORKFLOW == 0 )); then
    cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${DMX_PORT}"},"artnet":{"universe":${ARTNET_PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"${FIXTURE_NAME}"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON
fi

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
PREFLIGHT_STOPPED_SERVICE=0

remote() {
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"
}

open_ssh_master() {
    mkdir -p "${CONTROL_DIR}"
    echo "Открываем SSH-соединение. Может потребоваться пароль."
    ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
    SSH_OPENED=1
}

close_ssh_master() {
    if (( SSH_OPENED == 1 )); then
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi
    SSH_OPENED=0
    rm -rf -- "${CONTROL_DIR}"
    mkdir -p "${CONTROL_DIR}"
}

ssh_port_open() {
    timeout 2 bash -c "exec 3<>/dev/tcp/${TARGET_HOST}/22" >/dev/null 2>&1
}

wait_ssh_port_down() {
    for _ in $(seq 1 80); do
        if ! ssh_port_open; then
            return 0
        fi
        sleep 0.5
    done
    echo "SSH-порт WB8 не закрылся после команды reboot." >&2
    return 1
}

wait_ssh_port_up() {
    local attempts="${1:-240}"
    for _ in $(seq 1 "${attempts}"); do
        if ssh_port_open; then
            return 0
        fi
        sleep 1
    done
    echo "SSH-порт WB8 не появился после reboot." >&2
    return 1
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
)
for fixture_id in 1 2; do
    RETAINED_TOPICS+=("/devices/dmxwb_fixture_${fixture_id}/meta")
    for control in name power red green blue color brightness temperature; do
        RETAINED_TOPICS+=(
            "/devices/dmxwb_fixture_${fixture_id}/controls/${control}/meta"
            "/devices/dmxwb_fixture_${fixture_id}/controls/${control}"
        )
    done
    RETAINED_TOPICS+=("/devices/dmxwb_fixture_${fixture_id}/controls/reset/meta")
done
RETAINED_TOPICS+=("/devices/dmxwb_group_1/meta")
for control in name power red green blue color brightness temperature; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_group_1/controls/${control}/meta"
        "/devices/dmxwb_group_1/controls/${control}"
    )
done
RETAINED_TOPICS+=("/devices/dmxwb_group_1/controls/reset/meta")
RETAINED_TOPICS+=(
    "/devices/dmxwb_scene_1/meta"
    "/devices/dmxwb_scene_1/controls/name/meta"
    "/devices/dmxwb_scene_1/controls/name"
    "/devices/dmxwb_scene_1/controls/apply/meta"
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
        if (( REMOTE_BACKUP_READY == 1 && SSH_OPENED == 0 )); then
            if wait_ssh_port_up 240; then
                rm -rf -- "${CONTROL_DIR}"
                mkdir -p "${CONTROL_DIR}"
                echo "Для аварийного восстановления среды может потребоваться пароль SSH."
                if ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"; then
                    SSH_OPENED=1
                fi
            fi
        fi
        if (( SSH_OPENED == 1 )); then
            if (( PREFLIGHT_STOPPED_SERVICE == 1 )); then
                remote "systemctl start dmxwb.service >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
                PREFLIGHT_STOPPED_SERVICE=0
            else
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_2/controls/power/on' -m 0 >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
            fi
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

mqtt_get() {
    local topic="$1"
    remote "timeout 1 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '${topic}' 2>/dev/null || true"
}

wait_topic() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value
    for _ in $(seq 1 "${attempts}"); do
        value="$(mqtt_get "${topic}" | tr -d '\r\n')"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался MQTT ${topic}='${expected}', получено '${value:-<empty>}'" >&2
    return 1
}

wait_topic_contains() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-60}"
    local value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(mqtt_get "${topic}" | tr -d '\r\n')"
        if [[ "${value}" == *"${expected}"* ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "В MQTT ${topic} не найдено '${expected}'. Получено '${value:-<empty>}'" >&2
    return 1
}

config_value() {
    local code="$1"
    mqtt_get "/dmxwb/config" | python3 -c "${code}" 2>/dev/null | tr -d '\r\n'
}

wait_config_value() {
    local expected="$1"
    local code="$2"
    local attempts="${3:-80}"
    local value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(config_value "${code}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Конфигурация: ожидалось '${expected}', получено '${value}'." >&2
    return 1
}

status_value() {
    local path="$1"
    mqtt_get "/dmxwb/status" | python3 -c '
import json, sys
path = sys.argv[1]
value = json.load(sys.stdin)
for part in path.split("."):
    value = value[part]
if value is True:
    print("true")
elif value is False:
    print("false")
elif value is None:
    print("null")
else:
    print(value)
' "${path}" | tr -d '\r\n'
}

wait_status_value() {
    local path="$1"
    local expected="$2"
    local attempts="${3:-80}"
    local value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(status_value "${path}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался /dmxwb/status ${path}='${expected}', получено '${value}'" >&2
    return 1
}

service_pid() {
    remote "systemctl show -p MainPID --value dmxwb.service" | tr -d '\r\n'
}

wait_service_active() {
    local pid
    for _ in $(seq 1 120); do
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

wait_service_pid_change() {
    local previous_pid="$1"
    local pid
    for _ in $(seq 1 120); do
        if remote "systemctl is-active --quiet dmxwb.service"; then
            pid="$(service_pid)"
            if [[ "${pid}" =~ ^[1-9][0-9]*$ && "${pid}" != "${previous_pid}" ]]; then
                printf '%s' "${pid}"
                return 0
            fi
        fi
        sleep 0.25
    done
    echo "dmxwb.service не восстановился с новым PID после ${previous_pid}." >&2
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

send_artdmx() {
    local red="$1"
    local green="$2"
    local blue="$3"
    local white="$4"
    python3 - "${TARGET_HOST}" "${ARTNET_PORT_ADDRESS}" "${START_ADDRESS}" \
        "${red}" "${green}" "${blue}" "${white}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
start_index = int(sys.argv[3]) - 1
values = bytes(int(value) for value in sys.argv[4:8])
dmx = bytearray(300)
dmx[start_index:start_index + 4] = values
sub_uni = port_address & 0xff
net = (port_address >> 8) & 0x7f
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    for sequence in range(1, 13):
        packet = (
            b"Art-Net\x00"
            + struct.pack("<H", 0x5000)
            + bytes((0, 14, sequence, 0, sub_uni, net))
            + struct.pack(">H", len(dmx))
            + dmx
        )
        sock.sendto(packet, (host, 6454))
        time.sleep(0.05)
finally:
    sock.close()
PY
}

validate_reboot_state() {
    if (( NATURAL_ENTITY_WORKFLOW == 1 )); then
        remote "python3 - '${START_ADDRESS}' <<'PY'
import json, sys
with open('/etc/dmxwb/config.json', encoding='utf-8') as stream:
    config = json.load(stream)
with open('/var/lib/dmxwb/state.json', encoding='utf-8') as stream:
    state = json.load(stream)
assert config['revision'] == 3
assert config['dmx']['port'] == '/dev/ttyRS485-1'
assert config['artnet']['universe'] == 0
assert config['fixtures']['count'] == 1
assert config['fixtures']['start_address'] == int(sys.argv[1])
assert [item['id'] for item in config['fixtures']['items']] == [2]
assert len(config['groups']) == 1
assert config['groups'][0]['id'] == 1
assert config['groups'][0]['members'] == [2]
assert len(config['scenes']) == 1
assert config['scenes'][0]['id'] == 1
assert [item['fixture_id'] for item in config['scenes'][0]['fixtures']] == [1, 2]
assert config['id_counters'] == {
    'next_fixture_id': 3,
    'next_group_id': 2,
    'next_scene_id': 2,
}
assert [item['id'] for item in state['fixtures']] == [2]
fixture = state['fixtures'][0]
assert fixture['requested_power'] is True
assert fixture['red'] == 255
assert fixture['green'] == 0
assert fixture['blue'] == 0
PY"
        return
    fi

    remote "python3 - '${START_ADDRESS}' <<'PY'
import json, sys
with open('/etc/dmxwb/config.json', encoding='utf-8') as stream:
    config = json.load(stream)
with open('/var/lib/dmxwb/state.json', encoding='utf-8') as stream:
    state = json.load(stream)
assert config['revision'] == 1
assert config['dmx']['port'] == '/dev/ttyRS485-1'
assert config['artnet']['universe'] == 0
assert config['fixtures']['count'] == 1
assert config['fixtures']['start_address'] == int(sys.argv[1])
assert config['fixtures']['items'][0]['id'] == 1
fixture = next(item for item in state['fixtures'] if item['id'] == 1)
assert fixture['requested_power'] is True
assert fixture['red'] == 255
assert fixture['green'] == 0
assert fixture['blue'] == 0
PY"
}

validate_operational_status() {
    mqtt_get "/dmxwb/status" | python3 -c '
import json, sys
status = json.load(sys.stdin)
assert status["application"] == "running"
assert status["mqtt"] == "connected"
diagnostics = status["diagnostics"]
assert diagnostics["selected_source"] == "mqtt"
assert diagnostics["dmx"]["state"] == "running"
assert diagnostics["dmx"]["port"] == "/dev/ttyRS485-1"
assert diagnostics["dmx"]["refresh_hz"] == 44
assert diagnostics["dmx"]["physical_slot_limit"] == 300
assert diagnostics["mqtt"]["connected"] is True
assert diagnostics["artnet"]["universe"] == 0
assert diagnostics["artnet"]["transport_open"] is True
' 
}

record "=== DMXWB ${ACCEPTANCE_ID} ${ACCEPTANCE_TITLE} ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "target: ${TARGET}"
record "bundle_sha256: ${ARCHIVE_SHA256}"
record "source_id: ${SOURCE_ID}"
record "artifact_sha256: ${ARTIFACT_SHA256}"
record "dmx_port: ${DMX_PORT}"
record "fixture_start_address: ${START_ADDRESS}"

echo "DMXWB — ${ACCEPTANCE_ID} ${ACCEPTANCE_TITLE} acceptance"
echo "ВНИМАНИЕ: сценарий один раз перезагрузит WB8. Пароль SSH потребуется повторно."
if ! ask_yes_no "RGBW-светильник подключён к ${DMX_PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    exit 2
fi
if ! ask_yes_no "Внешний Интернет для WB8 недоступен, локальная LAN стабильна и WB8 можно сейчас перезагрузить?"; then
    exit 2
fi

echo
echo "=== WB8 preflight, persistent backup and installation ==="
open_ssh_master
remote "for c in awk bash cat cmp cp grep install journalctl ldd mkdir mosquitto_pub mosquitto_sub python3 readlink rm sha256sum stat sync systemctl tar timeout tr uname; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto"
remote "systemctl is-active --quiet nginx"
remote "test -e '${DMX_PORT}'"
remote "test \"\$(uname -m)\" = aarch64"
remote "grep -Eq '^VERSION_ID=\"?11\"?$' /etc/os-release"
if [[ "${ACCEPTANCE_VARIANT}" == dev013d || "${ACCEPTANCE_VARIANT}" == dev014c ]]; then
    TARGET_IDENTITY="$(remote "printf 'model='; tr -d '\\000' </proc/device-tree/model 2>/dev/null || true; printf ';wb_release='; tr -d '\\n' </etc/wb-release 2>/dev/null || true; printf ';kernel='; uname -r" | tr -d '\r')"
    record "target_identity: ${TARGET_IDENTITY}"
    record "build_toolchain_identity: Bullseye GCC 10.2.1 aarch64-linux-gnu-g++"
    record "build_sysroot_identity: Debian 11 Bullseye ARM64; libmosquitto 2.0.11"
fi

PREV_SERVICE_ACTIVE="$(remote "systemctl is-active --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
PREV_SERVICE_ENABLED="$(remote "systemctl is-enabled --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
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
if (( PREV_SERVICE_ACTIVE == 1 )); then
    PREV_SERVICE_PID="$(service_pid)"
    if [[ ! "${PREV_SERVICE_PID}" =~ ^[1-9][0-9]*$ ]] ||
       [[ "${RUNNING_DMXWB}" != "${PREV_SERVICE_PID} /usr/local/bin/dmxwb" ]]; then
        echo "Active dmxwb.service не совпадает с ожидаемым управляемым процессом:" >&2
        printf '%s\n' "${RUNNING_DMXWB:-<not found>}" >&2
        exit 1
    fi
    remote "systemctl stop dmxwb.service"
    PREFLIGHT_STOPPED_SERVICE=1
    record "existing_managed_dmxwb_service_stopped_for_backup: PASS (pid=${PREV_SERVICE_PID})"
elif [[ -n "${RUNNING_DMXWB}" ]]; then
    echo "На WB8 запущен неуправляемый DMXWB процесс:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
if ! serial_port_is_released; then
    echo "Порт ${DMX_PORT} уже используется другим процессом." >&2
    exit 1
fi

remote "rm -rf -- '${REMOTE_DIR}'; mkdir -p '${REMOTE_DIR}/backup'"
backup_remote_path "/usr/local/bin/dmxwb" binary
backup_remote_path "/etc/systemd/system/dmxwb.service" unit
backup_remote_path "/etc/dmxwb" config_dir
backup_remote_path "/var/lib/dmxwb" state_dir
backup_remote_path "/var/www/dmxwb" web_dir
REMOTE_BACKUP_READY=1
PREFLIGHT_STOPPED_SERVICE=0
capture_retained
record "persistent_original_environment_captured: PASS"

remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true; systemctl disable dmxwb.service >/dev/null 2>&1 || true; rm -f -- /usr/local/bin/dmxwb /etc/systemd/system/dmxwb.service; rm -rf -- /etc/dmxwb /var/lib/dmxwb /var/www/dmxwb; systemctl daemon-reload"
clear_retained
scp "${SCP_OPTS[@]}" "${ARCHIVE}" "${TARGET}:${REMOTE_ARCHIVE}" >/dev/null
if (( NATURAL_ENTITY_WORKFLOW == 0 )); then
    scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/config.json" "${TARGET}:${REMOTE_DIR}/acceptance-config.json" >/dev/null
fi
remote "test \"\$(sha256sum '${REMOTE_ARCHIVE}' | awk '{print \$1}')\" = '${ARCHIVE_SHA256}'; mkdir -p '${REMOTE_DIR}/bundle'; tar -C '${REMOTE_DIR}/bundle' -xzf '${REMOTE_ARCHIVE}'; cd '${REMOTE_BUNDLE}'; bash ./install.sh" | tee -a "${REPORT}"
wait_service_active >/dev/null
remote "test \"\$(sha256sum /usr/local/bin/dmxwb | awk '{print \$1}')\" = '${ARTIFACT_SHA256}'"
if [[ "${ACCEPTANCE_VARIANT}" == dev013d ]]; then
    record "exact_dev013a_bundle_and_binary_installed: PASS (${ARCHIVE_SHA256}; ${ARTIFACT_SHA256})"
elif [[ "${ACCEPTANCE_VARIANT}" == dev014c ]]; then
    record "exact_dev014c_bundle_and_binary_installed: PASS (${ARCHIVE_SHA256}; ${ARTIFACT_SHA256})"
fi
if (( NATURAL_ENTITY_WORKFLOW == 1 )); then
    PID_BEFORE_REBOOT="$(wait_service_active)"
    wait_topic "/devices/dmxwb/controls/status" running 80
    wait_config_value "0|0|0|0|1|1|1" '
import json,sys
c=json.load(sys.stdin)
print(c["revision"], c["fixtures"]["count"], len(c["groups"]), len(c["scenes"]), c["id_counters"]["next_fixture_id"], c["id_counters"]["next_group_id"], c["id_counters"]["next_scene_id"], sep="|")
'
    remote "cmp -s '${REMOTE_BUNDLE}/payload/etc/dmxwb/config.example.json' /etc/dmxwb/config.json"
    record "clean_install_zero_fixture_group_scene_config: PASS"

    remote "python3 - <<'PY'
from urllib.request import urlopen
with urlopen('http://127.0.0.1/dmxwb/', timeout=5) as response:
    assert response.status == 200
    assert b'DMXWB' in response.read()
PY"
    echo
    echo "=== Dedicated Web natural entity workflow ==="
    echo "Откройте http://${TARGET_HOST}/dmxwb/ и выполните Ctrl+F5."
    echo "В Настройках: укажите Start Address ${START_ADDRESS}, добавьте два светильника,"
    echo "затем добавьте одну группу, отметьте в ней оба светильника и нажмите Применить."
    if ! ask_yes_no "Web показал «Конфигурация сохранена», два светильника и группу из двух участников?"; then
        exit 1
    fi
    wait_config_value "1|${START_ADDRESS}|2|1,2|1|1,2|3|2|1" '
import json,sys
c=json.load(sys.stdin)
ids=",".join(str(x["id"]) for x in c["fixtures"]["items"])
members=",".join(str(x) for x in c["groups"][0]["members"])
print(c["revision"], c["fixtures"]["start_address"], c["fixtures"]["count"], ids, len(c["groups"]), members, c["id_counters"]["next_fixture_id"], c["id_counters"]["next_group_id"], c["id_counters"]["next_scene_id"], sep="|")
'
    record "web_fixture_add_before_group_membership_apply: PASS"

    echo "В разделе «Светильники и группы» задайте Светильнику 1 КРАСНЫЙ, Светильнику 2 СИНИЙ и включите оба."
    echo "Затем в разделе «Сцены» создайте сцену с именем DEV014C Scene."
    if ! ask_yes_no "Сцена создана, а физический первый светильник стабильно КРАСНЫЙ?"; then
        exit 1
    fi
    wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 80
    wait_topic "/devices/dmxwb_fixture_1/controls/power" 1 80
    wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;0;255" 80
    wait_topic "/devices/dmxwb_fixture_2/controls/power" 1 80
    wait_topic "/devices/dmxwb_scene_1/controls/name" "DEV014C Scene" 80
    wait_config_value "2|1|1|1,2|2" '
import json,sys
c=json.load(sys.stdin); s=c["scenes"][0]
ids=",".join(str(x["fixture_id"]) for x in s["fixtures"])
print(c["revision"], len(c["scenes"]), s["id"], ids, c["id_counters"]["next_scene_id"], sep="|")
'
    record "web_scene_created_from_two_fixture_state: PASS"

    echo "В Настройках удалите именно Светильник 1 и нажмите Применить."
    if ! ask_yes_no "В списке остался Светильник 2, в группе остался только он, а физический свет стал СИНИМ?"; then
        exit 1
    fi
    wait_config_value "3|1|2|1|2|1|1,2|3|2|2" '
import json,sys
c=json.load(sys.stdin); s=c["scenes"][0]
ids=",".join(str(x["id"]) for x in c["fixtures"]["items"])
members=",".join(str(x) for x in c["groups"][0]["members"])
scene_ids=",".join(str(x["fixture_id"]) for x in s["fixtures"])
print(c["revision"], c["fixtures"]["count"], ids, len(c["groups"]), members, len(c["scenes"]), scene_ids, c["id_counters"]["next_fixture_id"], c["id_counters"]["next_group_id"], c["id_counters"]["next_scene_id"], sep="|")
'
    wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;0;255" 80
    if [[ -n "$(mqtt_get "/devices/dmxwb_fixture_1/meta" | tr -d '\r\n')" ]]; then
        echo "Удалённый Fixture 1 остался в retained MQTT metadata." >&2
        exit 1
    fi
    record "web_arbitrary_fixture_delete_membership_cleanup_scene_history: PASS"

    echo
    echo "=== Standard WB HomeUI entity controls ==="
    wait_topic_contains "/devices/dmxwb_fixture_2/controls/power/meta" '"hidden":false'
    wait_topic_contains "/devices/dmxwb_group_1/controls/color/meta" '"hidden":false'
    wait_topic_contains "/devices/dmxwb_scene_1/controls/apply/meta" '"hidden":false'
    echo "Откройте штатный Web Wiren Board, не /dmxwb/."
    echo "Должны быть видны DMXWB, Светильник 2, Группа 1 и DEV014C Scene; Светильника 1 быть не должно."
    echo "Через Группу 1 установите ЗЕЛЁНЫЙ и Power ON."
    if ! ask_yes_no "Все нужные устройства видны, лишнего Fixture 1 нет, а физический свет стал ЗЕЛЁНЫМ?"; then
        exit 1
    fi
    wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;255;0" 80
    echo "В штатном WB Web нажмите Применить у DEV014C Scene."
    if ! ask_yes_no "Сцена вернула физический СИНИЙ свет?"; then
        exit 1
    fi
    wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;0;255" 80
    echo "В штатном WB Web установите Светильнику 2 КРАСНЫЙ и Power ON."
    if ! ask_yes_no "Физический свет стал стабильно КРАСНЫМ?"; then
        exit 1
    fi
    wait_topic "/devices/dmxwb_fixture_2/controls/color" "255;0;0" 80
    wait_topic "/devices/dmxwb_fixture_2/controls/power" 1 80
    record "standard_wb_homeui_fixture_group_scene_controls: PASS"
else
    remote "systemctl stop dmxwb.service; install -m 0644 '${REMOTE_DIR}/acceptance-config.json' /etc/dmxwb/config.json; rm -f -- /var/lib/dmxwb/state.json"
    clear_retained
    remote "systemctl start dmxwb.service"
    PID_BEFORE_REBOOT="$(wait_service_active)"
    wait_topic "/devices/dmxwb/controls/status" running 80
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 1"
    wait_topic "/devices/dmxwb_fixture_1/controls/red" 255 60
    wait_topic "/devices/dmxwb_fixture_1/controls/power" 1 60
fi

sleep 3
validate_reboot_state
validate_operational_status
if ! ask_yes_no "Перед reboot физический светильник стабильно КРАСНЫЙ?"; then
    exit 1
fi
record "pre_reboot_persisted_red_state: PASS (pid=${PID_BEFORE_REBOOT})"

BOOT_ID_BEFORE="$(remote "cat /proc/sys/kernel/random/boot_id" | tr -d '\r\n')"
CONFIG_SHA_BEFORE="$(remote "sha256sum /etc/dmxwb/config.json | awk '{print \$1}'" | tr -d '\r\n')"
remote "sync"

echo
echo "=== WB8 reboot ==="
set +e
remote "systemctl reboot" >/dev/null 2>&1
set -e
close_ssh_master
wait_ssh_port_down
record "wb8_shutdown_observed: PASS"
wait_ssh_port_up 240
echo "WB8 снова доступен."
open_ssh_master
remote "test -f '${REMOTE_DIR}/backup/binary.present'; test -f '${REMOTE_DIR}/retained-backup/0.present'"

BOOT_ID_AFTER="$(remote "cat /proc/sys/kernel/random/boot_id" | tr -d '\r\n')"
if [[ -z "${BOOT_ID_BEFORE}" || -z "${BOOT_ID_AFTER}" || "${BOOT_ID_BEFORE}" == "${BOOT_ID_AFTER}" ]]; then
    echo "boot_id не изменился; фактический reboot не подтверждён." >&2
    exit 1
fi
PID_AFTER_REBOOT="$(wait_service_active)"
remote "systemctl is-enabled --quiet dmxwb.service"
wait_topic "/devices/dmxwb/controls/status" running 120
wait_status_value application running 120
wait_status_value diagnostics.mqtt.state connected 120
wait_status_value diagnostics.dmx.state running 120
wait_status_value diagnostics.selected_source mqtt 120
remote "test \"\$(sha256sum /etc/dmxwb/config.json | awk '{print \$1}')\" = '${CONFIG_SHA_BEFORE}'"
validate_reboot_state
validate_operational_status
record "wb8_reboot_autostart_config_state_mqtt_restore: PASS (${BOOT_ID_BEFORE} -> ${BOOT_ID_AFTER}, pid=${PID_AFTER_REBOOT})"

if ! ask_yes_no "После полного reboot светильник автоматически восстановил стабильный КРАСНЫЙ свет?"; then
    exit 1
fi
record "post_reboot_physical_red_user: PASS"

remote "python3 - <<'PY'
from urllib.request import urlopen
with urlopen('http://127.0.0.1/dmxwb/', timeout=5) as response:
    body = response.read()
    assert response.status == 200
    assert b'DMXWB' in body
PY"
echo "Откройте http://${TARGET_HOST}/dmxwb/ и обновите страницу через Ctrl+F5."
if ! ask_yes_no "После reboot Web открылся из LAN, показывает связь и доступные команды?"; then
    exit 1
fi
record "post_reboot_local_web_user: PASS"

echo
echo "=== MQTT / Art-Net explicit Source ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
wait_topic "/devices/dmxwb/controls/source" artnet 40
wait_status_value diagnostics.selected_source artnet 80
send_artdmx 0 0 255 0
wait_status_value diagnostics.artnet.state ACTIVE 40
if ! ask_yes_no "Source=ART-NET: физический светильник стабильно СИНИЙ?"; then
    exit 1
fi
record "post_reboot_artnet_physical_blue_user: PASS"

remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt"
wait_topic "/devices/dmxwb/controls/source" mqtt 40
wait_status_value diagnostics.selected_source mqtt 80
if ! ask_yes_no "После возврата Source=WB MQTT светильник снова стабильно КРАСНЫЙ?"; then
    exit 1
fi
record "post_reboot_explicit_source_return_red_user: PASS"

if (( RUN_EXTENDED_RECOVERY == 1 )); then
    echo
    echo "=== Process crash and Mosquitto recovery ==="
    PID_BEFORE_CRASH="$(service_pid)"
    remote "kill -KILL '${PID_BEFORE_CRASH}'"
    PID_AFTER_CRASH="$(wait_service_pid_change "${PID_BEFORE_CRASH}")"
    wait_topic "/devices/dmxwb/controls/status" running 120
    wait_status_value diagnostics.mqtt.state connected 120
    if ! ask_yes_no "После automatic systemd recovery светильник снова стабильно КРАСНЫЙ?"; then
        exit 1
    fi
    record "systemd_process_crash_recovery_physical_user: PASS (${PID_BEFORE_CRASH} -> ${PID_AFTER_CRASH})"

    PID_BEFORE_MQTT_RESTART="$(service_pid)"
    remote "systemctl restart mosquitto"
    wait_status_value diagnostics.mqtt.state connected 160
    wait_status_value diagnostics.mqtt.recovery_state ok 160
    PID_AFTER_MQTT_RESTART="$(service_pid)"
    if [[ "${PID_AFTER_MQTT_RESTART}" != "${PID_BEFORE_MQTT_RESTART}" ]]; then
        echo "DMXWB PID изменился при recoverable Mosquitto restart." >&2
        exit 1
    fi
    if ! ask_yes_no "Во время restart Mosquitto физический КРАСНЫЙ свет оставался стабильным?"; then
        exit 1
    fi
    record "mosquitto_in_process_recovery_physical_user: PASS (pid=${PID_AFTER_MQTT_RESTART})"

    echo
    echo "=== Standard WB UI ==="
    echo "Откройте стандартный интерфейс Wiren Board (не /dmxwb/)."
    echo "От DMXWB должны быть видны системные Status/Source и ${FIXTURE_NAME} с его командами."
    if ! ask_yes_no "Стандартный WB UI соответствует этому?"; then
        exit 1
    fi
    record "standard_wb_homeui_visible_fixture_user: PASS"
fi

remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_${ACTIVE_FIXTURE_ID}/controls/power/on' -m 0"
wait_topic "/devices/dmxwb_fixture_${ACTIVE_FIXTURE_ID}/controls/power" 0 80
if ! ask_yes_no "Перед восстановлением исходной среды светильник полностью выключен?"; then
    exit 1
fi
remote "systemctl stop dmxwb.service"
if ! serial_port_is_released; then
    echo "После clean stop порт ${DMX_PORT} остался открыт." >&2
    exit 1
fi
record "final_power_off_clean_stop_port_release: PASS"

restore_remote_environment
record "original_environment_restored: PASS"
record "${ACCEPTANCE_VARIANT}_result: PASS"
record "=== DMXWB ${ACCEPTANCE_ID} ${ACCEPTANCE_TITLE} ACCEPTANCE PASS ==="

echo
echo "=== DMXWB ${ACCEPTANCE_ID} ${ACCEPTANCE_TITLE} ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf -- '${REMOTE_DIR}'"
close_ssh_master
rm -rf -- "${LOCAL_TMP}"
CLEANED=1
trap - EXIT INT TERM
