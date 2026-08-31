#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev012a_production_foreground_acceptance.sh SSH_TARGET [fixture-start-address]

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  fixture-start-address = 1

DEV-012A проверяет именно production executable `dmxwb` в foreground:

  config/state -> IntegratedRuntime -> MQTT/Art-Net -> DmxSourceRouter
  -> DmxOutput -> /dev/ttyRS485-1 -> реальный RGBW Fixture

Проверки:
  1. production daemon startup + retained MQTT status;
  2. MQTT RED -> physical RED;
  3. Source ART-NET + ArtDmx BLUE -> physical BLUE;
  4. MQTT GREEN обновляется в background, пока physical остаётся BLUE;
  5. Source WB MQTT -> physical GREEN;
  6. final Power OFF;
  7. SIGTERM -> clean exit + persistent state flush + serial release.

ArtDmx посылается с ноутбука на IPv4 из SSH_TARGET через UDP/6454.
QLC+/другой Art-Net OUTPUT на время теста должен быть полностью отключён.
Ответы на визуальные вопросы: только латинские y или n.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
START_ADDRESS="${2:-1}"
ARTNET_HOST="${TARGET#*@}"
PORT="/dev/ttyRS485-1"
PORT_ADDRESS=0

if [[ ! "${ARTNET_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "SSH_TARGET должен содержать IPv4, например root@10.200.200.1." >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV012A_PRODUCTION_FOREGROUND_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev012a-production"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev012a-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

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

for command_name in ssh scp sha256sum awk grep git sed tail tr seq python3 readelf file; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Не найден production ARM64 binary: ${BINARY}" >&2
    echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
    exit 1
fi

for source_file in \
    "${REPO_ROOT}/src/production_main.cpp" \
    "${REPO_ROOT}/src/integrated_runtime.cpp" \
    "${REPO_ROOT}/include/dmxwb/integrated_runtime.hpp" \
    "${REPO_ROOT}/src/mqtt_runtime.cpp" \
    "${REPO_ROOT}/src/dmx_output.cpp" \
    "${REPO_ROOT}/src/artnet_runtime.cpp"; do
    if [[ "${source_file}" -nt "${BINARY}" ]]; then
        echo "Production binary старее исходника: ${source_file}" >&2
        echo "Повторите: bash tools/wb8/build_bullseye_arm64.sh" >&2
        exit 1
    fi
done

if ! readelf -h "${BINARY}" | grep -q 'Machine:.*AArch64'; then
    echo "Production artifact не AArch64." >&2
    exit 1
fi
if ! readelf -d "${BINARY}" | grep -Fq 'libmosquitto.so.1'; then
    echo "Production artifact не содержит требуемую зависимость libmosquitto.so.1." >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPENED=0
RUNTIME_PID=""
RUNTIME_STARTED=0
RETAINED_CAPTURED=0
TEST_COMPLETE=0
CLEANED=0

record() {
    printf '%s\n' "$*" | tee -a "${REPORT}"
}

ask_yes_no() {
    local prompt="$1"
    local answer
    while true; do
        read -r -p "${prompt} (y/n): " answer
        case "${answer}" in
            y) return 0 ;;
            n) return 1 ;;
            *) echo "Введите только латинскую y или n." ;;
        esac
    done
}

remote() {
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$@"
}

mqtt_get() {
    local topic="$1"
    remote "timeout 5 mosquitto_sub -h 127.0.0.1 -p 1883 -t '${topic}' -C 1 2>/dev/null" | tr -d '\r'
}

wait_topic() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался ${topic}='${expected}', получено '${value}'" >&2
    return 1
}

state_fixture_field() {
    local field="$1"
    mqtt_get "/dmxwb/state" | python3 -c '
import json, sys
field = sys.argv[1]
doc = json.load(sys.stdin)
fixtures = doc.get("fixtures", [])
item = next((x for x in fixtures if x.get("id") == 1), None)
if item is None or field not in item:
    raise SystemExit(2)
value = item[field]
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
' "${field}"
}

wait_state_field() {
    local field="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(state_fixture_field "${field}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался /dmxwb/state fixture 1 ${field}='${expected}', получено '${value}'" >&2
    return 1
}

validate_status_json() {
    mqtt_get "/dmxwb/status" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
required = {"application", "dmx", "mqtt", "artnet", "configuration", "last_error"}
missing = sorted(required.difference(doc))
if missing:
    print("missing=" + ",".join(missing), file=sys.stderr)
    raise SystemExit(2)
print("PASS")
'
}

send_artdmx() {
    local r="$1"
    local g="$2"
    local b="$3"
    local w="$4"
    python3 - "${ARTNET_HOST}" "${PORT_ADDRESS}" "${r}" "${g}" "${b}" "${w}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
values = bytes(int(x) for x in sys.argv[3:7])
sub_uni = port_address & 0xff
net = (port_address >> 8) & 0x7f
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    for seq in range(1, 9):
        packet = (
            b"Art-Net\x00" +
            struct.pack("<H", 0x5000) +
            bytes((0, 14, seq, 0, sub_uni, net)) +
            struct.pack(">H", len(values)) +
            values
        )
        sock.sendto(packet, (host, 6454))
        time.sleep(0.05)
finally:
    sock.close()
PY
}

port_is_released() {
    remote "
for fd in /proc/[0-9]*/fd/*; do
    target=\"\$(readlink \"\${fd}\" 2>/dev/null || true)\"
    if [[ \"\${target}\" == '${PORT}' ]]; then
        printf '%s -> %s\n' \"\${fd}\" \"\${target}\" >&2
        exit 1
    fi
done
exit 0
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

restore_retained() {
    if (( RETAINED_CAPTURED == 0 )); then
        return 0
    fi
    local index=0
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "
set +e
if [[ \"\$(cat '${REMOTE_DIR}/retained-backup/${index}.present' 2>/dev/null)\" == 1 ]]; then
    mosquitto_pub -h 127.0.0.1 -p 1883 -r -t '${topic}' -f '${REMOTE_DIR}/retained-backup/${index}.payload'
else
    mosquitto_pub -h 127.0.0.1 -p 1883 -r -n -t '${topic}'
fi
" >/dev/null
        index=$((index + 1))
    done
    RETAINED_CAPTURED=0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( CLEANED == 0 )); then
        if (( SSH_OPENED == 1 )); then
            if (( RUNTIME_STARTED == 1 )) && [[ -n "${RUNTIME_PID}" ]]; then
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 0.5" >/dev/null 2>&1 || true
                remote "kill -TERM '${RUNTIME_PID}' 2>/dev/null || true; sleep 1; kill -KILL '${RUNTIME_PID}' 2>/dev/null || true" >/dev/null 2>&1 || true
            fi
            restore_retained >/dev/null 2>&1 || true
            remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
            ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
        fi
        rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}" >/dev/null 2>&1 || true
        CLEANED=1
    fi
    exit "${status}"
}
trap cleanup EXIT INT TERM

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV012A Production Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-012A PRODUCTION FOREGROUND ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "artnet_host: ${ARTNET_HOST}"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "fixed_refresh_hz: 44"
record "production_executable: dmxwb"

echo "DMXWB — DEV-012A production foreground acceptance"
echo "WB8:               ${TARGET}"
echo "Production binary: ${BINARY}"
echo "DMX port:          ${PORT}"
echo "Fixture Start:     ${START_ADDRESS}"
echo "Art-Net UDP host:  ${ARTNET_HOST}:6454"
echo

if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    echo "Для DEV-012A нужен контролируемый ArtDmx только от этого helper." >&2
    exit 2
fi

echo
echo "=== WB8 preflight ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl readlink; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto"
if ! remote "test -e '${PORT}'"; then
    echo "На WB8 отсутствует ${PORT}." >&2
    exit 1
fi

RUNNING_DMXWB="$(remote '
for exe in /proc/[0-9]*/exe; do
    target="$(readlink "${exe}" 2>/dev/null || true)"
    name="${target##*/}"
    case "${name}" in
        dmxwb|dmxwb-dev010-source-acceptance|dmxwb-mqtt-acceptance|dmxwb-hardware-diagnostics)
            pid="${exe#/proc/}"
            pid="${pid%/exe}"
            printf "%s %s\n" "${pid}" "${target}"
            ;;
    esac
done
' | tr -d '\r')"
if [[ -n "${RUNNING_DMXWB}" ]]; then
    echo "На WB8 уже запущен DMXWB/acceptance executable. Для DEV-012A он должен быть остановлен:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
record "no_running_dmxwb_process: PASS"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
capture_retained
record "original_retained_topics_captured: PASS"

scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb'"

TARGET_VERSION="$(remote "'${REMOTE_DIR}/dmxwb' --version" | tr -d '\r')"
record "target_version: ${TARGET_VERSION}"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Production dmxwb завершился раньше времени:" >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
if ! remote "grep -Fq 'dmxwb: running' '${REMOTE_DIR}/runtime.log'"; then
    echo "В production log нет 'dmxwb: running'." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "running" 40 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" 40 >/dev/null
if [[ "$(validate_status_json)" != "PASS" ]]; then
    echo "Некорректный retained /dmxwb/status." >&2
    exit 1
fi
record "production_runtime_started: PASS"
record "production_retained_status: PASS"
record "initial_source_mqtt: PASS"

echo
echo "=== 1. MQTT -> physical RED ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/brightness/on' -m '100'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m '1'"
wait_state_field requested_power true >/dev/null
wait_state_field red 255 >/dev/null
wait_state_field green 0 >/dev/null
wait_state_field blue 0 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 40 >/dev/null
if ask_yes_no "Физический светильник стабильно КРАСНЫЙ?"; then
    record "mqtt_red_physical_user: PASS"
else
    record "mqtt_red_physical_user: FAIL"
    exit 1
fi
record "mqtt_red_factual_state: PASS"

echo
echo "=== 2. Source ART-NET + ArtDmx BLUE ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
wait_topic "/devices/dmxwb/controls/source" "artnet" 40 >/dev/null
send_artdmx 0 0 255 0
sleep 1
if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Production dmxwb завершился после ArtDmx." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
if ask_yes_no "Source=ART-NET, физический светильник стабильно СИНИЙ?"; then
    record "artnet_blue_physical_user: PASS"
else
    record "artnet_blue_physical_user: FAIL"
    exit 1
fi
record "artnet_udp_input_production_path: PASS"

echo
echo "=== 3. MQTT background state while ART-NET remains physical ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '0;255;0'"
wait_state_field red 0 >/dev/null
wait_state_field green 255 >/dev/null
wait_state_field blue 0 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "artnet" 40 >/dev/null
if ask_yes_no "MQTT state уже GREEN, но физически светильник всё ещё СИНИЙ?"; then
    record "inactive_mqtt_updates_while_artnet_user: PASS"
else
    record "inactive_mqtt_updates_while_artnet_user: FAIL"
    exit 1
fi
record "inactive_mqtt_state_updated: PASS"

echo
echo "=== 4. ART-NET -> WB MQTT latest GREEN ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt"
wait_topic "/devices/dmxwb/controls/source" "mqtt" 40 >/dev/null
if ask_yes_no "После Source=WB MQTT физический светильник стал текущим ЗЕЛЁНЫМ?"; then
    record "artnet_to_mqtt_green_physical_user: PASS"
else
    record "artnet_to_mqtt_green_physical_user: FAIL"
    exit 1
fi
record "explicit_source_switch_latest_mqtt: PASS"

echo
echo "=== 5. Final Power OFF ==="
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m '0'"
wait_state_field requested_power false >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 40 >/dev/null
if ask_yes_no "Физический светильник полностью выключен?"; then
    record "final_power_off_user: PASS"
else
    record "final_power_off_user: FAIL"
    exit 1
fi

echo
echo "=== 6. SIGTERM / persistent flush / serial release ==="
remote "kill -TERM '${RUNTIME_PID}'"
for _ in $(seq 1 80); do
    if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
        break
    fi
    sleep 0.1
done
if remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Production dmxwb не завершился по SIGTERM." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
RUNTIME_PID=""
RUNTIME_STARTED=0

if ! remote "grep -Fq 'dmxwb: stopped' '${REMOTE_DIR}/runtime.log'"; then
    echo "В production log нет clean shutdown marker 'dmxwb: stopped'." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi

REMOTE_STATE="$(remote "cat '${REMOTE_DIR}/state.json'" | tr -d '\r')"
printf '%s' "${REMOTE_STATE}" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
if doc.get("source") != "mqtt":
    raise SystemExit("final source is not mqtt")
fixtures = doc.get("fixtures", [])
item = next((x for x in fixtures if x.get("id") == 1), None)
if item is None:
    raise SystemExit("fixture 1 absent")
expected = {
    "requested_power": False,
    "red": 0,
    "green": 255,
    "blue": 0,
    "brightness": 100,
}
for key, value in expected.items():
    if item.get(key) != value:
        raise SystemExit(f"unexpected {key}: {item.get(key)!r}")
'
record "sigterm_clean_exit: PASS"
record "persistent_state_flush: PASS"

if ! port_is_released; then
    echo "${PORT} остался открыт после shutdown." >&2
    exit 1
fi
record "serial_port_released_after_shutdown: PASS"

record "--- production runtime log ---"
remote "cat '${REMOTE_DIR}/runtime.log'" | tee -a "${REPORT}"

echo "Восстанавливаем исходные retained topics..."
restore_retained
record "original_retained_topics_restored: PASS"

TEST_COMPLETE=1
record "dev012a_production_foreground_result: PASS"
record "=== DMXWB DEV-012A PRODUCTION FOREGROUND PASS ==="

echo
echo "=== DMXWB DEV-012A PRODUCTION FOREGROUND PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
