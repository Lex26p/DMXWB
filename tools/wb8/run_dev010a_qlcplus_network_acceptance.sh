#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev010a_qlcplus_network_acceptance.sh user@wb8-host QLC_PLUS_PC_IP [port-address]

DEV-010A network-only acceptance с QLC+:
  - реальный UDP 6454 на WB8;
  - ArtPoll -> randomized unicast ArtPollReply;
  - QLC+ Nodes Tree discovery;
  - unicast ArtDmx QLC+ -> WB8;
  - точная проверка DMX channels 1..4;
  - реальный 3 s LOST + Hold Last;
  - восстановление того же QLC+ source без restart DMXWB runtime.

Этот шаг НЕ подключает Art-Net к физическому RS-485 DMX и НЕ переключает
application Source. Для PollReply используется явно development-only OEM sentinel
0xFFFF; он не является production OEM assignment.
USAGE
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
QLC_IP="$2"
PORT_ADDRESS="${3:-0}"

if [[ ! "${QLC_IP}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "QLC_PLUS_PC_IP должен быть IPv4 address." >&2
    exit 2
fi
if [[ ! "${PORT_ADDRESS}" =~ ^[0-9]+$ ]] || (( PORT_ADDRESS < 0 || PORT_ADDRESS > 32767 )); then
    echo "port-address должен быть целым числом 0..32767." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-artnet-acceptance"
REPORT="${REPO_ROOT}/docs/DEV010A_ARTNET_QLCPLUS_NETWORK_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev010a-artnet"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev010a-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

for command_name in ssh scp sha256sum awk grep git sed tail; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Не найден ${BINARY}" >&2
    echo "Сначала выполните: bash tools/wb8/build_dev010a_artnet_acceptance.sh" >&2
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
        if [[ -n "${RUNTIME_PID}" ]]; then
            remote "kill -TERM '${RUNTIME_PID}' 2>/dev/null || true; sleep 1; kill -KILL '${RUNTIME_PID}' 2>/dev/null || true" >/dev/null 2>&1 || true
        fi
        if (( RUNTIME_STARTED == 1 && TEST_COMPLETE == 0 )); then
            record "--- runtime.log after incomplete acceptance ---" || true
            remote "cat '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tee -a "${REPORT}" >/dev/null || true
        fi
        remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi
    rm -rf "${CONTROL_DIR}" >/dev/null 2>&1 || true
    exit "${status}"
}
trap cleanup EXIT INT TERM

latest_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
}

latest_channels() {
    remote "grep '^snapshot_channels_1_8:' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^snapshot_channels_1_8: //'" | tr -d '\r'
}

wait_channels() {
    local e1="$1" e2="$2" e3="$3" e4="$4" attempts="${5:-25}"
    local line="" c1="" c2="" c3="" c4="" rest=""
    for ((i=1; i<=attempts; ++i)); do
        line="$(latest_channels || true)"
        read -r c1 c2 c3 c4 rest <<<"${line}"
        if [[ "${c1}" == "${e1}" && "${c2}" == "${e2}" && "${c3}" == "${e3}" && "${c4}" == "${e4}" ]]; then
            printf '%s' "${line}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидались channels 1..4 = ${e1} ${e2} ${e3} ${e4}; последнее: '${line}'" >&2
    return 1
}

wait_source_state() {
    local expected="$1" attempts="${2:-30}" state=""
    for ((i=1; i<=attempts; ++i)); do
        state="$(latest_value status_source_state || true)"
        if [[ "${state}" == "${expected}" ]]; then
            printf '%s' "${state}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался source state ${expected}; последнее: '${state}'" >&2
    return 1
}

: > "${REPORT}"
record "=== DMXWB DEV-010A QLC+ Art-Net network acceptance ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "qlc_plus_pc_ip: ${QLC_IP}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "development_oem_placeholder: 0xFFFF"
record "physical_dmx_connected: no"
record "source_switching_connected: no"

echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

ROUTE_LINE="$(remote "ip -4 route get '${QLC_IP}' | head -n1" | tr -d '\r')"
WB_INTERFACE="$(awk '{for(i=1;i<=NF;i++) if($i=="dev" && i<NF){print $(i+1); exit}}' <<<"${ROUTE_LINE}")"
WB_IP="$(awk '{for(i=1;i<=NF;i++) if($i=="src" && i<NF){print $(i+1); exit}}' <<<"${ROUTE_LINE}")"
if [[ -z "${WB_INTERFACE}" ]]; then
    echo "Не удалось определить WB8 interface по route: ${ROUTE_LINE}" >&2
    exit 1
fi
if [[ -z "${WB_IP}" ]]; then
    WB_IP="$(remote "ip -4 -o addr show dev '${WB_INTERFACE}' scope global | awk '{print \$4}' | cut -d/ -f1 | head -n1" | tr -d '\r')"
fi
if [[ -z "${WB_IP}" ]]; then
    echo "Не удалось определить IPv4 WB8 interface ${WB_INTERFACE}." >&2
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
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_DIR}/dmxwb-artnet-acceptance" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-artnet-acceptance'"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-artnet-acceptance' --port-address '${PORT_ADDRESS}' --development-oem-code FFFF --mac '${WB_MAC}' >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2
if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Art-Net acceptance runtime завершился раньше времени:" >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi

if [[ "$(latest_value status_transport_open || true)" != "1" ]]; then
    echo "UDP 6454 transport не открылся:" >&2
    remote "tail -n 40 '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
record "udp_6454_open: PASS"

echo
echo "=== QLC+ настройка ==="
echo "WB8 IP:             ${WB_IP}"
echo "QLC+ PC IP:         ${QLC_IP}"
echo "Art-Net Port-Addr:  ${PORT_ADDRESS}"
echo
echo "В QLC+ выберите Universe 1 и назначьте Art-Net OUTPUT на сетевой интерфейс,"
echo "через который доступен WB8. В Art-Net configuration выставьте:"
echo "  IP Address / destination: ${WB_IP}"
echo "  Art-Net Universe:         ${PORT_ADDRESS}"
echo "  Transmission Mode:        Standard (для этого acceptance достаточно)"
echo
echo "Откройте Art-Net plugin configuration -> Nodes Tree."
echo "Узел с именем DMXWB DEV010A должен появиться на интерфейсе этой сети."
echo "Если Windows спросит firewall permission для QLC+, разрешите Private network."
echo
if ! ask_yes_no "QLC+ Nodes Tree показывает DMXWB DEV010A?"; then
    record "qlc_nodes_tree_discovery_user: FAIL"
    exit 1
fi
record "qlc_nodes_tree_discovery_user: PASS"
sleep 1
POLL_REPLIES="$(latest_value status_poll_replies_sent || true)"
if [[ ! "${POLL_REPLIES}" =~ ^[0-9]+$ ]] || (( POLL_REPLIES < 1 )); then
    echo "QLC+ discovery отмечен пользователем, но runtime не зафиксировал отправленный PollReply." >&2
    exit 1
fi
record "poll_reply_unicast_sent: PASS (${POLL_REPLIES})"

echo
echo "=== QLC+ ArtDmx exact values ==="
echo "Откройте Simple Desk для Universe 1 и выставьте:"
echo "  Channel 1 = 17"
echo "  Channel 2 = 34"
echo "  Channel 3 = 51"
echo "  Channel 4 = 68"
echo "Остальные каналы для этой проверки не важны."
if ! ask_yes_no "QLC+ показывает эти четыре значения и Art-Net output включён?"; then
    record "qlc_first_pattern_user: FAIL"
    exit 1
fi
FIRST_LINE="$(wait_channels 17 34 51 68)"
record "artdmx_first_pattern: PASS (${FIRST_LINE})"
if [[ "$(wait_source_state ACTIVE)" != "ACTIVE" ]]; then
    exit 1
fi
record "artnet_source_active: PASS"

echo
echo "=== Реальный LOST / Hold Last ==="
echo "Временно отключите Art-Net OUTPUT для Universe 1 (или снимите Art-Net output patch)."
echo "Не закрывайте этот script. После прекращения ArtDmx ждём больше 3 секунд."
if ! ask_yes_no "Art-Net output из QLC+ сейчас отключён?"; then
    record "qlc_output_disable_user: FAIL"
    exit 1
fi
sleep 4
if [[ "$(wait_source_state LOST 10)" != "LOST" ]]; then
    exit 1
fi
record "real_network_source_lost_after_3s: PASS"
HOLD_LINE="$(latest_channels || true)"
read -r hc1 hc2 hc3 hc4 _ <<<"${HOLD_LINE}"
if [[ "${hc1}" != "17" || "${hc2}" != "34" || "${hc3}" != "51" || "${hc4}" != "68" ]]; then
    echo "Hold Last нарушен после LOST: '${HOLD_LINE}'" >&2
    exit 1
fi
record "real_network_hold_last: PASS (${HOLD_LINE})"

echo
echo "=== Recovery без restart ==="
echo "Снова включите тот же Art-Net OUTPUT в QLC+ и выставьте:"
echo "  Channel 1 = 101"
echo "  Channel 2 = 102"
echo "  Channel 3 = 103"
echo "  Channel 4 = 104"
if ! ask_yes_no "Art-Net output снова включён и значения 101/102/103/104 выставлены?"; then
    record "qlc_recovery_pattern_user: FAIL"
    exit 1
fi
RECOVERY_LINE="$(wait_channels 101 102 103 104)"
record "artdmx_recovery_pattern: PASS (${RECOVERY_LINE})"
if [[ "$(wait_source_state ACTIVE)" != "ACTIVE" ]]; then
    exit 1
fi
record "same_runtime_recovery_to_active: PASS"

remote "kill -TERM '${RUNTIME_PID}'"
for _ in $(seq 1 50); do
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
remote "grep '^final_' '${REMOTE_DIR}/runtime.log'" | tee -a "${REPORT}"

CORE_REJECTIONS="$(remote "grep '^final_core_rejections:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
BIND_FAILURES="$(remote "grep '^final_bind_failures:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
RECEIVE_ERRORS="$(remote "grep '^final_receive_errors:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
SEND_ERRORS="$(remote "grep '^final_send_errors:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
LOST_EVENTS="$(remote "grep '^final_source_lost_events:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
FINAL_POLL="$(remote "grep '^final_poll_replies_sent:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"
SNAPSHOTS="$(remote "grep '^final_snapshots_published:' '${REMOTE_DIR}/runtime.log' | tail -n1 | awk '{print \$2}'" | tr -d '\r')"

for value_name in CORE_REJECTIONS BIND_FAILURES RECEIVE_ERRORS SEND_ERRORS LOST_EVENTS FINAL_POLL SNAPSHOTS; do
    value="${!value_name}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Не удалось прочитать ${value_name}: '${value}'" >&2
        exit 1
    fi
done

if (( CORE_REJECTIONS != 0 || BIND_FAILURES != 0 || RECEIVE_ERRORS != 0 || SEND_ERRORS != 0 )); then
    echo "Runtime diagnostics contain errors." >&2
    exit 1
fi
if (( LOST_EVENTS < 1 || FINAL_POLL < 1 || SNAPSHOTS < 2 )); then
    echo "Недостаточно acceptance events: lost=${LOST_EVENTS}, poll=${FINAL_POLL}, snapshots=${SNAPSHOTS}" >&2
    exit 1
fi

record "runtime_error_counters_zero: PASS"
record "runtime_required_events: PASS"
record "dev010a_qlcplus_network_result: PASS"
record "=== DMXWB DEV-010A QLC+ ART-NET NETWORK PASS ==="
TEST_COMPLETE=1

echo
echo "=== DMXWB DEV-010A QLC+ ART-NET NETWORK PASS ==="
echo "Report: ${REPORT}"
