#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

TARGET="${1:-root@10.200.200.1}"
START_ADDRESS="${2:-21}"
DMX_PORT="/dev/ttyRS485-1"
ARTNET_PORT_ADDRESS=0
ARTNET_HOST="${TARGET##*@}"

if [[ $# -gt 2 ]]; then
    echo "Usage: bash tools/wb8/run_dev013c_integrated_artnet_web_acceptance.sh [SSH_TARGET] [FIXTURE_START_ADDRESS]" >&2
    exit 2
fi
if [[ ! "${ARTNET_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "SSH_TARGET должен содержать IPv4 WB8, например root@10.200.200.1." >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] ||
   (( START_ADDRESS < 2 || START_ADDRESS > 297 )); then
    echo "Для DEV-013C Start Address должен быть целым числом 2..297." >&2
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
REPORT="${REPO_ROOT}/docs/DEV013C_INTEGRATED_ARTNET_WEB_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev013c-acceptance"
REMOTE_ARCHIVE="${REMOTE_DIR}/dmxwb-offline.tar.gz"
REMOTE_BUNDLE="${REMOTE_DIR}/bundle/dmxwb-wb8-bullseye-arm64"
LOCAL_TMP="$(mktemp -d)"
CONTROL_PATH="${LOCAL_TMP}/ssh-control-%C"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=${CONTROL_PATH}" -o ControlPersist=1200 -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-o ControlMaster=auto -o "ControlPath=${CONTROL_PATH}" -o ControlPersist=1200 -o StrictHostKeyChecking=accept-new)

for command_name in awk bash cat date mktemp python3 rm scp sed seq sha256sum sleep ssh tar tee tr; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена локальная команда: ${command_name}" >&2
        exit 1
    }
done
if [[ ! -f "${ARCHIVE}" ]]; then
    echo "Не найден финальный архив DEV-013A: ${ARCHIVE}" >&2
    exit 1
fi

ARCHIVE_SHA256="$(sha256sum "${ARCHIVE}" | awk '{print $1}')"
SOURCE_ID="$(tar -xOf "${ARCHIVE}" dmxwb-wb8-bullseye-arm64/MANIFEST.txt | sed -n 's/^source_id=//p')"
ARTIFACT_SHA256="$(tar -xOf "${ARCHIVE}" dmxwb-wb8-bullseye-arm64/MANIFEST.txt | sed -n 's/^artifact_sha256=//p')"
if [[ "${SOURCE_ID}" != "dev013a-final" ]]; then
    echo "Ожидался финальный source_id=dev013a-final, получено '${SOURCE_ID}'." >&2
    exit 1
fi

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${DMX_PORT}"},"artnet":{"universe":${ARTNET_PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV013C Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

: > "${REPORT}"
record() { printf '%s\n' "$1" | tee -a "${REPORT}"; }
ask_yes_no() {
    local answer
    while true; do
        read -r -p "$1 (y/n): " answer
        case "${answer}" in
            y|Y) return 0 ;;
            n|N) return 1 ;;
            *) echo "Введите y или n." ;;
        esac
    done
}

SSH_OPENED=0
BACKUP_READY=0
RETAINED_CAPTURED=0
CLEANED=0
PREV_SERVICE_ACTIVE=0
PREV_SERVICE_ENABLED=0

remote() { ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"; }

backup_path() {
    local path="$1" key="$2"
    remote "
if test -e '${path}' || test -L '${path}'; then
    cp -a -- '${path}' '${REMOTE_DIR}/backup/${key}'
    printf '1\n' > '${REMOTE_DIR}/backup/${key}.present'
else
    printf '0\n' > '${REMOTE_DIR}/backup/${key}.present'
fi
"
}

restore_path() {
    local path="$1" key="$2"
    remote "
rm -rf -- '${path}'
if test \"\$(cat '${REMOTE_DIR}/backup/${key}.present' 2>/dev/null)\" = 1; then
    cp -a -- '${REMOTE_DIR}/backup/${key}' '${path}'
fi
"
}

RETAINED_TOPICS=(
    /dmxwb/config /dmxwb/state /dmxwb/status
    /devices/dmxwb/meta
    /devices/dmxwb/controls/status/meta /devices/dmxwb/controls/status
    /devices/dmxwb/controls/source/meta /devices/dmxwb/controls/source
    /devices/dmxwb_fixture_1/meta
)
for control in name power red green blue color brightness temperature; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_fixture_1/controls/${control}/meta"
        "/devices/dmxwb_fixture_1/controls/${control}"
    )
done
RETAINED_TOPICS+=(/devices/dmxwb_fixture_1/controls/reset/meta)

capture_retained() {
    remote "mkdir -p '${REMOTE_DIR}/retained-backup'"
    local index=0 topic
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "
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
    local topic
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "mosquitto_pub -h 127.0.0.1 -p 1883 -r -n -t '${topic}'" >/dev/null
    done
}

restore_retained() {
    (( RETAINED_CAPTURED == 1 )) || return 0
    local index=0 topic
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

restore_environment() {
    (( BACKUP_READY == 1 )) || return 0
    remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
    restore_retained >/dev/null 2>&1 || true
    restore_path /usr/local/bin/dmxwb binary
    restore_path /etc/systemd/system/dmxwb.service unit
    restore_path /etc/dmxwb config_dir
    restore_path /var/lib/dmxwb state_dir
    restore_path /var/www/dmxwb web_dir
    remote "systemctl daemon-reload"
    if (( PREV_SERVICE_ENABLED == 1 )); then
        remote "systemctl enable dmxwb.service >/dev/null 2>&1 || true"
    else
        remote "systemctl disable dmxwb.service >/dev/null 2>&1 || true"
    fi
    if (( PREV_SERVICE_ACTIVE == 1 )); then
        remote "systemctl start dmxwb.service >/dev/null 2>&1 || true"
    fi
    BACKUP_READY=0
}

cleanup() {
    local result=$?
    trap - EXIT INT TERM
    set +e
    if (( CLEANED == 0 )); then
        if (( SSH_OPENED == 1 )); then
            remote "systemctl start mosquitto >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
            restore_environment >/dev/null 2>&1 || true
            remote "rm -rf -- '${REMOTE_DIR}'" >/dev/null 2>&1 || true
            ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
        fi
        rm -rf -- "${LOCAL_TMP}" >/dev/null 2>&1 || true
    fi
    exit "${result}"
}
trap cleanup EXIT INT TERM

mqtt_get() {
    remote "timeout 1 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '$1' 2>/dev/null || true"
}
mqtt_pub() {
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '$1' -m '$2'"
}
wait_topic() {
    local topic="$1" expected="$2" attempts="${3:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(mqtt_get "${topic}" | tr -d '\r\n')"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Ожидался MQTT ${topic}='${expected}', получено '${value}'." >&2
    return 1
}

status_value() {
    mqtt_get /dmxwb/status | python3 -c '
import json, sys
value = json.load(sys.stdin)
for part in sys.argv[1].split("."):
    value = value[part]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("null")
else:
    print(value)
' "$1" 2>/dev/null | tr -d '\r\n'
}
wait_status_value() {
    local path="$1" expected="$2" attempts="${3:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(status_value "${path}" || true)"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Ожидался /dmxwb/status ${path}='${expected}', получено '${value}'." >&2
    return 1
}

state_field() {
    mqtt_get /dmxwb/state | python3 -c '
import json, sys
field = sys.argv[1]
fixture = next(item for item in json.load(sys.stdin)["fixtures"] if item["id"] == 1)
value = fixture[field]
print("true" if value is True else "false" if value is False else value)
' "$1" 2>/dev/null | tr -d '\r\n'
}
wait_state_field() {
    local field="$1" expected="$2" attempts="${3:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(state_field "${field}" || true)"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Fixture state.${field}: ожидалось '${expected}', получено '${value}'." >&2
    return 1
}

service_pid() { remote "systemctl show -p MainPID --value dmxwb.service" | tr -d '\r\n'; }
wait_service_active() {
    local pid
    for _ in $(seq 1 120); do
        if remote "systemctl is-active --quiet dmxwb.service"; then
            pid="$(service_pid)"
            [[ "${pid}" =~ ^[1-9][0-9]*$ ]] && { printf '%s' "${pid}"; return 0; }
        fi
        sleep 0.25
    done
    remote "systemctl status dmxwb.service --no-pager -l || true" >&2
    return 1
}
serial_released() {
    remote "for fd in /proc/[0-9]*/fd/*; do test \"\$(readlink \"\${fd}\" 2>/dev/null || true)\" != '${DMX_PORT}' || exit 1; done"
}

send_artdmx() {
    local sequence="$1" length="$2" red="$3" green="$4" blue="$5" white="$6" duration="${7:-0.8}"
    python3 - "${ARTNET_HOST}" "${ARTNET_PORT_ADDRESS}" "${START_ADDRESS}" \
        "${sequence}" "${length}" "${red}" "${green}" "${blue}" "${white}" "${duration}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
start_index = int(sys.argv[3]) - 1
sequence = int(sys.argv[4])
length = int(sys.argv[5])
values = [int(value) for value in sys.argv[6:10]]
duration = float(sys.argv[10])
dmx = bytearray(length)
for offset, value in enumerate(values):
    index = start_index + offset
    if index < length:
        dmx[index] = value
packet = (
    b"Art-Net\x00"
    + struct.pack("<H", 0x5000)
    + bytes((0, 14, sequence, 0, port_address & 0xff, (port_address >> 8) & 0x7f))
    + struct.pack(">H", length)
    + dmx
)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        sock.sendto(packet, (host, 6454))
        time.sleep(0.2)
finally:
    sock.close()
PY
}

wait_revision_gt() {
    local baseline="$1" attempts="${2:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(status_value diagnostics.artnet.committed_revision || true)"
        if [[ "${value}" =~ ^[0-9]+$ ]] && (( value > baseline )); then return 0; fi
        sleep 0.25
    done
    echo "Art-Net committed_revision не увеличился после ${baseline}; получено '${value}'." >&2
    return 1
}

record "=== DMXWB DEV-013C INTEGRATED ART-NET/SOURCE/WEB/RECOVERY ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "target: ${TARGET}"
record "bundle_sha256: ${ARCHIVE_SHA256}"
record "source_id: ${SOURCE_ID}"
record "artifact_sha256: ${ARTIFACT_SHA256}"
record "dmx_port: ${DMX_PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "artnet_port_address: ${ARTNET_PORT_ADDRESS}"

echo "DMXWB — DEV-013C integrated Art-Net/Source/Web/recovery acceptance"
if ! ask_yes_no "RGBW-светильник подключён к ${DMX_PORT} и имеет Start Address ${START_ADDRESS}?"; then exit 2; fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT полностью отключён?"; then exit 2; fi

echo "=== WB8 backup and final bundle installation ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1
remote "for c in awk bash cat cp install mkdir mosquitto_pub mosquitto_sub python3 readlink rm sha256sum systemctl tar timeout tr uname; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto; test -e '${DMX_PORT}'; test \"\$(uname -m)\" = aarch64"
if ! serial_released; then echo "Порт ${DMX_PORT} уже занят." >&2; exit 1; fi

remote "rm -rf -- '${REMOTE_DIR}'; mkdir -p '${REMOTE_DIR}/backup' '${REMOTE_DIR}/bundle'"
PREV_SERVICE_ACTIVE="$(remote "systemctl is-active --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
PREV_SERVICE_ENABLED="$(remote "systemctl is-enabled --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
backup_path /usr/local/bin/dmxwb binary
backup_path /etc/systemd/system/dmxwb.service unit
backup_path /etc/dmxwb config_dir
backup_path /var/lib/dmxwb state_dir
backup_path /var/www/dmxwb web_dir
BACKUP_READY=1
capture_retained

remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true; systemctl disable dmxwb.service >/dev/null 2>&1 || true; rm -f -- /usr/local/bin/dmxwb /etc/systemd/system/dmxwb.service; rm -rf -- /etc/dmxwb /var/lib/dmxwb /var/www/dmxwb; systemctl daemon-reload"
clear_retained
scp "${SCP_OPTS[@]}" "${ARCHIVE}" "${TARGET}:${REMOTE_ARCHIVE}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/config.json" "${TARGET}:${REMOTE_DIR}/config.json" >/dev/null
remote "test \"\$(sha256sum '${REMOTE_ARCHIVE}' | awk '{print \$1}')\" = '${ARCHIVE_SHA256}'; tar -C '${REMOTE_DIR}/bundle' -xzf '${REMOTE_ARCHIVE}'; cd '${REMOTE_BUNDLE}'; bash ./install.sh" | tee -a "${REPORT}"
remote "systemctl stop dmxwb.service; install -m 0644 '${REMOTE_DIR}/config.json' /etc/dmxwb/config.json; rm -f -- /var/lib/dmxwb/state.json"
clear_retained
remote "systemctl start dmxwb.service"
PID_INITIAL="$(wait_service_active)"
wait_topic /devices/dmxwb/controls/status running
wait_topic /devices/dmxwb/controls/source mqtt
wait_status_value diagnostics.dmx.state running
wait_status_value diagnostics.artnet.transport_open true
record "final_bundle_runtime_ready: PASS (pid=${PID_INITIAL})"

echo "=== Art-Net length, mapping and explicit Source ==="
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
mqtt_pub /devices/dmxwb_fixture_1/controls/brightness/on 100
mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 1
wait_state_field red 255
if ! ask_yes_no "Source=WB MQTT: физический светильник стабильно КРАСНЫЙ?"; then exit 1; fi

mqtt_pub /devices/dmxwb/controls/source/on artnet
wait_topic /devices/dmxwb/controls/source artnet
wait_status_value diagnostics.selected_source artnet
send_artdmx 0 512 0 0 255 0
wait_status_value diagnostics.artnet.state ACTIVE
wait_status_value diagnostics.artnet.output_mode live
if ! ask_yes_no "512-канальный Art-Net кадр на Start Address ${START_ADDRESS} дал стабильный СИНИЙ?"; then exit 1; fi

REVISION_BEFORE_SHORT="$(status_value diagnostics.artnet.committed_revision)"
send_artdmx 0 2 0 0 0 0 0.4
wait_revision_gt "${REVISION_BEFORE_SHORT}"
if ! ask_yes_no "Короткий допустимый Art-Net кадр сохранил СИНИЕ каналы за своей длиной без вспышки/blackout?"; then exit 1; fi

send_artdmx 0 512 0 255 0 0
wait_status_value diagnostics.artnet.state ACTIVE
if ! ask_yes_no "Следующий 512-канальный кадр сделал светильник стабильно ЗЕЛЁНЫМ?"; then exit 1; fi
record "artnet_nondefault_mapping_short_hold_long_update: PASS"

mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
wait_state_field red 255
wait_state_field green 0
if ! ask_yes_no "При Source=ART-NET MQTT обновил состояние, но физический свет остался ЗЕЛЁНЫМ?"; then exit 1; fi
mqtt_pub /devices/dmxwb/controls/source/on mqtt
wait_topic /devices/dmxwb/controls/source mqtt
if ! ask_yes_no "Явный переход на WB MQTT целым кадром дал стабильный КРАСНЫЙ?"; then exit 1; fi

send_artdmx 0 512 0 0 255 0
if ! ask_yes_no "Новый Art-Net кадр в фоне не изменил физический КРАСНЫЙ при Source=WB MQTT?"; then exit 1; fi
mqtt_pub /devices/dmxwb/controls/source/on artnet
wait_topic /devices/dmxwb/controls/source artnet
if ! ask_yes_no "Явный переход на ART-NET сразу вывел последний цельный СИНИЙ кадр?"; then exit 1; fi
record "explicit_source_background_latest_snapshot_no_mixing: PASS"

echo "=== Art-Net LOST / Hold Last / restarted sequence recovery ==="
send_artdmx 128 512 0 0 255 0 0.8
wait_status_value diagnostics.artnet.last_sequence 128
PID_BEFORE_ARTNET_LOSS="$(service_pid)"
sleep 3.3
wait_status_value diagnostics.artnet.state LOST
wait_topic /devices/dmxwb/controls/source artnet
if ! ask_yes_no "После Art-Net LOST выбранный Source остался ART-NET, а светильник удержал стабильный СИНИЙ?"; then exit 1; fi
send_artdmx 1 512 0 255 0 0 1.0
wait_status_value diagnostics.artnet.state ACTIVE
wait_status_value diagnostics.artnet.last_sequence 1
PID_AFTER_ARTNET_RECOVERY="$(service_pid)"
if [[ "${PID_AFTER_ARTNET_RECOVERY}" != "${PID_BEFORE_ARTNET_LOSS}" ]]; then
    echo "DMXWB PID изменился при Art-Net LOST/recovery." >&2
    exit 1
fi
if ! ask_yes_no "Возобновившийся контроллер с Sequence=1 без restart DMXWB дал стабильный ЗЕЛЁНЫЙ?"; then exit 1; fi
record "artnet_hold_last_restarted_sequence_same_process: PASS (pid=${PID_AFTER_ARTNET_RECOVERY})"

echo "=== MQTT/Web reconnect and daemon availability ==="
mqtt_pub /devices/dmxwb/controls/source/on mqtt
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
wait_topic /devices/dmxwb/controls/source mqtt
wait_state_field red 255
echo "Откройте http://${ARTNET_HOST}/dmxwb/, обновите страницу через Ctrl+F5 и дождитесь состояния «Связь установлена»."
if ! ask_yes_no "Web показывает работающую DMXWB и доступные команды, физический свет КРАСНЫЙ?"; then exit 1; fi

PID_BEFORE_MQTT_RESTART="$(service_pid)"
remote "systemctl restart mosquitto"
for _ in $(seq 1 80); do remote "systemctl is-active --quiet mosquitto" && break; sleep 0.25; done
remote "systemctl is-active --quiet mosquitto"
wait_topic /devices/dmxwb/controls/status running 160
wait_status_value diagnostics.mqtt.connected true 160
wait_status_value diagnostics.mqtt.recovery_state ok 160
PID_AFTER_MQTT_RESTART="$(service_pid)"
if [[ "${PID_AFTER_MQTT_RESTART}" != "${PID_BEFORE_MQTT_RESTART}" ]]; then
    echo "DMXWB PID изменился при restart Mosquitto." >&2
    exit 1
fi
if ! ask_yes_no "Во время restart Mosquitto КРАСНЫЙ свет был стабильным, а Web сам восстановил связь и команды?"; then exit 1; fi
record "mqtt_web_reconnect_same_process_physical_continuity: PASS (pid=${PID_AFTER_MQTT_RESTART})"

echo "В Web установите Fixture Color = СИНИЙ и Power = ON."
if ! ask_yes_no "Web-команда после reconnect сделала физический светильник СИНИМ?"; then exit 1; fi
wait_state_field red 0
wait_state_field green 0
wait_state_field blue 255

remote "systemctl stop dmxwb.service"
wait_topic /devices/dmxwb/controls/status off
if ! ask_yes_no "После stop Web без reload показал недоступность DMXWB и заблокировал команды?"; then exit 1; fi
remote "systemctl start dmxwb.service"
PID_AFTER_START="$(wait_service_active)"
wait_topic /devices/dmxwb/controls/status running 120
if ! ask_yes_no "После start Web без reload снова показал работающую DMXWB и разрешил команды?"; then exit 1; fi
echo "В Web установите Fixture Color = КРАСНЫЙ и Power = ON."
if ! ask_yes_no "Web-команда после возврата службы сделала физический светильник КРАСНЫМ?"; then exit 1; fi
wait_state_field red 255
wait_state_field green 0
wait_state_field blue 0
record "web_factual_stop_start_command_restore: PASS (pid=${PID_AFTER_START})"

mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 0
wait_topic /devices/dmxwb_fixture_1/controls/power 0
if ! ask_yes_no "Перед завершением физический светильник полностью выключен?"; then exit 1; fi
remote "systemctl stop dmxwb.service"
serial_released
record "final_power_off_clean_stop_port_release: PASS"

restore_environment
record "original_environment_restored: PASS"
record "dev013c_result: PASS"
record "=== DMXWB DEV-013C INTEGRATED ART-NET/SOURCE/WEB/RECOVERY ACCEPTANCE PASS ==="

echo "=== DMXWB DEV-013C INTEGRATED ART-NET/SOURCE/WEB/RECOVERY ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"
remote "rm -rf -- '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf -- "${LOCAL_TMP}"
CLEANED=1
trap - EXIT INT TERM
