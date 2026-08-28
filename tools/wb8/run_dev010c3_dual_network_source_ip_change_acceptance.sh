#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010c3_dual_network_source_ip_change_acceptance.sh \
    user@wb8-host PRIMARY_PC_IP WIFI_PC_IP WIFI_WB_IP [fixture-start-address]

Для текущего acceptance stand:
  PRIMARY_PC_IP = 10.200.200.2
  WIFI_PC_IP    = 192.168.42.160
  WIFI_WB_IP    = 192.168.42.1

DEV-010C3 real-network Art-Net source IPv4 change acceptance:
  - SSH/control stays on the existing primary WB connection;
  - both Windows IPv4 addresses and both WB routes are verified without changing
    DHCP, addresses or routes;
  - QLC+ 5.2.2 first sends GREEN through the primary path;
  - primary Art-Net OUTPUT is fully disabled for >3 s, causing LOST while
    Source remains ART-NET and physical output Holds Last GREEN;
  - the same QLC+ output is then reconfigured to the Wi-Fi path and sends RED;
  - DMXWB must accept the new source IPv4 without process restart or blackout;
  - old and new Art-Net OUTPUT paths must never be active at the same time.

Art-Net Port-Address = 0.
Development-only Art-Net OEM sentinel = 0xFFFF; это НЕ production OEM assignment.
No Windows/WB network configuration is modified by this helper.
Ответы на визуальные вопросы: только латинские y или n.
USAGE
}
if [[ $# -lt 4 || $# -gt 5 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
PRIMARY_PC_IP="$2"
WIFI_PC_IP="$3"
WIFI_WB_IP="$4"
START_ADDRESS="${5:-1}"
PORT="/dev/ttyRS485-1"
PORT_ADDRESS=0

for ip_value in "${PRIMARY_PC_IP}" "${WIFI_PC_IP}" "${WIFI_WB_IP}"; do
    if [[ ! "${ip_value}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
        echo "Ожидался IPv4 address, получено '${ip_value}'." >&2
        exit 2
    fi
done
if [[ "${PRIMARY_PC_IP}" == "${WIFI_PC_IP}" ]]; then
    echo "PRIMARY_PC_IP и WIFI_PC_IP должны различаться." >&2
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
REPORT="${REPO_ROOT}/docs/DEV010C3_ARTNET_SOURCE_IP_CHANGE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010c3-dual-network-ip-change"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010c3-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"
NETWORK_PS1="${REPO_ROOT}/tools/windows/check_dev010c3_dual_network.ps1"
NETWORK_PS1_WIN=""

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

if [[ ! -f "${NETWORK_PS1}" ]]; then
    echo "Не найден Windows dual-network checker: ${NETWORK_PS1}" >&2
    exit 1
fi
NETWORK_PS1_WIN="$(wslpath -w "${NETWORK_PS1}")"

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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV010C3 Source IP Change Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-010C3 REAL-NETWORK SOURCE IPv4 CHANGE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target_management_path: ${TARGET}"
record "primary_pc_ipv4: ${PRIMARY_PC_IP}"
record "wifi_pc_ipv4: ${WIFI_PC_IP}"
record "wifi_wb_ipv4_expected: ${WIFI_WB_IP}"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "fixture_channels_rgbw: ${R}/${G}/${B}/${W}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "development_oem_placeholder: 0xFFFF"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-010C3 real-network Art-Net source IPv4 change"
echo "Management/SSH: ${TARGET}"
echo "Primary QLC+:   ${PRIMARY_PC_IP}"
echo "Wi-Fi QLC+:    ${WIFI_PC_IP}"
echo "Wi-Fi WB8:     ${WIFI_WB_IP}"
echo "Fixture RGBW:  channels ${R}/${G}/${B}/${W}"
echo
echo "Оба сетевых соединения должны оставаться подключёнными весь тест."
echo "Helper ничего не меняет в IP/DHCP/routes."
echo
if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi

QLC_VERSION="5.2.2"
record "external_controller: QLC+ ${QLC_VERSION}"
record "external_controller_version_source: fixed acceptance-stand value confirmed by user"

echo
echo "=== Deterministic start / dual-network preflight ==="
echo "Перед запуском runtime ОТКЛЮЧИТЕ Art-Net OUTPUT patch у Universe 1."
echo "Глобальный Blackout / зелёный глаз НЕ использовать."
if ! ask_yes_no "Art-Net OUTPUT patch в QLC+ сейчас отключён?"; then
    record "qlc_output_disabled_before_runtime_user: FAIL"
    exit 2
fi
record "qlc_output_disabled_before_runtime_user: PASS"

echo "Открываем одно SSH-соединение по primary management path."
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

PRIMARY_ROUTE="$(remote "ip -4 route get '${PRIMARY_PC_IP}' | head -n1" | tr -d '\r')"
PRIMARY_WB_INTERFACE="$(awk '{for(i=1;i<=NF;i++) if($i=="dev" && i<NF){print $(i+1); exit}}' <<<"${PRIMARY_ROUTE}")"
PRIMARY_WB_IP="$(awk '{for(i=1;i<=NF;i++) if($i=="src" && i<NF){print $(i+1); exit}}' <<<"${PRIMARY_ROUTE}")"
if [[ -z "${PRIMARY_WB_INTERFACE}" || -z "${PRIMARY_WB_IP}" ]]; then
    echo "Не удалось определить primary WB route: ${PRIMARY_ROUTE}" >&2
    exit 1
fi

WIFI_ROUTE="$(remote "ip -4 route get '${WIFI_PC_IP}' | head -n1" | tr -d '\r')"
WIFI_WB_INTERFACE="$(awk '{for(i=1;i<=NF;i++) if($i=="dev" && i<NF){print $(i+1); exit}}' <<<"${WIFI_ROUTE}")"
WIFI_WB_ROUTE_IP="$(awk '{for(i=1;i<=NF;i++) if($i=="src" && i<NF){print $(i+1); exit}}' <<<"${WIFI_ROUTE}")"
if [[ -z "${WIFI_WB_INTERFACE}" || -z "${WIFI_WB_ROUTE_IP}" ]]; then
    echo "Не удалось определить Wi-Fi WB route: ${WIFI_ROUTE}" >&2
    exit 1
fi
if [[ "${WIFI_WB_ROUTE_IP}" != "${WIFI_WB_IP}" ]]; then
    echo "WB route к ${WIFI_PC_IP} использует src ${WIFI_WB_ROUTE_IP}, ожидался ${WIFI_WB_IP}." >&2
    exit 1
fi
if [[ "${PRIMARY_WB_INTERFACE}" == "${WIFI_WB_INTERFACE}" ]]; then
    echo "Primary и Wi-Fi routes используют один WB interface '${PRIMARY_WB_INTERFACE}' — dual-network IP-change не доказан." >&2
    exit 1
fi
if ! remote "ip -4 addr show dev '${WIFI_WB_INTERFACE}' | grep -F 'inet ${WIFI_WB_IP}/' >/dev/null"; then
    echo "На WB interface ${WIFI_WB_INTERFACE} не найден ${WIFI_WB_IP}." >&2
    exit 1
fi

WB_MAC="$(remote "cat '/sys/class/net/${PRIMARY_WB_INTERFACE}/address'" | tr -d '\r')"
if [[ ! "${WB_MAC}" =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]]; then
    echo "Не удалось определить primary WB MAC: '${WB_MAC}'" >&2
    exit 1
fi

record "primary_wb_interface: ${PRIMARY_WB_INTERFACE}"
record "primary_wb_ipv4: ${PRIMARY_WB_IP}"
record "primary_wb_mac: ${WB_MAC}"
record "primary_route_to_pc: ${PRIMARY_ROUTE}"
record "wifi_wb_interface: ${WIFI_WB_INTERFACE}"
record "wifi_wb_ipv4: ${WIFI_WB_ROUTE_IP}"
record "wifi_route_to_pc: ${WIFI_ROUTE}"
record "wb_dual_network_interfaces_distinct: PASS"

WINDOWS_NETWORK_LOG="${LOCAL_TMP}/windows-dual-network.txt"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${NETWORK_PS1_WIN}" \
    -PrimarySourceIp "${PRIMARY_PC_IP}" \
    -PrimaryDestinationIp "${PRIMARY_WB_IP}" \
    -SecondarySourceIp "${WIFI_PC_IP}" \
    -SecondaryDestinationIp "${WIFI_WB_IP}" \
    | tr -d '\r' | tee "${WINDOWS_NETWORK_LOG}" | tee -a "${REPORT}"

if ! grep -qx 'dual_network_check=PASS' "${WINDOWS_NETWORK_LOG}"; then
    echo "Windows dual-network preflight не дал PASS." >&2
    exit 1
fi
record "dual_network_preflight: PASS"
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
record "runtime_pid_before_source_ip_change: ${RUNTIME_PID}"
record "runtime_starttime_before_source_ip_change: ${RUNTIME_STARTTIME}"

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
echo "  Network interface:        ${PRIMARY_PC_IP}"
echo "  Destination / IP Address: ${PRIMARY_WB_IP}"
echo "  Art-Net Universe:         ${PORT_ADDRESS}"
echo "  Transmission Mode:        Standard"
echo "В Simple Desk выставьте:"
echo "  Channel ${R} = 0"
echo "  Channel ${G} = 255"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
if ! ask_yes_no "Art-Net OUTPUT снова включён и QLC+ сейчас отправляет этот зелёный Art-Net на ${PRIMARY_WB_IP}?"; then
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

FRAMES_BEFORE_IP_CHANGE="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_BEFORE_IP_CHANGE}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_BEFORE_IP_CHANGE}'" >&2
    exit 1
fi
record "dmx_frames_before_source_ip_change: ${FRAMES_BEFORE_IP_CHANGE}"

echo
echo "=== Release PRIMARY source lock: ${PRIMARY_PC_IP} ==="
echo "Полностью ОТКЛЮЧИТЕ Art-Net OUTPUT patch QLC+."
echo "Не меняйте Source DMXWB — он должен остаться ART-NET."
if ! ask_yes_no "Primary Art-Net OUTPUT полностью отключён?"; then
    record "primary_artnet_output_disabled_user: FAIL"
    exit 1
fi
record "primary_artnet_output_disabled_user: PASS"

echo "Ждём >3 s для LOST/source-lock release..."
sleep 3.6

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "DMXWB runtime завершился во время source-loss window." >&2
    exit 1
fi
RUNTIME_STARTTIME_LOST="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ "${RUNTIME_STARTTIME_LOST}" != "${RUNTIME_STARTTIME}" ]]; then
    echo "DMXWB PID/starttime изменился во время source-loss window." >&2
    exit 1
fi
if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source автоматически изменился после потери primary Art-Net." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null

FRAMES_AFTER_LOST="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
LOST_FRAME_DELTA=$((FRAMES_AFTER_LOST - FRAMES_BEFORE_IP_CHANGE))
if (( LOST_FRAME_DELTA < 100 )); then
    echo "Во время LOST физический DMX передал слишком мало кадров: delta=${LOST_FRAME_DELTA}." >&2
    exit 1
fi
record "same_dmxwb_process_during_primary_lost: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"
record "source_stays_artnet_during_primary_lost: PASS"
record "physical_dmx_continues_during_primary_lost: PASS (${FRAMES_BEFORE_IP_CHANGE} -> ${FRAMES_AFTER_LOST}, delta=${LOST_FRAME_DELTA})"

if ask_yes_no "После >3 s без primary Art-Net светильник остался GREEN без blackout/flicker?"; then
    record "primary_lost_hold_last_green_user: PASS"
else
    record "primary_lost_hold_last_green_user: FAIL"
    exit 1
fi

echo
echo "=== Reconfigure SAME QLC+ output to Wi-Fi source IPv4 ==="
echo "Теперь подключите Art-Net OUTPUT снова, но ТОЛЬКО через Wi-Fi:"
echo "  Network interface:        ${WIFI_PC_IP}"
echo "  Destination / IP Address: ${WIFI_WB_IP}"
echo "  Art-Net Universe:         ${PORT_ADDRESS}"
echo "  Transmission Mode:        Standard"
echo
echo "После переподключения QLC+ может сбросить channel values в 0."
echo "Поэтому ОБЯЗАТЕЛЬНО заново выставьте RED:"
echo "  Channel ${R} = 255"
echo "  Channel ${G} = 0"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
echo
echo "Старый primary OUTPUT ${PRIMARY_PC_IP} -> ${PRIMARY_WB_IP} должен оставаться выключенным."
if ! ask_yes_no "QLC+ сейчас отправляет RED только по Wi-Fi ${WIFI_PC_IP} -> ${WIFI_WB_IP}?"; then
    record "wifi_source_red_config_user: FAIL"
    exit 1
fi
record "wifi_source_red_config_user: PASS"

# Existing snapshot flag is already set, so the new source is proven by:
# 1) >3 s release of the old lock,
# 2) distinct verified real network path,
# 3) new physical RED without restarting DMXWB,
# 4) zero conflicts in final diagnostics.
sleep 1

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "DMXWB runtime завершился после Wi-Fi source change." >&2
    exit 1
fi
RUNTIME_STARTTIME_AFTER="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ "${RUNTIME_STARTTIME_AFTER}" != "${RUNTIME_STARTTIME}" ]]; then
    echo "DMXWB PID/starttime изменился после Wi-Fi source change." >&2
    exit 1
fi
if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source изменился вместо сохранения ART-NET." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null

if ask_yes_no "Без restart DMXWB светильник перешёл GREEN -> RED и сейчас стабильно RED?"; then
    record "wifi_new_source_red_physical_user: PASS"
else
    record "wifi_new_source_red_physical_user: FAIL"
    exit 1
fi

FRAMES_AFTER_IP_CHANGE="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
IP_CHANGE_FRAME_DELTA=$((FRAMES_AFTER_IP_CHANGE - FRAMES_AFTER_LOST))
if (( IP_CHANGE_FRAME_DELTA < 40 )); then
    echo "После Wi-Fi source change физический DMX передал слишком мало кадров: delta=${IP_CHANGE_FRAME_DELTA}." >&2
    exit 1
fi
record "same_dmxwb_process_after_source_ip_change: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"
record "source_stays_artnet_after_source_ip_change: PASS"
record "physical_dmx_continues_after_source_ip_change: PASS (${FRAMES_AFTER_LOST} -> ${FRAMES_AFTER_IP_CHANGE}, delta=${IP_CHANGE_FRAME_DELTA})"
record "real_network_source_ipv4_change_path: PASS (${PRIMARY_PC_IP} -> ${WIFI_PC_IP})"

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
if [[ "${FINAL_SOURCE_STATE}" != "ACTIVE" ]]; then
    echo "При оставленном включённым Wi-Fi QLC+ source ожидался final_artnet_source_state=ACTIVE, получено '${FINAL_SOURCE_STATE}'." >&2
    exit 1
fi
record "final_artnet_source_state_after_ip_change: PASS (ACTIVE)"

if [[ "$(final_value final_artnet_sync_mode || true)" != "ASYNC" ]]; then
    echo "QLC+ source IP-change acceptance ожидает ASYNC Art-Net mode." >&2
    exit 1
fi
record "final_artnet_sync_mode: PASS (ASYNC)"

LOST_EVENTS="$(final_value final_artnet_source_lost_events || true)"
CONFLICTS="$(final_value final_artnet_conflicts || true)"
SWITCHES="$(final_value final_router_source_switches || true)"
DATAGRAMS="$(final_value final_artnet_datagrams_received || true)"
ARTNET_RX="$(final_value final_router_artnet_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
for pair in \
    "LOST_EVENTS:${LOST_EVENTS}" \
    "CONFLICTS:${CONFLICTS}" \
    "SWITCHES:${SWITCHES}" \
    "DATAGRAMS:${DATAGRAMS}" \
    "ARTNET_RX:${ARTNET_RX}" \
    "DMX_FRAMES:${DMX_FRAMES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( LOST_EVENTS < 1 )); then
    echo "Не зафиксирован intentional LOST старого primary source." >&2
    exit 1
fi
if (( CONFLICTS != 0 )); then
    echo "При корректном последовательном IP-change conflicts должны быть 0; получено ${CONFLICTS}. Проверьте, что primary output не остался активным." >&2
    exit 1
fi
if (( SWITCHES < 2 )); then
    echo "Ожидалось минимум 2 explicit Source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( DATAGRAMS < 20 )); then
    echo "Слишком мало Art-Net datagrams для двух real-network phases: ${DATAGRAMS}." >&2
    exit 1
fi
if (( ARTNET_RX < 10 )); then
    echo "Слишком мало routed Art-Net snapshots: ${ARTNET_RX}." >&2
    exit 1
fi
if (( DMX_FRAMES <= FRAMES_AFTER_IP_CHANGE )); then
    echo "Физические DMX frames не продолжились до shutdown." >&2
    exit 1
fi

record "primary_source_lost_release_events: PASS (${LOST_EVENTS})"
record "artnet_conflicts: PASS (0)"
record "source_switch_count: PASS (${SWITCHES})"
record "artnet_datagrams_received: PASS (${DATAGRAMS})"
record "router_artnet_snapshots: PASS (${ARTNET_RX})"
record "physical_dmx_frames: PASS (${DMX_FRAMES})"
record "source_ipv4_change_acceptance: PASS (${PRIMARY_PC_IP} -> ${WIFI_PC_IP}, same DMXWB process)"
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
record "dev010c3_real_network_source_ipv4_change_result: PASS"
record "=== DMXWB DEV-010C3 REAL-NETWORK SOURCE IPv4 CHANGE PASS ==="
echo
echo "=== DMXWB DEV-010C3 REAL-NETWORK SOURCE IPv4 CHANGE PASS ==="
echo "Report: ${REPORT}"
