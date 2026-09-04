#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

TARGET="${1:-root@10.200.200.1}"
START_ADDRESS="${2:-21}"
DMX_PORT="/dev/ttyRS485-1"

if [[ $# -gt 2 ]]; then
    echo "Usage: bash tools/wb8/run_dev013b_integrated_logical_acceptance.sh [SSH_TARGET] [FIXTURE_START_ADDRESS]" >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] ||
   (( START_ADDRESS < 2 || START_ADDRESS > 293 )); then
    echo "Для DEV-013B Start Address должен быть целым числом 2..293." >&2
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
REPORT="${REPO_ROOT}/docs/DEV013B_INTEGRATED_LOGICAL_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev013b-acceptance"
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
{"version":1,"revision":1,"dmx":{"port":"${DMX_PORT}"},"artnet":{"universe":0},"fixtures":{"count":2,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV013B Fixture A"},{"id":2,"name":"DEV013B Fixture B"}]},"groups":[{"id":1,"name":"DEV013B Pair","members":[1,2]}],"scenes":[],"id_counters":{"next_fixture_id":3,"next_group_id":2,"next_scene_id":1}}
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
)
for id in 1 2; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_fixture_${id}/meta"
        "/devices/dmxwb_fixture_${id}/controls/name/meta"
        "/devices/dmxwb_fixture_${id}/controls/name"
        "/devices/dmxwb_fixture_${id}/controls/power/meta"
        "/devices/dmxwb_fixture_${id}/controls/power"
        "/devices/dmxwb_fixture_${id}/controls/red/meta"
        "/devices/dmxwb_fixture_${id}/controls/red"
        "/devices/dmxwb_fixture_${id}/controls/green/meta"
        "/devices/dmxwb_fixture_${id}/controls/green"
        "/devices/dmxwb_fixture_${id}/controls/blue/meta"
        "/devices/dmxwb_fixture_${id}/controls/blue"
        "/devices/dmxwb_fixture_${id}/controls/color/meta"
        "/devices/dmxwb_fixture_${id}/controls/color"
        "/devices/dmxwb_fixture_${id}/controls/brightness/meta"
        "/devices/dmxwb_fixture_${id}/controls/brightness"
        "/devices/dmxwb_fixture_${id}/controls/temperature/meta"
        "/devices/dmxwb_fixture_${id}/controls/temperature"
        "/devices/dmxwb_fixture_${id}/controls/reset/meta"
    )
done
RETAINED_TOPICS+=(/devices/dmxwb_group_1/meta)
for control in name power red green blue color brightness temperature; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_group_1/controls/${control}/meta"
        "/devices/dmxwb_group_1/controls/${control}"
    )
done
RETAINED_TOPICS+=(/devices/dmxwb_group_1/controls/reset/meta)
for id in 1 2; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_scene_${id}/meta"
        "/devices/dmxwb_scene_${id}/controls/name/meta"
        "/devices/dmxwb_scene_${id}/controls/name"
        "/devices/dmxwb_scene_${id}/controls/apply/meta"
    )
done

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
            remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_group_1/controls/power/on' -m 0 >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
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
    local topic="$1" expected="$2" attempts="${3:-60}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(mqtt_get "${topic}" | tr -d '\r\n')"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Ожидался MQTT ${topic}='${expected}', получено '${value}'." >&2
    return 1
}

state_field() {
    mqtt_get /dmxwb/state | python3 -c '
import json, sys
fixture_id = int(sys.argv[1])
field = sys.argv[2]
state = json.load(sys.stdin)
fixture = next(item for item in state["fixtures"] if item["id"] == fixture_id)
value = fixture[field]
print("true" if value is True else "false" if value is False else value)
' "$1" "$2" 2>/dev/null | tr -d '\r\n'
}

wait_state_field() {
    local id="$1" field="$2" expected="$3" attempts="${4:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(state_field "${id}" "${field}" || true)"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Fixture ${id} state.${field}: ожидалось '${expected}', получено '${value}'." >&2
    return 1
}

config_probe() {
    mqtt_get /dmxwb/config | python3 -c "$1" 2>/dev/null | tr -d '\r\n'
}
wait_config_probe() {
    local expected="$1" code="$2" attempts="${3:-80}" value=""
    for _ in $(seq 1 "${attempts}"); do
        value="$(config_probe "${code}" || true)"
        [[ "${value}" == "${expected}" ]] && return 0
        sleep 0.25
    done
    echo "Конфигурация: ожидалось '${expected}', получено '${value}'." >&2
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

record "=== DMXWB DEV-013B INTEGRATED FIXTURE/GROUP/SCENE ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "target: ${TARGET}"
record "bundle_sha256: ${ARCHIVE_SHA256}"
record "source_id: ${SOURCE_ID}"
record "artifact_sha256: ${ARTIFACT_SHA256}"
record "dmx_port: ${DMX_PORT}"
record "fixture_start_address: ${START_ADDRESS}"

echo "DMXWB — DEV-013B integrated logical-lighting acceptance"
if ! ask_yes_no "Один RGBW-светильник подключён к ${DMX_PORT} и временно имеет Start Address ${START_ADDRESS}?"; then exit 2; fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT полностью отключён?"; then exit 2; fi

echo "=== WB8 backup and final bundle installation ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1
remote "for c in awk bash cat cmp cp install ldd mkdir mosquitto_pub mosquitto_sub python3 readlink rm sha256sum systemctl tar timeout tr uname; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
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
wait_config_probe "${START_ADDRESS}" 'import json,sys; print(json.load(sys.stdin)["fixtures"]["start_address"])'
wait_config_probe 2 'import json,sys; print(json.load(sys.stdin)["fixtures"]["count"])'
record "final_bundle_installed_nondefault_address: PASS (pid=${PID_INITIAL})"

echo "=== Fixture RGBW, Brightness, Power and Reset ==="
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
mqtt_pub /devices/dmxwb_fixture_1/controls/brightness/on 100
mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 1
wait_topic /devices/dmxwb_fixture_1/controls/color '255;0;0'
if ! ask_yes_no "Fixture A физически стабильно КРАСНЫЙ на ненулевом Start Address?"; then exit 1; fi

mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '0;255;0'
wait_topic /devices/dmxwb_fixture_1/controls/color '0;255;0'
if ! ask_yes_no "Fixture A стал стабильно ЗЕЛЁНЫМ?"; then exit 1; fi
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '0;0;255'
wait_topic /devices/dmxwb_fixture_1/controls/color '0;0;255'
if ! ask_yes_no "Fixture A стал стабильно СИНИМ?"; then exit 1; fi

mqtt_pub /devices/dmxwb_fixture_1/controls/temperature/on 100
wait_state_field 1 white 255
wait_state_field 1 temperature 100
if ! ask_yes_no "Temperature=100 дал яркий физический RGBW-белый?"; then exit 1; fi
mqtt_pub /devices/dmxwb_fixture_1/controls/brightness/on 20
wait_state_field 1 brightness 20
if ! ask_yes_no "Brightness=20 заметно уменьшил яркость без выключения?"; then exit 1; fi
mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 0
wait_topic /devices/dmxwb_fixture_1/controls/power 0
if ! ask_yes_no "Power OFF полностью выключил Fixture A?"; then exit 1; fi
mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 1
wait_state_field 1 brightness 20
if ! ask_yes_no "Power ON восстановил прежний приглушённый белый?"; then exit 1; fi
mqtt_pub /devices/dmxwb_fixture_1/controls/reset/on 1
for field in red green blue white brightness; do wait_state_field 1 "${field}" "$( [[ ${field} == brightness ]] && echo 100 || echo 255 )"; done
if ! ask_yes_no "Reset включил Fixture A на максимальный RGBW-белый и Brightness=100?"; then exit 1; fi
record "fixture_rgbw_brightness_power_reset_physical: PASS"

echo "=== Group logical state and physical member ==="
mqtt_pub /devices/dmxwb_group_1/controls/color/on '0;255;0'
mqtt_pub /devices/dmxwb_group_1/controls/brightness/on 100
mqtt_pub /devices/dmxwb_group_1/controls/power/on 1
for id in 1 2; do
    wait_state_field "${id}" red 0
    wait_state_field "${id}" green 255
    wait_state_field "${id}" blue 0
    wait_state_field "${id}" requested_power true
done
wait_topic /devices/dmxwb_group_1/controls/power 1
if ! ask_yes_no "Group-команда сделала физический Fixture A стабильно ЗЕЛЁНЫМ?"; then exit 1; fi
mqtt_pub /devices/dmxwb_group_1/controls/power/on 0
wait_topic /devices/dmxwb_group_1/controls/power 0
for id in 1 2; do wait_state_field "${id}" requested_power false; done
if ! ask_yes_no "Group Power OFF полностью выключил физический Fixture A?"; then exit 1; fi
mqtt_pub /devices/dmxwb_group_1/controls/power/on 1
for id in 1 2; do wait_state_field "${id}" requested_power true; done
if ! ask_yes_no "Group Power ON восстановил зелёное состояние Fixture A?"; then exit 1; fi
record "group_two_member_state_and_physical_member: PASS"

echo "=== Scene create, apply, overwrite, rename and delete ==="
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
mqtt_pub /devices/dmxwb_fixture_2/controls/color/on '0;0;255'
wait_state_field 1 red 255
wait_state_field 2 blue 255
mqtt_pub /dmxwb/scenes/create '{"request_id":"dev013b-create-1","name":"DEV013B Scene One"}'
wait_topic /devices/dmxwb_scene_1/controls/name 'DEV013B Scene One'
wait_config_probe 1 'import json,sys; print(len(json.load(sys.stdin)["scenes"]))'

mqtt_pub /devices/dmxwb_group_1/controls/color/on '0;255;0'
wait_state_field 1 green 255
if ! ask_yes_no "Перед Scene Apply физический Fixture A ЗЕЛЁНЫЙ?"; then exit 1; fi
mqtt_pub /dmxwb/scenes/1/apply '{"request_id":"dev013b-apply-1"}'
wait_state_field 1 red 255
wait_state_field 1 green 0
wait_state_field 2 blue 255
if ! ask_yes_no "Scene Apply одним переходом вернул физический Fixture A в КРАСНЫЙ?"; then exit 1; fi

mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '0;0;255'
mqtt_pub /devices/dmxwb_fixture_2/controls/color/on '255;0;0'
wait_state_field 1 blue 255
mqtt_pub /dmxwb/scenes/1/overwrite '{"request_id":"dev013b-overwrite-1"}'
wait_config_probe '0;0;255|255;0;0' '
import json, sys
config = json.load(sys.stdin)
scene = next(item for item in config["scenes"] if item["id"] == 1)
saved = {item["fixture_id"]: item for item in scene["fixtures"]}
print("{};{};{}|{};{};{}".format(
    saved[1]["red"], saved[1]["green"], saved[1]["blue"],
    saved[2]["red"], saved[2]["green"], saved[2]["blue"]))
'
mqtt_pub /devices/dmxwb_group_1/controls/color/on '0;255;0'
wait_state_field 1 green 255
mqtt_pub /dmxwb/scenes/1/apply '{"request_id":"dev013b-apply-2"}'
wait_state_field 1 blue 255
wait_state_field 2 red 255
if ! ask_yes_no "Перезаписанная Scene вернула физический Fixture A в СИНИЙ?"; then exit 1; fi

mqtt_pub /dmxwb/scenes/1/rename '{"request_id":"dev013b-rename-1","name":"DEV013B Scene Renamed"}'
wait_topic /devices/dmxwb_scene_1/controls/name 'DEV013B Scene Renamed'
mqtt_pub /dmxwb/scenes/1/delete '{"request_id":"dev013b-delete-1"}'
wait_config_probe 0 'import json,sys; print(len(json.load(sys.stdin)["scenes"]))'
mqtt_pub /dmxwb/scenes/create '{"request_id":"dev013b-create-2","name":"DEV013B Persistent Scene"}'
wait_topic /devices/dmxwb_scene_2/controls/name 'DEV013B Persistent Scene'
record "scene_full_lifecycle_stable_id_and_physical_apply: PASS"

echo "=== Names and persistence across service restart ==="
mqtt_pub /devices/dmxwb_fixture_1/controls/name/on 'DEV013B Fixture A Final'
mqtt_pub /devices/dmxwb_fixture_2/controls/name/on 'DEV013B Fixture B Final'
mqtt_pub /devices/dmxwb_group_1/controls/name/on 'DEV013B Pair Final'
wait_topic /devices/dmxwb_fixture_1/controls/name 'DEV013B Fixture A Final'
wait_topic /devices/dmxwb_fixture_2/controls/name 'DEV013B Fixture B Final'
wait_topic /devices/dmxwb_group_1/controls/name 'DEV013B Pair Final'
mqtt_pub /devices/dmxwb_fixture_1/controls/color/on '255;0;0'
mqtt_pub /devices/dmxwb_fixture_1/controls/brightness/on 50
mqtt_pub /devices/dmxwb_fixture_1/controls/power/on 1
wait_state_field 1 red 255
wait_state_field 1 brightness 50
sleep 3

CONFIG_SHA_BEFORE="$(remote "sha256sum /etc/dmxwb/config.json | awk '{print \$1}'" | tr -d '\r\n')"
STATE_SHA_BEFORE="$(remote "sha256sum /var/lib/dmxwb/state.json | awk '{print \$1}'" | tr -d '\r\n')"
PID_BEFORE_RESTART="$(service_pid)"
remote "systemctl restart dmxwb.service"
PID_AFTER_RESTART="$(wait_service_active)"
if [[ "${PID_BEFORE_RESTART}" == "${PID_AFTER_RESTART}" ]]; then echo "PID не изменился после restart." >&2; exit 1; fi
wait_topic /devices/dmxwb/controls/status running 120
wait_topic /devices/dmxwb/controls/source mqtt 120
remote "test \"\$(sha256sum /etc/dmxwb/config.json | awk '{print \$1}')\" = '${CONFIG_SHA_BEFORE}'; test \"\$(sha256sum /var/lib/dmxwb/state.json | awk '{print \$1}')\" = '${STATE_SHA_BEFORE}'"
wait_topic /devices/dmxwb_fixture_1/controls/name 'DEV013B Fixture A Final'
wait_topic /devices/dmxwb_group_1/controls/name 'DEV013B Pair Final'
wait_topic /devices/dmxwb_scene_2/controls/name 'DEV013B Persistent Scene'
wait_state_field 1 red 255
wait_state_field 1 brightness 50
wait_state_field 1 requested_power true
if ! ask_yes_no "После service restart физический Fixture A восстановил приглушённый КРАСНЫЙ?"; then exit 1; fi
record "config_state_names_group_scene_restart_restore: PASS (${PID_BEFORE_RESTART} -> ${PID_AFTER_RESTART})"

mqtt_pub /devices/dmxwb_group_1/controls/power/on 0
wait_topic /devices/dmxwb_group_1/controls/power 0
if ! ask_yes_no "Перед завершением физический Fixture A полностью выключен?"; then exit 1; fi
remote "systemctl stop dmxwb.service"
serial_released
record "final_power_off_clean_stop_port_release: PASS"

restore_environment
record "original_environment_restored: PASS"
record "dev013b_result: PASS"
record "=== DMXWB DEV-013B INTEGRATED FIXTURE/GROUP/SCENE ACCEPTANCE PASS ==="

echo "=== DMXWB DEV-013B INTEGRATED FIXTURE/GROUP/SCENE ACCEPTANCE PASS ==="
echo "Report: ${REPORT}"
remote "rm -rf -- '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf -- "${LOCAL_TMP}"
CLEANED=1
trap - EXIT INT TERM
