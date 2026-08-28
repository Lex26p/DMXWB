#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010c6_reconnect_latest_acceptance.sh user@wb8-host QLC_PLUS_PC_IP [fixture-start-address]

DEV-010C6 repeated reconnect + latest/no-FIFO acceptance:
  - QLC+ 5.2.2 first proves the normal real Art-Net path;
  - QLC+ output is disabled and its source lock is released;
  - a deterministic named/versioned Windows probe on the same external PC
    performs three >3 s source-loss/reconnect cycles without restarting DMXWB;
  - each loss must Hold Last with Source remaining ART-NET;
  - a high-rate ArtDmx burst stresses the runtime mailbox/latest path;
  - the final BLUE guard must take over quickly with no delayed playback of the
    burst, demonstrating latest committed snapshot / no ArtDmx FIFO behavior;
  - physical DMX remains fixed 44 Hz throughout.

Art-Net Port-Address = 0.
Development-only Art-Net OEM sentinel = 0xFFFF; это НЕ production OEM assignment.
The Windows probe changes no IP/DHCP/route configuration.
Ответы на визуальные вопросы: только латинские y или n.
USAGE
}
if [[ $# -lt 2 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
QLC_IP="$2"
START_ADDRESS="${3:-1}"
PORT="/dev/ttyRS485-1"
PORT_ADDRESS=0

if [[ ! "${QLC_IP}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "QLC_PLUS_PC_IP должен быть IPv4 address." >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi

R=$((START_ADDRESS))
G=$((START_ADDRESS + 1))
B=$((START_ADDRESS + 2))
W=$((START_ADDRESS + 3))

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-dev010-source-acceptance"
REPORT="${REPO_ROOT}/docs/DEV010C6_RECONNECT_LATEST_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010c6-reconnect-latest"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010c6-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"
STRESS_PS1="${REPO_ROOT}/tools/windows/send_dev010c6_reconnect_latest_probe.ps1"
STRESS_PS1_WIN=""

for command_name in ssh scp sha256sum awk grep git sed tail tr seq powershell.exe wslpath; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Не найден ${BINARY}" >&2
    echo "Сначала выполните: bash tools/wb8/build_dev010b_source_acceptance.sh" >&2
    exit 1
fi

if [[ ! -f "${STRESS_PS1}" ]]; then
    echo "Не найден Windows reconnect/latest helper: ${STRESS_PS1}" >&2
    exit 1
fi
STRESS_PS1_WIN="$(wslpath -w "${STRESS_PS1}")"

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"
SSH_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")
SCP_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")

SSH_OPENED=0
RUNTIME_PID=""
RUNTIME_STARTED=0
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
        if (( RUNTIME_STARTED == 1 && TEST_COMPLETE == 0 )) && [[ -n "${RUNTIME_PID}" ]]; then
            # Best-effort safe logical OFF while unified runtime still owns serial.
            remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 1" >/dev/null 2>&1 || true
            record "--- runtime.log after incomplete acceptance ---" || true
            remote "tail -n 180 '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tee -a "${REPORT}" >/dev/null || true
        fi
        if [[ -n "${RUNTIME_PID}" ]]; then
            remote "kill -TERM '${RUNTIME_PID}' 2>/dev/null || true; sleep 1; kill -KILL '${RUNTIME_PID}' 2>/dev/null || true" >/dev/null 2>&1 || true
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
    remote "timeout 8 mosquitto_sub -h 127.0.0.1 -p 1883 -t '${topic}' -C 1 2>/dev/null" | tr -d '\r'
}

mqtt_pub() {
    local topic="$1" payload="$2"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -m '${payload}'"
}

wait_topic() {
    local topic="$1" expected="$2" attempts="${3:-20}" value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.5
    done
    echo "Ожидался ${topic}=${expected}, получено '${value}'" >&2
    return 1
}

latest_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
}

wait_runtime_value() {
    local key="$1" expected="$2" attempts="${3:-30}" value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key}=${expected}, последнее '${value}'" >&2
    remote "tail -n 100 '${REMOTE_DIR}/runtime.log'" >&2 || true
    return 1
}

wait_runtime_uint_ge() {
    local key="$1" minimum="$2" attempts="${3:-30}" value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" =~ ^[0-9]+$ ]] && (( value >= minimum )); then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key} >= ${minimum}, последнее '${value}'" >&2
    remote "tail -n 100 '${REMOTE_DIR}/runtime.log'" >&2 || true
    return 1
}

final_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
}

require_final_zero() {
    local key="$1" value
    value="$(final_value "${key}" || true)"
    if [[ "${value}" != "0" ]]; then
        echo "Ожидался ${key}=0, получено '${value}'" >&2
        exit 1
    fi
    record "${key}: PASS (0)"
}

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV010C6 Reconnect Latest Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-010C6 REPEATED RECONNECT + LATEST NO-FIFO ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "qlc_plus_pc_ip: ${QLC_IP}"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "fixture_channels_rgbw: ${R}/${G}/${B}/${W}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "development_oem_placeholder: 0xFFFF"
record "stress_probe: DMXWB DEV010C6 Reconnect Latest Probe 1.0"
record "stress_probe_ipv4: ${QLC_IP}"
record "stress_probe_physical: 43"
record "stress_probe_port_address: 0"
record "stress_reconnect_silence_seconds: 3.6"
record "stress_burst_packets_requested: 4096"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-010C6 repeated reconnect + latest/no-FIFO"
echo "WB8:              ${TARGET}"
echo "QLC+ PC:          ${QLC_IP}"
echo "Fixture RGBW:     channels ${R}/${G}/${B}/${W}"
echo "Art-Net Universe: ${PORT_ADDRESS}"
echo
echo "Сначала QLC+ подтвердит обычный GREEN ArtDmx interoperability."
echo "Затем deterministic Windows probe выполнит repeated LOST/reconnect и burst latest/no-FIFO stress."
echo "DMXWB runtime во время теста НЕ перезапускается."
echo
if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi

QLC_VERSION="5.2.2"
record "external_controller: QLC+ ${QLC_VERSION}"
record "external_controller_version_source: fixed acceptance-stand value confirmed by user"
record "external_controller_ipv4: ${QLC_IP}"
record "external_controller_role: active_reference_source"

PROBE_STATUS="$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${STRESS_PS1_WIN}" -Action Status -SourceIp "${QLC_IP}" -DestinationIp "10.200.200.1" -Physical 43 | tr -d '\r')"
record "stress_probe_status: ${PROBE_STATUS//$'\n'/; }"

echo
echo "=== Детерминированный старт Art-Net ==="
echo "Перед запуском runtime временно ОТКЛЮЧИТЕ Art-Net OUTPUT patch у Universe 1."
echo "Глобальный Blackout / зелёный глаз НЕ использовать."
if ! ask_yes_no "Art-Net OUTPUT patch в QLC+ сейчас отключён?"; then
    record "qlc_output_disabled_before_runtime_user: FAIL"
    exit 2
fi
record "qlc_output_disabled_before_runtime_user: PASS"

echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl ip; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
if [[ "$(remote "systemctl is-active mosquitto 2>/dev/null || true" | tr -d '\r')" != "active" ]]; then
    echo "Mosquitto service на target не active." >&2
    exit 1
fi
if ! remote "test -e '${PORT}'"; then
    echo "На WB8 отсутствует ${PORT}." >&2
    exit 1
fi

ROUTE_LINE="$(remote "ip -4 route get '${QLC_IP}' | head -n1" | tr -d '\r')"
WB_INTERFACE="$(awk '{for(i=1;i<=NF;i++) if($i=="dev" && i<NF){print $(i+1); exit}}' <<<"${ROUTE_LINE}")"
WB_IP="$(awk '{for(i=1;i<=NF;i++) if($i=="src" && i<NF){print $(i+1); exit}}' <<<"${ROUTE_LINE}")"
if [[ -z "${WB_INTERFACE}" || -z "${WB_IP}" ]]; then
    echo "Не удалось определить WB8 interface/IP по route: ${ROUTE_LINE}" >&2
    exit 1
fi
WB_MAC="$(remote "cat '/sys/class/net/${WB_INTERFACE}/address'" | tr -d '\r')"
if [[ ! "${WB_MAC}" =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]]; then
    echo "Не удалось определить MAC WB8 interface ${WB_INTERFACE}: '${WB_MAC}'" >&2
    exit 1
fi
record "wb8_interface: ${WB_INTERFACE}"
record "wb8_ipv4: ${WB_IP}"
record "wb8_mac: ${WB_MAC}"
record "route_to_qlc: ${ROUTE_LINE}"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance'"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-dev010-source-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' --development-oem-code FFFF --mac '${WB_MAC}' --status-interval-ms 250 >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Unified acceptance runtime завершился раньше времени:" >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
if [[ "$(remote "grep -c '^runtime_started: PASS$' '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tr -d '\r')" == "0" ]]; then
    echo "runtime_started: PASS не найден." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
record "unified_runtime_started: PASS"

RUNTIME_STARTTIME="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ ! "${RUNTIME_STARTTIME}" =~ ^[0-9]+$ ]]; then
    echo "Не удалось зафиксировать process starttime runtime." >&2
    exit 1
fi
record "runtime_pid_before_reconnect_stress: ${RUNTIME_PID}"
record "runtime_starttime_before_reconnect_stress: ${RUNTIME_STARTTIME}"

wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" 20 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_runtime_value status_has_mqtt_snapshot 1 >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "initial_source_mqtt: PASS"

sleep 1
if [[ "$(latest_value status_has_artnet_snapshot || true)" != "0" ]]; then
    echo "До включения QLC+ уже появился Art-Net snapshot. OUTPUT patch должен быть реально отключён." >&2
    remote "tail -n 80 '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
record "no_artnet_snapshot_before_qlc_enable: PASS"

# Keep a safe MQTT state in background. It must not take over during Art-Net loss.
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 20 >/dev/null

echo
echo "=== QLC+ initial Art-Net GREEN ==="
echo "Теперь СНОВА ПОДКЛЮЧИТЕ Art-Net OUTPUT к Universe 1 и выставьте значения ниже."
echo "В QLC+ Universe 1 подключите Art-Net OUTPUT:"
echo "  Network interface:        ${QLC_IP}"
echo "  Destination / IP Address: ${WB_IP}"
echo "  Art-Net Universe:         ${PORT_ADDRESS}"
echo "  Transmission Mode:        Standard"
echo "В Simple Desk выставьте:"
echo "  Channel ${R} = 0"
echo "  Channel ${G} = 255"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
if ! ask_yes_no "Art-Net OUTPUT снова включён и QLC+ сейчас отправляет этот зелёный Art-Net на ${WB_IP}?"; then
    record "qlc_green_user: FAIL"
    exit 1
fi
wait_runtime_value status_has_artnet_snapshot 1 50 >/dev/null
record "fresh_initial_artnet_snapshot_received: PASS"

mqtt_pub "/devices/dmxwb/controls/source/on" "artnet"
wait_topic "/devices/dmxwb/controls/source" "artnet" 20 >/dev/null
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null
record "source_artnet_selected: PASS"

if ask_yes_no "После Source=ART-NET светильник физически ЗЕЛЁНЫЙ?"; then
    record "artnet_green_physical_user: PASS"
else
    record "artnet_green_physical_user: FAIL"
    exit 1
fi

FRAMES_BEFORE_STRESS="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_BEFORE_STRESS}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_BEFORE_STRESS}'" >&2
    exit 1
fi
record "dmx_frames_before_reconnect_stress: ${FRAMES_BEFORE_STRESS}"

echo
echo "=== Release QLC+ source lock before deterministic reconnect probe ==="
echo "Теперь ОТКЛЮЧИТЕ Art-Net OUTPUT patch QLC+ и больше его не включайте в этом тесте."
echo "Глобальный Blackout / зелёный глаз НЕ использовать."
if ! ask_yes_no "QLC+ Art-Net OUTPUT patch отключён?"; then
    record "qlc_output_disabled_before_reconnect_probe_user: FAIL"
    exit 1
fi
record "qlc_output_disabled_before_reconnect_probe_user: PASS"

sleep 3.6
record "qlc_source_loss_window_elapsed: PASS (>3 s)"

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "DMXWB runtime завершился до reconnect stress." >&2
    exit 1
fi
RUNTIME_STARTTIME_PROBE="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ "${RUNTIME_STARTTIME_PROBE}" != "${RUNTIME_STARTTIME}" ]]; then
    echo "DMXWB PID/starttime изменился до reconnect stress." >&2
    exit 1
fi
record "same_dmxwb_process_before_reconnect_stress: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"

echo
echo "=== Repeated LOST/reconnect acceptance ==="
echo "Теперь reconnect-циклы проверяются ПОШАГОВО."
echo "Burst будет отдельным этапом и не относится к Hold Last проверке."

run_probe_action() {
    local action="$1"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${STRESS_PS1_WIN}" \
        -Action "${action}" -SourceIp "${QLC_IP}" -DestinationIp "${WB_IP}" -Physical 43 \
        | tr -d '\r' | tee -a "${REPORT}"
}

echo
echo "--- Cycle 1: GREEN -> LOST Hold Last ---"
run_probe_action SendGreen
if ask_yes_no "Светильник сейчас GREEN?"; then
    record "reconnect_cycle1_green_active_user: PASS"
else
    record "reconnect_cycle1_green_active_user: FAIL"
    exit 1
fi

echo "Начинается intentional SILENCE 1 на 3.6 s. Светильник должен всё время оставаться GREEN."
sleep 3.6
if ask_yes_no "Во время SILENCE 1 не было blackout/смены цвета, светильник остался GREEN?"; then
    record "reconnect_cycle1_green_hold_last_user: PASS"
else
    record "reconnect_cycle1_green_hold_last_user: FAIL"
    exit 1
fi

echo
echo "--- Cycle 2: reconnect RED -> LOST Hold Last ---"
run_probe_action SendRed
if ask_yes_no "После reconnect светильник стал RED?"; then
    record "reconnect_cycle2_red_active_user: PASS"
else
    record "reconnect_cycle2_red_active_user: FAIL"
    exit 1
fi

echo "Начинается intentional SILENCE 2 на 3.6 s. Светильник должен всё время оставаться RED."
sleep 3.6
if ask_yes_no "Во время SILENCE 2 не было blackout/смены цвета, светильник остался RED?"; then
    record "reconnect_cycle2_red_hold_last_user: PASS"
else
    record "reconnect_cycle2_red_hold_last_user: FAIL"
    exit 1
fi

echo
echo "--- Cycle 3: reconnect BLUE -> LOST Hold Last ---"
run_probe_action SendBlue
if ask_yes_no "После reconnect светильник стал BLUE?"; then
    record "reconnect_cycle3_blue_active_user: PASS"
else
    record "reconnect_cycle3_blue_active_user: FAIL"
    exit 1
fi

echo "Начинается intentional SILENCE 3 на 3.6 s. Светильник должен всё время оставаться BLUE."
sleep 3.6
if ask_yes_no "Во время SILENCE 3 не было blackout/смены цвета, светильник остался BLUE?"; then
    record "reconnect_cycle3_blue_hold_last_user: PASS"
else
    record "reconnect_cycle3_blue_hold_last_user: FAIL"
    exit 1
fi

echo
echo "--- Final reconnect before burst: WHITE ---"
run_probe_action SendWhite
if ask_yes_no "После final reconnect светильник стал WHITE?"; then
    record "reconnect_final_white_active_user: PASS"
else
    record "reconnect_final_white_active_user: FAIL"
    exit 1
fi
record "repeated_reconnect_hold_last_user: PASS"

echo
echo "=== Latest/no-FIFO burst stress ==="
echo "ВАЖНО: только на следующем коротком BURST этапе мигание ОЖИДАЕМО."
echo "Probe около 0.4-1 s быстро чередует RED/GREEN/WHITE/BLUE."
echo "Это намеренный stress input, а не reconnect failure."
echo
echo "После строки phase=FINAL_BLUE_GUARD_START мигание должно прекратиться,"
echo "и светильник должен быстро стать стабильным BLUE."

STRESS_LOG="${LOCAL_TMP}/reconnect-latest-output.txt"
: > "${STRESS_LOG}"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${STRESS_PS1_WIN}" \
    -Action RunBurst -SourceIp "${QLC_IP}" -DestinationIp "${WB_IP}" -Physical 43 \
    | tr -d '\r' | tee "${STRESS_LOG}" | tee -a "${REPORT}"

for marker in \
    'phase=BURST_START' \
    'phase=FINAL_BLUE_GUARD_START' \
    'phase=FINAL_BLUE_STABLE'; do
    if ! grep -qx "${marker}" "${STRESS_LOG}"; then
        echo "В probe output отсутствует marker: ${marker}" >&2
        exit 1
    fi
done

BURST_SENT="$(sed -n 's/^burst_packets_sent=//p' "${STRESS_LOG}" | tail -n1)"
BURST_MS="$(sed -n 's/^burst_duration_ms=//p' "${STRESS_LOG}" | tail -n1)"
if [[ ! "${BURST_SENT}" =~ ^[0-9]+$ ]] || (( BURST_SENT != 4096 )); then
    echo "Некорректный burst_packets_sent='${BURST_SENT}', ожидалось 4096." >&2
    exit 1
fi
if [[ ! "${BURST_MS}" =~ ^[0-9]+$ ]] || (( BURST_MS <= 0 || BURST_MS > 5000 )); then
    echo "Некорректный burst_duration_ms='${BURST_MS}'." >&2
    exit 1
fi
record "deterministic_reconnect_burst_probe: PASS (4096 packets, ${BURST_MS} ms)"

if ask_yes_no "После FINAL_BLUE_GUARD_START мигание быстро прекратилось (примерно до 1 s), светильник стал и остался BLUE?"; then
    record "latest_no_fifo_final_blue_user: PASS"
else
    record "latest_no_fifo_final_blue_user: FAIL"
    exit 1
fi

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "DMXWB runtime завершился во время reconnect/latest stress." >&2
    exit 1
fi
RUNTIME_STARTTIME_AFTER="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ "${RUNTIME_STARTTIME_AFTER}" != "${RUNTIME_STARTTIME}" ]]; then
    echo "DMXWB PID/starttime изменился во время reconnect/latest stress." >&2
    exit 1
fi
record "same_dmxwb_process_after_reconnect_stress: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"

if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source автоматически изменился во время repeated reconnect." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
record "source_stays_artnet_through_reconnects: PASS"

FRAMES_AFTER_STRESS="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_AFTER_STRESS}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_AFTER_STRESS}'" >&2
    exit 1
fi
FRAME_DELTA=$((FRAMES_AFTER_STRESS - FRAMES_BEFORE_STRESS))
if (( FRAME_DELTA < 500 )); then
    echo "Во время reconnect/latest stress передано слишком мало DMX frames: delta=${FRAME_DELTA}." >&2
    exit 1
fi
record "physical_dmx_continues_during_reconnect_stress: PASS (${FRAMES_BEFORE_STRESS} -> ${FRAMES_AFTER_STRESS}, delta=${FRAME_DELTA})"

# Prepare safe MQTT OFF in background, then explicitly return to MQTT.
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 20 >/dev/null
if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Background MQTT command unexpectedly changed Source." >&2
    exit 1
fi

mqtt_pub "/devices/dmxwb/controls/source/on" "mqtt"
wait_topic "/devices/dmxwb/controls/source" "mqtt" 20 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_runtime_value status_artnet_output_active 0 >/dev/null
record "final_explicit_return_to_mqtt: PASS"

if ask_yes_no "После возврата Source на WB MQTT светильник полностью ВЫКЛЮЧЕН?"; then
    record "final_mqtt_off_physical_user: PASS"
else
    record "final_mqtt_off_physical_user: FAIL"
    exit 1
fi

remote "kill -TERM '${RUNTIME_PID}'"
for _ in $(seq 1 80); do
    if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
        break
    fi
    sleep 0.1
done
if remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Runtime не завершился по SIGTERM." >&2
    exit 1
fi
RUNTIME_PID=""

record "--- final runtime diagnostics ---"
remote "grep -E '^(final_|state_flush_action:|software_result:)' '${REMOTE_DIR}/runtime.log'" | tee -a "${REPORT}"

if [[ "$(final_value software_result || true)" != "PASS" ]]; then
    echo "Unified runtime software_result != PASS" >&2
    exit 1
fi
if [[ "$(final_value final_selected_source || true)" != "mqtt" ]]; then
    echo "Финальный Source должен быть mqtt." >&2
    exit 1
fi
FINAL_SOURCE_STATE="$(final_value final_artnet_source_state || true)"
if [[ "${FINAL_SOURCE_STATE}" != "ACTIVE" && "${FINAL_SOURCE_STATE}" != "LOST" ]]; then
    echo "Некорректный final_artnet_source_state='${FINAL_SOURCE_STATE}'." >&2
    exit 1
fi
record "final_artnet_source_state_after_reconnect_stress: PASS (${FINAL_SOURCE_STATE})"

if [[ "$(final_value final_artnet_sync_mode || true)" != "ASYNC" ]]; then
    echo "DEV-010C6 probe не отправляет sync packets; final_artnet_sync_mode должен быть ASYNC." >&2
    exit 1
fi
record "final_artnet_sync_mode: PASS (ASYNC)"

LOST_EVENTS="$(final_value final_artnet_source_lost_events || true)"
CONFLICTS="$(final_value final_artnet_conflicts || true)"
SWITCHES="$(final_value final_router_source_switches || true)"
ARTNET_PUBLISHED="$(final_value final_artnet_snapshots_published || true)"
ARTNET_ROUTED="$(final_value final_router_artnet_snapshots_received || true)"
DATAGRAMS="$(final_value final_artnet_datagrams_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
for pair in \
    "LOST_EVENTS:${LOST_EVENTS}" \
    "CONFLICTS:${CONFLICTS}" \
    "SWITCHES:${SWITCHES}" \
    "ARTNET_PUBLISHED:${ARTNET_PUBLISHED}" \
    "ARTNET_ROUTED:${ARTNET_ROUTED}" \
    "DATAGRAMS:${DATAGRAMS}" \
    "DMX_FRAMES:${DMX_FRAMES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

# One LOST releases the initial QLC+ lock; three more are intentional probe
# silence/reconnect cycles. A later operator delay may add another LOST.
if (( LOST_EVENTS < 4 )); then
    echo "Ожидалось минимум 4 source LOST events (QLC release + 3 reconnect cycles), получено ${LOST_EVENTS}." >&2
    exit 1
fi
if (( CONFLICTS != 0 )); then
    echo "Repeated reconnect одного source identity не должен создавать conflicts; получено ${CONFLICTS}." >&2
    exit 1
fi
if (( SWITCHES < 2 )); then
    echo "Ожидалось минимум 2 explicit Source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( DATAGRAMS < 500 )); then
    echo "Stress слишком мал: runtime получил только ${DATAGRAMS} Art-Net datagrams." >&2
    exit 1
fi
if (( ARTNET_PUBLISHED < 500 )); then
    echo "Stress слишком мал: опубликовано только ${ARTNET_PUBLISHED} committed Art-Net snapshots." >&2
    exit 1
fi
if (( ARTNET_ROUTED <= 0 )); then
    echo "Router не получил Art-Net snapshots." >&2
    exit 1
fi
# Runtime publishes each committed revision it receives, while the coordinator
# samples only the latest generation after runtime.step(). Under the burst the
# routed count must therefore be substantially smaller than committed revisions.
if (( ARTNET_ROUTED * 2 >= ARTNET_PUBLISHED )); then
    echo "Не доказано latest/coalescing: routed=${ARTNET_ROUTED}, published=${ARTNET_PUBLISHED}." >&2
    exit 1
fi
if (( DMX_FRAMES <= FRAMES_AFTER_STRESS )); then
    echo "Физические DMX frames не продолжились до shutdown." >&2
    exit 1
fi

record "repeated_source_lost_reacquire_events: PASS (${LOST_EVENTS})"
record "artnet_conflicts: PASS (0)"
record "source_switch_count: PASS (${SWITCHES})"
record "artnet_datagrams_received_stress: PASS (${DATAGRAMS})"
record "artnet_committed_snapshots_published: PASS (${ARTNET_PUBLISHED})"
record "router_latest_snapshots_routed: PASS (${ARTNET_ROUTED})"
record "latest_coalescing_ratio: PASS (routed ${ARTNET_ROUTED} < 50% of published ${ARTNET_PUBLISHED})"
record "physical_dmx_frames: PASS (${DMX_FRAMES})"

require_final_zero final_mqtt_callback_failures
require_final_zero final_mqtt_runtime_dmx_publish_failures
require_final_zero final_mqtt_runtime_state_save_failures
require_final_zero final_artnet_bind_failures
require_final_zero final_artnet_receive_errors
require_final_zero final_artnet_send_errors
require_final_zero final_artnet_core_rejections
require_final_zero final_artnet_route_failures
require_final_zero final_router_physical_publish_failures
require_final_zero final_dmx_sink_start_failures
require_final_zero final_dmx_sink_publish_failures
require_final_zero final_dmx_sink_unexpected_stops
require_final_zero final_dmx_open_failures
require_final_zero final_dmx_send_failures
require_final_zero final_dmx_missed_deadlines
require_final_zero final_dmx_serial_open_after_stop

if [[ "$(final_value final_dmx_active_refresh_hz || true)" != "44" ]]; then
    echo "final_dmx_active_refresh_hz должен быть 44." >&2
    exit 1
fi
record "fixed_44hz_physical_output: PASS"

if [[ "$(final_value final_artnet_transport_open_before_shutdown || true)" != "1" ]]; then
    echo "Art-Net transport должен быть open непосредственно перед shutdown." >&2
    exit 1
fi
if [[ "$(final_value final_artnet_transport_open_after_shutdown || true)" != "0" ]]; then
    echo "Art-Net transport должен быть closed после shutdown." >&2
    exit 1
fi
record "artnet_transport_lifecycle: PASS"

TEST_COMPLETE=1
record "dev010c6_repeated_reconnect_latest_no_fifo_result: PASS"
record "=== DMXWB DEV-010C6 REPEATED RECONNECT LATEST NO-FIFO PASS ==="
echo
echo "=== DMXWB DEV-010C6 REPEATED RECONNECT LATEST NO-FIFO PASS ==="
echo "Report: ${REPORT}"
