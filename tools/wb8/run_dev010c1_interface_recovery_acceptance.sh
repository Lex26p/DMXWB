#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010c1_interface_recovery_acceptance.sh user@wb8-host QLC_PLUS_PC_IP [fixture-start-address]

DEV-010C1 WB8 network-interface down/up recovery acceptance:
  - unified MQTT + Art-Net + Source Router + physical DmxOutput;
  - real QLC+ ArtDmx selected as physical Source;
  - WB8 interface to QLC+ is administratively DOWN for about 7 seconds;
  - interface UP is performed automatically by an independent transient systemd service;
  - a second delayed systemd service is armed only as safety recovery;
  - DMXWB process is not restarted;
  - Source stays ART-NET;
  - physical DMX holds the last Art-Net value and keeps 44 Hz output;
  - after interface returns, the same process accepts new ArtDmx again.

Art-Net Port-Address = 0.
Development-only Art-Net OEM sentinel = 0xFFFF; это НЕ production OEM assignment.
QLC+ Art-Net OUTPUT patch во время interface outage НЕ отключать.
Ответы на визуальные вопросы: только латинские y или n.

ВАЖНО:
  SSH идёт через тот же interface, который будет выключен. Скрипт сам
  запланирует автоматический UP до отключения interface. После восстановления
  SSH-пароль может потребоваться второй раз.
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
REPORT="${REPO_ROOT}/docs/DEV010C1_WB8_INTERFACE_RECOVERY_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010c1-interface"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010c1-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"
OUTAGE_UNIT="dmxwb-dev010c1-iface-${$}"
FALLBACK_UNIT="${OUTAGE_UNIT}-fallback"

for command_name in ssh scp sha256sum awk grep git sed tail tr seq ping; do
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

SSH_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=1200"
    -o "StrictHostKeyChecking=accept-new"
    -o "ConnectTimeout=6"
    -o "ServerAliveInterval=2"
    -o "ServerAliveCountMax=3"
)
SCP_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=1200"
    -o "StrictHostKeyChecking=accept-new"
    -o "ConnectTimeout=6"
)

SSH_OPENED=0
RUNTIME_PID=""
RUNTIME_STARTTIME=""
RUNTIME_STARTED=0
TEST_COMPLETE=0
WB_IP=""
WB_INTERFACE=""
IP_BIN=""

remote() { ssh "${SSH_OPTS[@]}" "${TARGET}" "$@"; }
record() { printf '%s\n' "$*" | tee -a "${REPORT}"; }

open_master() {
    rm -f "${CONTROL_DIR}"/control-* >/dev/null 2>&1 || true
    ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
    SSH_OPENED=1
}

close_master() {
    if (( SSH_OPENED == 1 )); then
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
        SSH_OPENED=0
    fi
    rm -f "${CONTROL_DIR}"/control-* >/dev/null 2>&1 || true
}

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

wait_for_ping() {
    local attempts="${1:-25}"
    for ((i=1; i<=attempts; ++i)); do
        if ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
            printf '%s' "${i}"
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_stable_ping() {
    local required="${1:-4}"
    local attempts="${2:-40}"
    local consecutive=0
    for ((i=1; i<=attempts; ++i)); do
        if ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
            consecutive=$((consecutive + 1))
            if (( consecutive >= required )); then
                return 0
            fi
        else
            consecutive=0
        fi
        sleep 1
    done
    return 1
}

reopen_ssh_after_network() {
    close_master
    if ! wait_for_stable_ping 4 40; then
        echo "WB8 ${WB_IP} не достиг устойчивой доступности (4 последовательных ping)." >&2
        return 1
    fi
    echo "WB8 снова стабильно доступен. Переоткрываем SSH; пароль может потребоваться ещё раз." >&2
    if ! open_master; then
        echo "Не удалось переоткрыть SSH после восстановления сети." >&2
        return 1
    fi
    return 0
}

verify_same_runtime_resilient() {
    local context="$1"
    local observed_starttime=""
    local rc=0

    set +e
    observed_starttime="$(remote "if kill -0 '${RUNTIME_PID}' 2>/dev/null; then awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'; else exit 3; fi" 2>/dev/null | tr -d '\r')"
    rc=$?
    set -e

    if (( rc == 255 )); then
        echo "SSH недоступен во время проверки '${context}'; это не считается смертью runtime." >&2
        if ! reopen_ssh_after_network; then
            return 1
        fi
        set +e
        observed_starttime="$(remote "if kill -0 '${RUNTIME_PID}' 2>/dev/null; then awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'; else exit 3; fi" 2>/dev/null | tr -d '\r')"
        rc=$?
        set -e
    fi

    if (( rc == 3 )); then
        echo "После доступного SSH PID ${RUNTIME_PID} действительно отсутствует." >&2
        return 1
    fi
    if (( rc != 0 )); then
        echo "Не удалось проверить runtime identity для '${context}', SSH/remote rc=${rc}." >&2
        return 1
    fi
    if [[ ! "${observed_starttime}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный process starttime '${observed_starttime}' для '${context}'." >&2
        return 1
    fi
    if [[ "${observed_starttime}" != "${RUNTIME_STARTTIME}" ]]; then
        echo "Runtime PID/starttime изменился для '${context}': process был перезапущен." >&2
        return 1
    fi
    return 0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM

    # If interrupted during the deliberate outage, wait for the primary/fallback
    # systemd service to restore network before best-effort runtime cleanup.
    if [[ -n "${WB_IP}" ]] && ! ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
        echo "Ожидаем automatic interface safety recovery..." >&2
        wait_for_ping 25 >/dev/null 2>&1 || true
    fi

    close_master

    if [[ -n "${WB_IP}" ]] && ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
        if open_master >/dev/null 2>&1; then
            remote "systemctl stop '${FALLBACK_UNIT}.service' >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
            if (( RUNTIME_STARTED == 1 && TEST_COMPLETE == 0 )) && [[ -n "${RUNTIME_PID}" ]]; then
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 1" >/dev/null 2>&1 || true
                record "--- runtime.log after incomplete acceptance ---" || true
                remote "tail -n 200 '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tee -a "${REPORT}" >/dev/null || true
            fi
            if [[ -n "${RUNTIME_PID}" ]]; then
                remote "kill -TERM '${RUNTIME_PID}' 2>/dev/null || true; sleep 1; kill -KILL '${RUNTIME_PID}' 2>/dev/null || true" >/dev/null 2>&1 || true
            fi
            remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
            close_master
        fi
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV010C1 Interface Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

cat > "${LOCAL_TMP}/interface-cycle.sh" <<'SH'
#!/bin/sh
set -eu
iface="$1"
log="$2"
ip_bin="$3"
fallback_service="$4"

{
    printf 'cycle_started_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'interface: %s\n' "${iface}"
    printf 'initial_operstate: '
    cat "/sys/class/net/${iface}/operstate" 2>/dev/null || true
} > "${log}"

# Give the submitting SSH command enough time to return and the client enough
# time to close its control master before the test interface is dropped.
sleep 3

{
    printf 'before_down_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'before_down_operstate: '
    cat "/sys/class/net/${iface}/operstate" 2>/dev/null || true
} >> "${log}"

"${ip_bin}" link set dev "${iface}" down

{
    printf 'interface_down_commanded_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'after_down_operstate: '
    cat "/sys/class/net/${iface}/operstate" 2>/dev/null || true
} >> "${log}"

sleep 7

"${ip_bin}" link set dev "${iface}" up

{
    printf 'interface_up_commanded_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
    sleep 1
    printf 'after_up_operstate: '
    cat "/sys/class/net/${iface}/operstate" 2>/dev/null || true
} >> "${log}"

# The primary path has already restored the interface. Cancel the delayed
# safety service immediately on the WB8 itself, before any SSH reconnect delay.
systemctl stop "${fallback_service}" >/dev/null 2>&1 || true

{
    printf 'primary_cancelled_safety_fallback_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'cycle_completed_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
} >> "${log}"
SH

cat > "${LOCAL_TMP}/interface-fallback.sh" <<'SH'
#!/bin/sh
set -eu
iface="$1"
log="$2"
ip_bin="$3"

sleep 20
"${ip_bin}" link set dev "${iface}" up
{
    printf 'safety_fallback_executed_utc: '
    date -u +%Y-%m-%dT%H:%M:%SZ
} >> "${log}"
SH

chmod 0755 "${LOCAL_TMP}/interface-cycle.sh" "${LOCAL_TMP}/interface-fallback.sh"

: > "${REPORT}"
record "=== DMXWB DEV-010C1 WB8 NETWORK INTERFACE DOWN / UP RECOVERY ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "qlc_plus_pc_ip: ${QLC_IP}"
record "external_controller: QLC+ 5"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "fixture_channels_rgbw: ${R}/${G}/${B}/${W}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "development_oem_placeholder: 0xFFFF"
record "planned_interface_down_seconds: 7"
record "safety_fallback_up_seconds: 20"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-010C1 WB8 interface down/up recovery"
echo "WB8:              ${TARGET}"
echo "QLC+ PC:          ${QLC_IP}"
echo "Fixture RGBW:     channels ${R}/${G}/${B}/${W}"
echo "Art-Net Universe: ${PORT_ADDRESS}"
echo
echo "Тест временно выключит WB8 network interface к QLC+ примерно на 7 секунд."
echo "DMXWB process и физический DmxOutput перезапускаться НЕ будут."
echo "Interface UP выполняется автоматически systemd service на самом WB8."
echo
if ! ask_yes_no "RGBW-светильник подключён к ${PORT}, Start Address ${START_ADDRESS}, и краткий network outage допустим?"; then
    exit 2
fi

echo "Открываем SSH. После network outage пароль может потребоваться ещё раз."
open_master

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl systemd-run ip; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
if [[ "$(remote "systemctl is-active mosquitto 2>/dev/null || true" | tr -d '\r')" != "active" ]]; then
    echo "Mosquitto service на target не active." >&2
    exit 1
fi
if ! remote "test -e '${PORT}'"; then
    echo "На WB8 отсутствует ${PORT}." >&2
    exit 1
fi

IP_BIN="$(remote "command -v ip" | tr -d '\r')"
if [[ -z "${IP_BIN}" ]]; then
    echo "На WB8 не найден ip." >&2
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

LINK_BEFORE="$(remote "ip -o link show dev '${WB_INTERFACE}'" | tr -d '\r')"
ADDR_BEFORE="$(remote "ip -4 -o addr show dev '${WB_INTERFACE}'" | tr -d '\r')"
if [[ "${ADDR_BEFORE}" != *"${WB_IP}"* ]]; then
    echo "IPv4 ${WB_IP} не найден на ${WB_INTERFACE} перед тестом." >&2
    exit 1
fi
if ! ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
    echo "WB8 ${WB_IP} не отвечает на ping до interface test; нельзя доказать down/up." >&2
    exit 1
fi

record "wb8_interface: ${WB_INTERFACE}"
record "wb8_ipv4: ${WB_IP}"
record "wb8_mac: ${WB_MAC}"
record "route_to_qlc: ${ROUTE_LINE}"
record "link_before: ${LINK_BEFORE}"
record "addr_before: ${ADDR_BEFORE}"
record "pre_outage_ping: PASS"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" \
    "${BINARY}" \
    "${LOCAL_TMP}/config.json" \
    "${LOCAL_TMP}/state.json" \
    "${LOCAL_TMP}/interface-cycle.sh" \
    "${LOCAL_TMP}/interface-fallback.sh" \
    "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance' '${REMOTE_DIR}/interface-cycle.sh' '${REMOTE_DIR}/interface-fallback.sh'"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-dev010-source-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' --development-oem-code FFFF --mac '${WB_MAC}' --status-interval-ms 250 >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Unified acceptance runtime завершился раньше времени:" >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
RUNTIME_STARTTIME="$(remote "awk '{print \$22}' '/proc/${RUNTIME_PID}/stat'" | tr -d '\r')"
if [[ ! "${RUNTIME_STARTTIME}" =~ ^[0-9]+$ ]]; then
    echo "Не удалось зафиксировать process starttime runtime." >&2
    exit 1
fi

if [[ "$(remote "grep -c '^runtime_started: PASS$' '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tr -d '\r')" == "0" ]]; then
    echo "runtime_started: PASS не найден." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
record "unified_runtime_started: PASS"
record "runtime_pid_before_outage: ${RUNTIME_PID}"
record "runtime_starttime_before_outage: ${RUNTIME_STARTTIME}"

wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" 20 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_runtime_value status_has_mqtt_snapshot 1 >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "initial_source_mqtt: PASS"

# Keep safe MQTT OFF in the background. It must not take over during interface loss.
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 20 >/dev/null

echo
echo "=== QLC+ Art-Net GREEN before interface outage ==="
echo "QLC+ Universe 1 должен иметь постоянно подключённый Art-Net OUTPUT:"
echo "  Network interface:        ${QLC_IP}"
echo "  Destination / IP Address: ${WB_IP}"
echo "  Art-Net Universe:         ${PORT_ADDRESS}"
echo "  Transmission Mode:        Standard"
echo "В Simple Desk заново выставьте:"
echo "  Channel ${R} = 0"
echo "  Channel ${G} = 255"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
echo "Art-Net OUTPUT patch после этого НЕ отключать до конца network outage."
if ! ask_yes_no "QLC+ сейчас отправляет ЗЕЛЁНЫЙ Art-Net на ${WB_IP}?"; then
    record "qlc_green_before_interface_outage_user: FAIL"
    exit 1
fi

wait_runtime_value status_has_artnet_snapshot 1 50 >/dev/null
record "artnet_snapshot_before_interface_outage: PASS"

mqtt_pub "/devices/dmxwb/controls/source/on" "artnet"
wait_topic "/devices/dmxwb/controls/source" "artnet" 20 >/dev/null
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null
record "source_artnet_selected: PASS"

if ask_yes_no "Перед interface outage светильник физически ЗЕЛЁНЫЙ?"; then
    record "green_physical_before_interface_outage_user: PASS"
else
    record "green_physical_before_interface_outage_user: FAIL"
    exit 1
fi

FRAMES_BEFORE="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_BEFORE}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_BEFORE}'" >&2
    exit 1
fi
record "dmx_frames_before_interface_outage: ${FRAMES_BEFORE}"

echo
echo "=== WB8 interface DOWN / automatic UP ==="
echo "Следующие ~10 секунд QLC+ не трогайте и наблюдайте за светильником."
echo "SSH к ${WB_IP} ожидаемо пропадёт. Скрипт продолжит работать локально."
if ! ask_yes_no "Готовы выполнить реальный ${WB_INTERFACE} DOWN/UP?"; then
    record "interface_cycle_user_ready: FAIL"
    exit 2
fi
record "interface_cycle_user_ready: PASS"

# Arm safety fallback first. It is not the PASS path; it only guarantees a later
# UP if the primary interface-cycle transient service fails unexpectedly.
remote "systemd-run --unit='${FALLBACK_UNIT}' '${REMOTE_DIR}/interface-fallback.sh' '${WB_INTERFACE}' '${REMOTE_DIR}/interface-outage.log' '${IP_BIN}' >'${REMOTE_DIR}/fallback-systemd-run.log' 2>&1"
record "safety_fallback_service_armed: PASS (${FALLBACK_UNIT}.service)"

# Primary service: wait 3 s, interface DOWN for 7 s, then interface UP.
remote "systemd-run --unit='${OUTAGE_UNIT}' '${REMOTE_DIR}/interface-cycle.sh' '${WB_INTERFACE}' '${REMOTE_DIR}/interface-outage.log' '${IP_BIN}' '${FALLBACK_UNIT}.service' >'${REMOTE_DIR}/cycle-systemd-run.log' 2>&1"
record "primary_interface_cycle_service_started: PASS (${OUTAGE_UNIT}.service)"

# Close multiplexed SSH deliberately before the service drops the link.
close_master

# Primary service waits 3 seconds before DOWN. At ~5 seconds the interface must
# be demonstrably unreachable from the laptop.
sleep 5
if ping -c 1 -W 1 "${WB_IP}" >/dev/null 2>&1; then
    echo "WB8 ${WB_IP} всё ещё отвечает во время ожидаемого interface DOWN." >&2
    exit 1
fi
record "interface_unreachable_during_down: PASS"

# Wait for automatic recovery. Primary should restore around t=10 s; safety
# fallback is only at t=20 s. We allow enough time to regain connectivity, but
# later require the PRIMARY systemd unit itself to have Result=success.
RECOVERY_POLL="$(wait_for_ping 25 || true)"
if [[ -z "${RECOVERY_POLL}" ]]; then
    echo "WB8 ${WB_IP} не вернулся даже после automatic safety recovery window." >&2
    exit 1
fi
record "interface_reachable_after_automatic_up: PASS (poll ${RECOVERY_POLL})"

if ! wait_for_stable_ping 4 30; then
    echo "После automatic UP WB8 не достиг устойчивой сетевой доступности." >&2
    exit 1
fi
record "interface_stable_after_automatic_up: PASS (4 consecutive ping)"

echo "Network стабильно вернулся. Открываем новое SSH-соединение; пароль может потребоваться второй раз."
open_master

LINK_AFTER="$(remote "ip -o link show dev '${WB_INTERFACE}'" | tr -d '\r')"
ADDR_AFTER="$(remote "ip -4 -o addr show dev '${WB_INTERFACE}'" | tr -d '\r')"
if [[ "${ADDR_AFTER}" != *"${WB_IP}"* ]]; then
    echo "После UP IPv4 ${WB_IP} не восстановлен на ${WB_INTERFACE}." >&2
    exit 1
fi
record "link_after: ${LINK_AFTER}"
record "addr_after: ${ADDR_AFTER}"

PRIMARY_RESULT="$(remote "systemctl show '${OUTAGE_UNIT}.service' -p Result --value 2>/dev/null || true" | tr -d '\r')"
if [[ "${PRIMARY_RESULT}" != "success" ]]; then
    echo "Primary interface-cycle systemd unit Result='${PRIMARY_RESULT}', ожидался success." >&2
    remote "systemctl status '${OUTAGE_UNIT}.service' --no-pager 2>/dev/null || true" >&2 || true
    exit 1
fi
record "primary_interface_cycle_service_result: PASS (success)"

OUTAGE_LOG="$(remote "cat '${REMOTE_DIR}/interface-outage.log' 2>/dev/null || true" | tr -d '\r')"
if [[ "${OUTAGE_LOG}" != *"interface_down_commanded_utc:"* ||
      "${OUTAGE_LOG}" != *"interface_up_commanded_utc:"* ||
      "${OUTAGE_LOG}" != *"primary_cancelled_safety_fallback_utc:"* ||
      "${OUTAGE_LOG}" != *"cycle_completed_utc:"* ]]; then
    echo "Interface outage log не подтверждает полный primary DOWN/UP cycle:" >&2
    printf '%s\n' "${OUTAGE_LOG}" >&2
    exit 1
fi
record "--- interface cycle log ---"
printf '%s\n' "${OUTAGE_LOG}" | tee -a "${REPORT}"
record "primary_interface_down_up_log: PASS"

# Primary service itself must cancel the delayed safety service immediately
# after successful interface UP. This check is intentionally independent of
# how long the user takes to re-enter the SSH password.
FALLBACK_MARKER="$(remote "grep -c '^safety_fallback_executed_utc:' '${REMOTE_DIR}/interface-outage.log' 2>/dev/null || true" | tr -d '\r')"
if [[ "${FALLBACK_MARKER}" != "0" ]]; then
    echo "Safety fallback выполнился до того, как primary path смог его отменить." >&2
    exit 1
fi
record "primary_cancelled_safety_fallback: PASS"
record "safety_fallback_not_used_for_pass: PASS"

if ! verify_same_runtime_resilient "after primary interface cycle"; then
    exit 1
fi
record "same_runtime_process_after_interface_cycle: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"

if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source автоматически изменился после interface outage." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
# Allow resumed QLC+ traffic to make GoodOutput active again.
wait_runtime_value status_artnet_output_active 1 60 >/dev/null
record "source_stays_artnet_and_recovers_active: PASS"

FRAMES_AFTER="$(wait_runtime_uint_ge status_dmx_frames_sent 1 30)"
if [[ ! "${FRAMES_AFTER}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный status_dmx_frames_sent='${FRAMES_AFTER}'" >&2
    exit 1
fi
FRAME_DELTA=$((FRAMES_AFTER - FRAMES_BEFORE))
if (( FRAME_DELTA < 200 )); then
    echo "Физический DmxOutput передал слишком мало кадров через outage: delta=${FRAME_DELTA}." >&2
    exit 1
fi
record "physical_dmx_continues_through_interface_outage: PASS (${FRAMES_BEFORE} -> ${FRAMES_AFTER}, delta=${FRAME_DELTA})"

if ask_yes_no "Во время DOWN/UP светильник всё время оставался ЗЕЛЁНЫМ, без blackout и заметного моргания?"; then
    record "physical_hold_last_during_interface_outage_user: PASS"
else
    record "physical_hold_last_during_interface_outage_user: FAIL"
    exit 1
fi

echo
echo "=== Fresh ArtDmx after WB8 interface recovery ==="
echo "QLC+ Art-Net OUTPUT оставьте подключённым и выставьте:"
echo "  Channel ${R} = 255"
echo "  Channel ${G} = 0"
echo "  Channel ${B} = 0"
echo "  Channel ${W} = 0"
if ! ask_yes_no "QLC+ после recovery сейчас отправляет КРАСНЫЙ Art-Net на ${WB_IP}?"; then
    record "qlc_red_after_interface_recovery_user: FAIL"
    exit 1
fi
sleep 2
if ! verify_same_runtime_resilient "after fresh ArtDmx post-interface recovery"; then
    exit 1
fi
record "same_runtime_process_after_fresh_artdmx: PASS (pid ${RUNTIME_PID}, starttime ${RUNTIME_STARTTIME})"

if [[ "$(mqtt_get "/devices/dmxwb/controls/source" || true)" != "artnet" ]]; then
    echo "Source изменился во время post-interface recovery." >&2
    exit 1
fi
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_artnet_output_active 1 >/dev/null

if ask_yes_no "После восстановления сети и отправки нового красного Art-Net светильник сейчас физически КРАСНЫЙ?"; then
    record "fresh_artdmx_red_after_interface_recovery_user: PASS"
else
    record "fresh_artdmx_red_after_interface_recovery_user: FAIL"
    exit 1
fi

# Safe final state is already MQTT OFF in background.
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 20 >/dev/null
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
    echo "После interface recovery финальный Art-Net source state должен быть ACTIVE." >&2
    exit 1
fi
record "final_artnet_source_active_after_interface_recovery: PASS"

LOST_EVENTS="$(final_value final_artnet_source_lost_events || true)"
SWITCHES="$(final_value final_router_source_switches || true)"
ARTNET_RX="$(final_value final_router_artnet_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
TRANSPORT_RECOVERIES="$(final_value final_artnet_transport_recoveries || true)"

for pair in \
    "LOST_EVENTS:${LOST_EVENTS}" \
    "SWITCHES:${SWITCHES}" \
    "ARTNET_RX:${ARTNET_RX}" \
    "DMX_FRAMES:${DMX_FRAMES}" \
    "TRANSPORT_RECOVERIES:${TRANSPORT_RECOVERIES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( LOST_EVENTS < 1 )); then
    echo "Runtime не зафиксировал Art-Net LOST во время 7 s interface outage." >&2
    exit 1
fi
if (( SWITCHES < 2 )); then
    echo "Ожидалось минимум 2 explicit source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( ARTNET_RX < 2 )); then
    echo "Недостаточно Art-Net snapshots до/после interface recovery: ${ARTNET_RX}." >&2
    exit 1
fi
if (( DMX_FRAMES <= FRAMES_AFTER )); then
    echo "Физические DMX frames не продолжились после post-recovery проверки." >&2
    exit 1
fi

record "artnet_source_lost_events: PASS (${LOST_EVENTS})"
record "source_switch_count: PASS (${SWITCHES})"
record "router_artnet_snapshots: PASS (${ARTNET_RX})"
record "physical_dmx_frames: PASS (${DMX_FRAMES})"
# A UDP socket bound to INADDR_ANY can remain valid while a Linux interface is
# administratively down; therefore transport_recoveries=0 is acceptable here.
record "artnet_transport_recoveries_observed: ${TRANSPORT_RECOVERIES}"

require_final_zero final_mqtt_callback_failures
require_final_zero final_mqtt_runtime_dmx_publish_failures
require_final_zero final_mqtt_runtime_state_save_failures
require_final_zero final_artnet_bind_failures
require_final_zero final_artnet_receive_errors

SEND_ERRORS="$(final_value final_artnet_send_errors || true)"
if [[ ! "${SEND_ERRORS}" =~ ^[0-9]+$ ]]; then
    echo "Некорректный final_artnet_send_errors='${SEND_ERRORS}'" >&2
    exit 1
fi
if (( SEND_ERRORS > 0 && TRANSPORT_RECOVERIES < 1 )); then
    echo "Art-Net send error был зафиксирован, но transport recovery отсутствует." >&2
    exit 1
fi
if (( SEND_ERRORS == 0 )); then
    record "final_artnet_send_errors: PASS (0; outage did not coincide with a pending PollReply)"
else
    record "final_artnet_send_errors: PASS (${SEND_ERRORS}; transient outage error recovered)"
fi

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
record "artnet_transport_lifecycle: PASS"

TEST_COMPLETE=1
record "dev010c1_interface_down_up_recovery_result: PASS"
record "=== DMXWB DEV-010C1 WB8 INTERFACE DOWN UP RECOVERY PASS ==="
echo
echo "=== DMXWB DEV-010C1 WB8 INTERFACE DOWN UP RECOVERY PASS ==="
echo "Report: ${REPORT}"
