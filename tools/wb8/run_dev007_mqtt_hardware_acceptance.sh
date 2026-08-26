#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev007_mqtt_hardware_acceptance.sh user@wb8-host [start-address]

DEV-007 MQTT + Fixture hardware acceptance:
  - real local Mosquitto 127.0.0.1:1883;
  - retained /on rejection;
  - MQTT -> Controller -> Fixture -> DmxOutput -> RS-485;
  - factual retained state;
  - broker stop/start without process restart;
  - full republish after reconnect;
  - LWT status=off;
  - final all-off.

На текущем стенде /dev/ttyRS485-1 считается постоянно отключённым
в WB Serial Device Driver Configuration. wb-mqtt-serial не останавливается.
Ответы на визуальные вопросы: только латинские y или n.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
START_ADDRESS="${2:-1}"
PORT="/dev/ttyRS485-1"

if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "start-address должен быть целым числом 1..297 для одного RGBW Fixture." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-mqtt-acceptance"
DIAG_BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV007_MQTT_HARDWARE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev007-mqtt"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev007-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

for command_name in ssh scp sha256sum awk grep git; do
    command -v "${command_name}" >/dev/null 2>&1 || { echo "Не найдена команда: ${command_name}" >&2; exit 1; }
done

if [[ ! -x "${BINARY}" || ! -x "${DIAG_BINARY}" ]]; then
    echo "Не найдены ARM64 artifacts DEV-007." >&2
    echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")
SCP_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")

SSH_OPENED=0
REMOTE_READY=0
RUNTIME_PID=""
BROKER_WAS_ACTIVE=0
TEST_COMPLETE=0

remote() { ssh "${SSH_OPTS[@]}" "${TARGET}" "$@"; }
record() { printf '%s\n' "$*" | tee -a "${REPORT}"; }

ask_yes_no() {
    local prompt="$1" answer
    while true; do
        read -r -p "${prompt} (y/n): " answer
        case "${answer}" in
            y) return 0 ;;
            n) return 1 ;;
            *) echo "Введите только латинскую y или n." ;;
        esac
    done
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if (( SSH_OPENED == 1 )); then
        if [[ -n "${RUNTIME_PID}" ]]; then
            remote "kill -TERM '${RUNTIME_PID}' 2>/dev/null || true; sleep 1; kill -KILL '${RUNTIME_PID}' 2>/dev/null || true" >/dev/null 2>&1 || true
        fi
        if (( BROKER_WAS_ACTIVE == 1 )); then
            remote "systemctl start mosquitto >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
        fi
        if (( REMOTE_READY == 1 && TEST_COMPLETE == 0 )); then
            remote "'${REMOTE_DIR}/dmxwb' --fixture-hardware-test all-off --port '${PORT}' --start-address '${START_ADDRESS}' --seconds 1" >/dev/null 2>&1 || true
        fi
        remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi
    rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}" >/dev/null 2>&1 || true
    exit "${status}"
}
trap cleanup EXIT INT TERM

mqtt_get() {
    local topic="$1"
    # -C 1 already limits mosquitto_sub to one MQTT message. Do not use
    # tail/head here: canonical /dmxwb/config and /dmxwb/state payloads are
    # intentionally multi-line JSON, and line filtering would corrupt them.
    remote "timeout 8 mosquitto_sub -h 127.0.0.1 -p 1883 -t '${topic}' -C 1 2>/dev/null" | tr -d '\r'
}

mqtt_pub() {
    local topic="$1" payload="$2"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -m '${payload}'"
}

mqtt_pub_retained() {
    local topic="$1" payload="$2"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -m '${payload}' -r"
}

mqtt_clear_retained() {
    local topic="$1"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -r -n" >/dev/null
}

wait_topic() {
    local topic="$1" expected="$2" attempts="${3:-12}" value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 1
    done
    echo "Ожидался ${topic}=${expected}, получено '${value}'" >&2
    return 1
}

start_runtime() {
    local log_name="$1"
    remote "rm -f '${REMOTE_DIR}/${log_name}'; '${REMOTE_DIR}/dmxwb-mqtt-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' >'${REMOTE_DIR}/${log_name}' 2>&1 & echo \$!" | tr -d '\r'
}

stop_runtime_gracefully() {
    local pid="$1"
    remote "kill -TERM '${pid}'; for i in \$(seq 1 50); do kill -0 '${pid}' 2>/dev/null || exit 0; sleep 0.1; done; exit 1"
}

diag_value() {
    local output="$1" key="$2"
    awk -F': ' -v key="${key}" '$1 == key {print $2}' <<<"${output}" | tail -n1 | tr -d '\r'
}

is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":0},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV007 Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON
cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-007 MQTT + Fixture hardware acceptance ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-007 MQTT hardware acceptance"
echo "Контроллер: ${TARGET}"
echo "Порт: ${PORT}"
echo "Fixture Start Address: ${START_ADDRESS}"
echo "В тесте Mosquitto будет кратковременно stop/start для проверки reconnect."
echo "wb-mqtt-serial не останавливается; ${PORT} считается освобождённым."
echo
if ! ask_yes_no "Светильник подключён и краткий restart локального Mosquitto допустим?"; then
    exit 2
fi

echo "Открываем одно SSH-соединение. Пароль потребуется один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
if [[ "$(remote "systemctl is-active mosquitto 2>/dev/null || true" | tr -d '\r')" != "active" ]]; then
    echo "Mosquitto service на target не active." >&2
    exit 1
fi
BROKER_WAS_ACTIVE=1

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${DIAG_BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/"
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-mqtt-acceptance' '${REMOTE_DIR}/dmxwb'"
REMOTE_READY=1

record ""
record "--- target identity ---"
remote "printf 'model: '; tr -d '\0' </proc/device-tree/model 2>/dev/null || true; echo; uname -a; if [ -r /etc/wb-release ]; then cat /etc/wb-release; fi; printf 'libmosquitto: '; ldconfig -p 2>/dev/null | grep -m1 libmosquitto.so.1 || true" | tee -a "${REPORT}"

# Retained commands must not restore the model.
mqtt_pub_retained "/devices/dmxwb/controls/source/on" "artnet"
mqtt_pub_retained "/devices/dmxwb_fixture_1/controls/power/on" "1"

RUNTIME_PID="$(start_runtime run-a.log)"
record "run_a_pid: ${RUNTIME_PID}"
wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 10 >/dev/null
record "retained_source_command_ignored: PASS"
record "retained_fixture_command_ignored: PASS"
mqtt_clear_retained "/devices/dmxwb/controls/source/on"
mqtt_clear_retained "/devices/dmxwb_fixture_1/controls/power/on"

# Real MQTT -> Fixture -> DMX.
mqtt_pub "/devices/dmxwb_fixture_1/controls/color/on" "255;0;0"
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 10 >/dev/null
if ask_yes_no "Светильник физически КРАСНЫЙ?"; then record "red_user_observation: PASS"; else record "red_user_observation: FAIL"; exit 1; fi

mqtt_pub "/devices/dmxwb_fixture_1/controls/brightness/on" "50"
wait_topic "/devices/dmxwb_fixture_1/controls/red" "127" 10 >/dev/null
if ask_yes_no "Красный свет стал примерно 50% яркости?"; then record "brightness_user_observation: PASS"; else record "brightness_user_observation: FAIL"; exit 1; fi

mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 10 >/dev/null
STATE_PAYLOAD="$(mqtt_get "/dmxwb/state")"
STATE_COMPACT="$(printf '%s' "${STATE_PAYLOAD}" | tr -d '[:space:]')"
if [[ "${STATE_COMPACT}" != *'"requested_power":false'* || "${STATE_COMPACT}" != *'"red":255'* || "${STATE_COMPACT}" != *'"brightness":50'* ]]; then
    record "saved_state_while_off: FAIL"
    echo "Некорректный /dmxwb/state после Power OFF: ${STATE_PAYLOAD}" >&2
    exit 1
fi
record "saved_state_while_off: PASS"
if ask_yes_no "Power OFF физически полностью выключил светильник?"; then record "power_off_user_observation: PASS"; else exit 1; fi

mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/red" "127" 10 >/dev/null
if ask_yes_no "Power ON восстановил красный примерно 50%?"; then record "power_restore_user_observation: PASS"; else exit 1; fi

# Prepare a stable blue state and allow debounce save.
mqtt_pub "/devices/dmxwb_fixture_1/controls/brightness/on" "100"
mqtt_pub "/devices/dmxwb_fixture_1/controls/color/on" "0;0;255"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 10 >/dev/null
sleep 3
if ask_yes_no "Перед reconnect-test светильник стабильно СИНИЙ?"; then record "blue_user_observation: PASS"; else exit 1; fi

# Poison retained state (not command), then force broker reconnect. Full republish must repair it.
mqtt_pub_retained "/devices/dmxwb_fixture_1/controls/color" "9;9;9"
mqtt_pub_retained "/dmxwb/state" "stale-marker"
remote "systemctl stop mosquitto"
sleep 4
if ! remote "kill -0 '${RUNTIME_PID}'"; then
    record "process_survives_broker_down: FAIL"
    exit 1
fi
record "process_survives_broker_down: PASS"
if ask_yes_no "Пока Mosquitto был остановлен, синий DMX свет не погас и не моргал?"; then record "dmx_continues_broker_down_user: PASS"; else exit 1; fi
remote "systemctl start mosquitto"
wait_topic "/devices/dmxwb/controls/status" "running" 30 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 20 >/dev/null
STATE_PAYLOAD="$(mqtt_get "/dmxwb/state")"
STATE_COMPACT="$(printf '%s' "${STATE_PAYLOAD}" | tr -d '[:space:]')"
if [[ "${STATE_COMPACT}" == "stale-marker" || "${STATE_COMPACT}" != *'"blue":255'* ]]; then
    record "full_republish_after_reconnect: FAIL"
    exit 1
fi
record "full_republish_after_reconnect: PASS"

# Graceful stop Run A and inspect diagnostics.
stop_runtime_gracefully "${RUNTIME_PID}"
RUNTIME_PID=""
RUN_A_LOG="$(remote "cat '${REMOTE_DIR}/run-a.log'")"
printf '%s\n' "${RUN_A_LOG}" | tee -a "${REPORT}"
connections="$(diag_value "${RUN_A_LOG}" mqtt_successful_connections)"
disconnects="$(diag_value "${RUN_A_LOG}" mqtt_disconnects)"
ignored="$(diag_value "${RUN_A_LOG}" mqtt_commands_ignored)"
republishes="$(diag_value "${RUN_A_LOG}" runtime_full_republishes)"
frames="$(diag_value "${RUN_A_LOG}" dmx_frames_sent)"
missed="$(diag_value "${RUN_A_LOG}" dmx_missed_deadlines)"
send_failures="$(diag_value "${RUN_A_LOG}" dmx_send_failures)"
software="$(diag_value "${RUN_A_LOG}" software_result)"
if ! is_uint "${connections}" || ! is_uint "${disconnects}" || ! is_uint "${ignored}" || ! is_uint "${republishes}" || ! is_uint "${frames}" || ! is_uint "${missed}" || ! is_uint "${send_failures}" || \
   (( connections < 2 || disconnects < 1 || ignored < 2 || republishes < 2 || frames < 200 || missed != 0 || send_failures != 0 )) || [[ "${software}" != "PASS" ]]; then
    record "run_a_software_acceptance: FAIL"
    exit 1
fi
record "run_a_software_acceptance: PASS"
wait_topic "/devices/dmxwb/controls/status" "off" 10 >/dev/null
record "graceful_off_status: PASS"

# Unexpected process death must produce broker LWT=off.
RUNTIME_PID="$(start_runtime run-b.log)"
wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
remote "kill -KILL '${RUNTIME_PID}'"
RUNTIME_PID=""
wait_topic "/devices/dmxwb/controls/status" "off" 15 >/dev/null
record "mqtt_lwt_off: PASS"

# Final all-off through the real MQTT runtime, then graceful shutdown.
RUNTIME_PID="$(start_runtime run-c.log)"
wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 10 >/dev/null
if ask_yes_no "FINAL: светильник полностью выключен?"; then record "final_all_off_user_observation: PASS"; else exit 1; fi
sleep 3
stop_runtime_gracefully "${RUNTIME_PID}"
RUNTIME_PID=""
RUN_C_LOG="$(remote "cat '${REMOTE_DIR}/run-c.log'")"
printf '%s\n' "${RUN_C_LOG}" | tee -a "${REPORT}"
if [[ "$(diag_value "${RUN_C_LOG}" software_result)" != "PASS" ]]; then
    record "final_run_software_acceptance: FAIL"
    exit 1
fi
record "final_run_software_acceptance: PASS"
wait_topic "/devices/dmxwb/controls/status" "off" 10 >/dev/null

TEST_COMPLETE=1
record "dev007_mqtt_hardware_result: PASS"
record "=== DMXWB DEV-007 MQTT + FIXTURE HARDWARE PASS ==="
echo
echo "PASS: MQTT loss/recovery не остановил continuous DMX, Fixture contract подтверждён физически."
echo "Отчёт: ${REPORT}"
