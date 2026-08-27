#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010b_artnet_lost_hold_recovery_acceptance.sh user@wb8-host QLC_PLUS_PC_IP [fixture-start-address]

DEV-010B physical Art-Net LOST / Hold Last / recovery acceptance:
  - unified MQTT + Art-Net + Source Router + DmxOutput runtime;
  - explicit Source=ART-NET with real QLC+ ArtDmx;
  - stop real ArtDmx packets for >3 s;
  - application Source remains ART-NET;
  - physical DMX continues at fixed 44 Hz;
  - physical light holds the last Art-Net value (no blackout);
  - same runtime accepts ArtDmx again after QLC+ output returns;
  - no process/operator restart is used.

Art-Net Port-Address = 0.
Development-only Art-Net OEM sentinel = 0xFFFF; это НЕ production OEM assignment.
Для LOST нужно реально снять/отключить Art-Net OUTPUT patch в QLC+.
Глобальный Blackout (зелёный глаз) НЕ использовать: он продолжает передавать ArtDmx.
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
REPORT="${REPO_ROOT}/docs/DEV010B_ARTNET_LOST_HOLD_RECOVERY_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010b-lost-recovery"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010b-lost-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

for command_name in ssh scp sha256sum awk grep git sed tail tr seq; do
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV010B Lost Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-010B ART-NET LOST / HOLD LAST / RECOVERY ==="
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
record "source_loss_timeout_seconds: 3"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-010B Art-Net LOST / Hold Last / recovery"
echo "WB8:              ${TARGET}"
echo "QLC+ PC:          ${QLC_IP}"
echo "Fixture RGBW:     channels ${R}/${G}/${B}/${W}"
echo "Art-Net Universe: ${PORT_ADDRESS}"
echo
echo "Этот тест намеренно прекращает ArtDmx больше чем на 3 секунды."
echo "Runtime DMXWB НЕ перезапускается."
echo
if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi

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

FRAMES_BEFORE_LOSS="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_BEFORE_LOSS}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_BEFORE_LOSS}'" >&2
    exit 1
fi
record "dmx_frames_before_loss: ${FRAMES_BEFORE_LOSS}"

echo
echo "=== Real Art-Net LOST / Hold Last ==="
echo "Сейчас в QLC+ НУЖНО ПРЕКРАТИТЬ ArtDmx:"
echo "  - снимите/удалите Art-Net OUTPUT patch у Universe 1;"
echo "  - НЕ используйте глобальный Blackout / зелёный глаз."
echo "Simple Desk значения 0/255/0/0 можно оставить как есть."
if ! ask_yes_no "Art-Net OUTPUT patch из QLC+ сейчас отключён и ArtDmx больше не отправляется?"; then
    record "qlc_artnet_output_disabled_user: FAIL"
    exit 1
fi

# 3 s is the product source-loss timeout. Give it enough margin and also prove
# that the independent physical 44 Hz loop continued throughout the outage.
sleep 4.5

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Runtime завершился во время Art-Net LOST." >&2
    remote "tail -n 160 '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
record "same_runtime_alive_during_lost: PASS"

if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source автоматически изменился после потери Art-Net; это запрещено." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "source_stays_artnet_during_lost: PASS"

FRAMES_AFTER_LOSS="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_AFTER_LOSS}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_AFTER_LOSS}'" >&2
    exit 1
fi
FRAME_DELTA=$((FRAMES_AFTER_LOSS - FRAMES_BEFORE_LOSS))
if (( FRAME_DELTA < 100 )); then
    echo "За LOST-окно физический DmxOutput передал слишком мало кадров: delta=${FRAME_DELTA}." >&2
    exit 1
fi
record "physical_dmx_continues_during_lost: PASS (${FRAMES_BEFORE_LOSS} -> ${FRAMES_AFTER_LOSS}, delta=${FRAME_DELTA})"

if ask_yes_no "После >3 секунд без ArtDmx светильник всё ещё ЗЕЛЁНЫЙ, без blackout и заметного моргания?"; then
    record "physical_hold_last_green_user: PASS"
else
    record "physical_hold_last_green_user: FAIL"
    exit 1
fi

echo
echo "=== Recovery without runtime restart ==="
echo "Снова подключите тот же Art-Net OUTPUT patch в QLC+ и выставьте:"
echo "  Channel ${R} = 255"
echo "  Channel ${G} = 0"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
if ! ask_yes_no "Art-Net OUTPUT снова включён и QLC+ отправляет КРАСНЫЙ кадр на ${WB_IP}?"; then
    record "qlc_artnet_reenabled_red_user: FAIL"
    exit 1
fi

sleep 2
if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Runtime завершился вместо recovery." >&2
    exit 1
fi
if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source изменился во время recovery." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null
record "same_runtime_recovery_source_artnet: PASS"

if ask_yes_no "Без restart DMXWB светильник физически сменился с ЗЕЛЁНОГО на КРАСНЫЙ?"; then
    record "physical_recovery_red_user: PASS"
else
    record "physical_recovery_red_user: FAIL"
    exit 1
fi

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
if [[ "$(final_value final_artnet_source_state || true)" != "ACTIVE" ]]; then
    echo "После recovery финальный Art-Net source state должен быть ACTIVE." >&2
    exit 1
fi
record "final_artnet_source_active_after_recovery: PASS"

LOST_EVENTS="$(final_value final_artnet_source_lost_events || true)"
SWITCHES="$(final_value final_router_source_switches || true)"
ARTNET_RX="$(final_value final_router_artnet_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
for pair in "LOST_EVENTS:${LOST_EVENTS}" "SWITCHES:${SWITCHES}" "ARTNET_RX:${ARTNET_RX}" "DMX_FRAMES:${DMX_FRAMES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( LOST_EVENTS < 1 )); then
    echo "Runtime не зафиксировал ни одного Art-Net LOST event." >&2
    exit 1
fi
if (( SWITCHES < 2 )); then
    echo "Ожидалось минимум 2 explicit source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( ARTNET_RX < 2 )); then
    echo "Недостаточно Art-Net snapshots для initial + recovery проверки: ${ARTNET_RX}." >&2
    exit 1
fi
if (( DMX_FRAMES <= FRAMES_AFTER_LOSS )); then
    echo "Физические DMX frames не продолжились после recovery." >&2
    exit 1
fi

record "artnet_source_lost_events: PASS (${LOST_EVENTS})"
record "source_switch_count: PASS (${SWITCHES})"
record "router_artnet_snapshots: PASS (${ARTNET_RX})"
record "physical_dmx_frames: PASS (${DMX_FRAMES})"

require_final_zero final_mqtt_callback_failures
require_final_zero final_mqtt_runtime_dmx_publish_failures
require_final_zero final_mqtt_runtime_state_save_failures
require_final_zero final_artnet_bind_failures
require_final_zero final_artnet_receive_errors
require_final_zero final_artnet_send_errors
require_final_zero final_artnet_core_rejections
require_final_zero final_artnet_conflicts
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
record "same_process_artnet_transport_lifecycle: PASS"

TEST_COMPLETE=1
record "dev010b_artnet_lost_hold_recovery_result: PASS"
record "=== DMXWB DEV-010B ART-NET LOST HOLD RECOVERY PASS ==="
echo
echo "=== DMXWB DEV-010B ART-NET LOST HOLD RECOVERY PASS ==="
echo "Report: ${REPORT}"
