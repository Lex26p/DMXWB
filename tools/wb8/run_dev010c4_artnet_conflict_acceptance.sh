#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010c4_artnet_conflict_acceptance.sh user@wb8-host QLC_PLUS_PC_IP [fixture-start-address]

DEV-010C4 real-network Art-Net source conflict / no-merge acceptance:
  - QLC+ remains the active real Art-Net controller and continuously sends GREEN;
  - DMXWB Source is explicitly ART-NET;
  - WB8 injects valid conflicting RED ArtDmx packets from 127.0.0.1, Physical=99;
  - conflicting packets must increment conflict diagnostics but must not mutate output;
  - physical fixture must remain GREEN with no RED takeover or visible merge/flicker;
  - QLC+ stream remains ACTIVE and DMXWB process is not restarted;
  - physical DMX remains fixed 44 Hz.

Art-Net Port-Address = 0.
Development-only Art-Net OEM sentinel = 0xFFFF; это НЕ production OEM assignment.
QLC+ Art-Net OUTPUT patch не отключать во время conflict injection.
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
REPORT="${REPO_ROOT}/docs/DEV010C4_ARTNET_CONFLICT_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010c4-conflict"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010c4-ssh-${USER:-user}"
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV010C4 Conflict Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

cat > "${LOCAL_TMP}/inject_conflict.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

# Valid ArtDmx, Port-Address 0, Sequence 0, Physical 99, Length 4.
# Payload is RED = 255/0/0/0. Sending to loopback guarantees a source IPv4
# different from the active QLC+ source while using the same Art-Net runtime.
packet='\x41\x72\x74\x2d\x4e\x65\x74\x00\x00\x50\x00\x0e\x00\x63\x00\x00\x00\x04\xff\x00\x00\x00'

for ((i=1; i<=150; ++i)); do
    printf '%b' "${packet}" > /dev/udp/127.0.0.1/6454
    sleep 0.02
done
SH
chmod 0755 "${LOCAL_TMP}/inject_conflict.sh"

: > "${REPORT}"
record "=== DMXWB DEV-010C4 ART-NET SOURCE CONFLICT / NO-MERGE ==="
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
record "source_identity: IPv4 + ArtDmx.Physical"
record "conflict_injector_ipv4: 127.0.0.1"
record "conflict_injector_physical: 99"
record "conflict_injector_payload_rgbw: 255/0/0/0"
record "conflict_injector_packets: 150"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-010C4 Art-Net source conflict / no-merge"
echo "WB8:              ${TARGET}"
echo "QLC+ PC:          ${QLC_IP}"
echo "Fixture RGBW:     channels ${R}/${G}/${B}/${W}"
echo "Art-Net Universe: ${PORT_ADDRESS}"
echo
echo "QLC+ будет непрерывно держать GREEN как active Art-Net source."
echo "WB8 локально отправит конфликтующий RED ArtDmx с другого source identity."
echo "Runtime DMXWB и QLC+ во время injection НЕ перезапускаются."
echo
if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi

QLC_VERSION=""
while [[ -z "${QLC_VERSION}" ]]; do
    read -r -p "Введите точную версию QLC+ из About / «О программе»: " QLC_VERSION
    if [[ -z "${QLC_VERSION}" ]]; then
        echo "Версия не должна быть пустой."
    fi
done
record "external_controller: QLC+ ${QLC_VERSION}"
record "external_controller_ipv4: ${QLC_IP}"
record "external_controller_role: active_reference_source"

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
scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${LOCAL_TMP}/inject_conflict.sh" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance' '${REMOTE_DIR}/inject_conflict.sh'"

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
record "runtime_pid_before_conflict: ${RUNTIME_PID}"
record "runtime_starttime_before_conflict: ${RUNTIME_STARTTIME}"

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

FRAMES_BEFORE_CONFLICT="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_BEFORE_CONFLICT}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_BEFORE_CONFLICT}'" >&2
    exit 1
fi
record "dmx_frames_before_conflict: ${FRAMES_BEFORE_CONFLICT}"

echo
echo "=== Conflicting ArtDmx injection while QLC+ stays active ==="
echo "QLC+ OUTPUT patch НЕ отключайте и GREEN значения не меняйте."
echo "Helper отправит с самого WB8 150 валидных RED ArtDmx:"
echo "  source IPv4: 127.0.0.1"
echo "  Physical:    99"
echo "  RGBW:        255/0/0/0"
echo "Active QLC+ ${QLC_VERSION} продолжает отправлять GREEN с ${QLC_IP}."
if ! ask_yes_no "QLC+ сейчас стабильно отправляет GREEN и готовы начать conflict injection?"; then
    record "conflict_injection_user_ready: FAIL"
    exit 1
fi
record "conflict_injection_user_ready: PASS"

remote "bash '${REMOTE_DIR}/inject_conflict.sh' >'${REMOTE_DIR}/conflict-inject.log' 2>&1"
record "conflict_injection_sent: PASS (150 packets from 127.0.0.1 Physical=99)"

sleep 0.5
if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "DMXWB runtime завершился во время conflict injection." >&2
    remote "tail -n 160 '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
RUNTIME_STARTTIME_AFTER="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ "${RUNTIME_STARTTIME_AFTER}" != "${RUNTIME_STARTTIME}" ]]; then
    echo "DMXWB PID/starttime изменился во время conflict injection." >&2
    exit 1
fi
record "same_dmxwb_process_after_conflict: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"

if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source изменился во время conflict injection." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 40 >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "source_stays_artnet_after_conflict: PASS"

FRAMES_AFTER_CONFLICT="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_AFTER_CONFLICT}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_AFTER_CONFLICT}'" >&2
    exit 1
fi
FRAME_DELTA=$((FRAMES_AFTER_CONFLICT - FRAMES_BEFORE_CONFLICT))
if (( FRAME_DELTA < 100 )); then
    echo "Во время conflict injection физический DmxOutput передал слишком мало кадров: delta=${FRAME_DELTA}." >&2
    exit 1
fi
record "physical_dmx_continues_during_conflict: PASS (${FRAMES_BEFORE_CONFLICT} -> ${FRAMES_AFTER_CONFLICT}, delta=${FRAME_DELTA})"

if ask_yes_no "Во время conflict injection светильник оставался ЗЕЛЁНЫМ, без RED takeover и заметного flicker/merge?"; then
    record "physical_no_merge_conflict_user: PASS"
else
    record "physical_no_merge_conflict_user: FAIL"
    exit 1
fi

echo "QLC+ продолжайте оставлять на GREEN до финального возврата Source в MQTT."
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
    echo "После conflict injection активный QLC+ source должен снова быть ACTIVE." >&2
    exit 1
fi
record "final_artnet_source_active_after_conflict: PASS"

LOST_EVENTS="$(final_value final_artnet_source_lost_events || true)"
CONFLICTS="$(final_value final_artnet_conflicts || true)"
SWITCHES="$(final_value final_router_source_switches || true)"
ARTNET_RX="$(final_value final_router_artnet_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
for pair in "LOST_EVENTS:${LOST_EVENTS}" "CONFLICTS:${CONFLICTS}" "SWITCHES:${SWITCHES}" "ARTNET_RX:${ARTNET_RX}" "DMX_FRAMES:${DMX_FRAMES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( LOST_EVENTS != 0 )); then
    echo "Во время conflict test QLC+ не должен был уходить LOST; получено ${LOST_EVENTS}." >&2
    exit 1
fi
if (( CONFLICTS < 50 )); then
    echo "Недостаточно реально обработанных conflict packets: ${CONFLICTS}; ожидалось >=50." >&2
    exit 1
fi
if (( SWITCHES < 2 )); then
    echo "Ожидалось минимум 2 explicit source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( ARTNET_RX < 2 )); then
    echo "Недостаточно нормальных QLC+ Art-Net snapshots: ${ARTNET_RX}." >&2
    exit 1
fi
if (( DMX_FRAMES <= FRAMES_AFTER_CONFLICT )); then
    echo "Физические DMX frames не продолжились после conflict injection." >&2
    exit 1
fi

record "artnet_source_lost_events: PASS (0)"
record "artnet_conflicts_observed: PASS (${CONFLICTS})"
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
record "dev010c4_artnet_conflict_no_merge_result: PASS"
record "=== DMXWB DEV-010C4 ART-NET SOURCE CONFLICT NO-MERGE PASS ==="
echo
echo "=== DMXWB DEV-010C4 ART-NET SOURCE CONFLICT NO-MERGE PASS ==="
echo "Report: ${REPORT}"
