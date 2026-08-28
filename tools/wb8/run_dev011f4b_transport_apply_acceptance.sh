#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev011f4b_transport_apply_acceptance.sh SSH_TARGET WEB_HOST [fixture-start-address]

Для текущего стенда:
  bash tools/wb8/run_dev011f4b_transport_apply_acceptance.sh root@10.200.200.1 10.200.200.1 1

Нужен один RGBW Fixture и доступ к обоим встроенным RS-485:
  /dev/ttyRS485-1
  /dev/ttyRS485-2

ВАЖНО:
  - RS485-1 и RS485-2 НЕ соединять параллельно в одну DMX-шину.
  - DMX A/B переставляется с одного порта на другой только по подсказке helper.
  - оба порта должны быть свободны от Serial Device Driver/других процессов.
  - QLC+/другой Art-Net OUTPUT должен быть полностью отключён.
  - ArtDmx для теста отправляет deterministic Windows PowerShell probe.

Ответы на визуальные вопросы: только латинские y или n.
USAGE
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
WEB_HOST="$2"
START_ADDRESS="${3:-1}"
PORT1="/dev/ttyRS485-1"
PORT2="/dev/ttyRS485-2"
UNIVERSE_INITIAL=0
UNIVERSE_TEST=17

if [[ ! "${WEB_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "WEB_HOST должен быть IPv4 address." >&2
    exit 2
fi
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_SCRIPT="${REPO_ROOT}/tools/wb8/build_dev010b_source_acceptance.sh"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-dev010-source-acceptance"
ARTDMX_PROBE="${REPO_ROOT}/tools/windows/send_dev011f4_artdmx.ps1"
REPORT="${REPO_ROOT}/docs/DEV011F4B_TRANSPORT_STRUCTURAL_APPLY_REPORT.txt"

REMOTE_DIR="/tmp/dmxwb-dev011f4b-transport"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev011f4b-ssh-${USER:-user}"
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

for command_name in ssh scp sha256sum awk grep git sed tail tr seq tar python3 wslpath powershell.exe; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

if [[ ! -f "${ARTDMX_PROBE}" ]]; then
    echo "Не найден ${ARTDMX_PROBE}" >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPENED=0
RUNTIME_PID=""
RUNTIME_STARTED=0
RETAINED_CAPTURED=0
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

config_probe() {
    local code="$1"
    mqtt_get "/dmxwb/config" | python3 -c "${code}"
}

config_revision() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["revision"])'
}

config_dmx_port() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["dmx"]["port"])'
}

config_artnet_universe() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["artnet"]["universe"])'
}

wait_config_value() {
    local getter="$1"
    local expected="$2"
    local attempts="${3:-50}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(${getter} 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидалось ${getter}='${expected}', получено '${value}'" >&2
    return 1
}

state_fixture_field() {
    local field="$1"
    mqtt_get "/dmxwb/state" | python3 -c '
import json,sys
field=sys.argv[1]
d=json.load(sys.stdin)
f=next((x for x in d.get("fixtures",[]) if x.get("id")==1),None)
if f is None or field not in f:
    raise SystemExit(2)
v=f[field]
if isinstance(v,bool):
    print("true" if v else "false")
else:
    print(v)
' "${field}"
}

wait_state_field() {
    local field="$1"
    local expected="$2"
    local attempts="${3:-50}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(state_fixture_field "${field}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "Fixture 1: ожидалось ${field}='${expected}', получено '${value}'" >&2
    return 1
}

wait_topic() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-50}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${topic}='${expected}', получено '${value}'" >&2
    return 1
}

latest_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
}

wait_runtime_value() {
    local key="$1"
    local expected="$2"
    local attempts="${3:-60}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key}='${expected}', последнее '${value}'" >&2
    return 1
}

wait_runtime_uint_ge() {
    local key="$1"
    local minimum="$2"
    local attempts="${3:-60}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" =~ ^[0-9]+$ ]] && (( value >= minimum )); then
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key} >= ${minimum}, последнее '${value}'" >&2
    return 1
}

wait_log_exact() {
    local expected="$1"
    local attempts="${2:-60}"
    for ((i=1; i<=attempts; ++i)); do
        if remote "grep -Fqx '${expected}' '${REMOTE_DIR}/runtime.log' 2>/dev/null"; then
            return 0
        fi
        sleep 0.2
    done
    echo "Не появилась строка runtime log: ${expected}" >&2
    return 1
}

assert_runtime_alive() {
    if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
        echo "Acceptance runtime завершился; ожидался тот же PID ${RUNTIME_PID}." >&2
        remote "tail -n 120 '${REMOTE_DIR}/runtime.log'" >&2 || true
        return 1
    fi
}

send_artdmx() {
    local port_address="$1"
    local red="$2"
    local green="$3"
    local blue="$4"
    local white="${5:-0}"
    local windows_probe
    windows_probe="$(wslpath -w "${ARTDMX_PROBE}")"
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "${windows_probe}" \
        -Target "${WEB_HOST}" \
        -PortAddress "${port_address}" \
        -Red "${red}" \
        -Green "${green}" \
        -Blue "${blue}" \
        -White "${white}" \
        -Count 40 \
        -IntervalMs 25 | tr -d '\r'
}

final_value() {
    latest_value "$1"
}

require_final_zero() {
    local key="$1"
    local value
    value="$(final_value "${key}" || true)"
    if [[ "${value}" != "0" ]]; then
        echo "Ожидался ${key}=0, получено '${value}'" >&2
        exit 1
    fi
    record "${key}: PASS (0)"
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
if timeout 0.45 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '${topic}' > '${REMOTE_DIR}/retained-backup/${index}.payload' 2>/dev/null; then
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
if [ \"\$(cat '${REMOTE_DIR}/retained-backup/${index}.present' 2>/dev/null)\" = 1 ]; then
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
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 0.4" >/dev/null 2>&1 || true
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
{"version":1,"revision":1,"dmx":{"port":"${PORT1}"},"artnet":{"universe":${UNIVERSE_INITIAL}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"F4 Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-011F4B REAL TRANSPORT STRUCTURAL APPLY ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "target: ${TARGET}"
record "web_host: ${WEB_HOST}"
record "fixture_start_address: ${START_ADDRESS}"
record "port_initial: ${PORT1}"
record "port_test: ${PORT2}"
record "artnet_initial: ${UNIVERSE_INITIAL}"
record "artnet_test: ${UNIVERSE_TEST}"

echo "DMXWB — DEV-011F4B transport structural Apply acceptance"
echo
echo "Этот тест НЕ добавляет diagnostics. Он проверяет только реальное применение:"
echo "  DMX Port: ${PORT1} -> ${PORT2} -> ${PORT1}"
echo "  Art-Net Universe: ${UNIVERSE_INITIAL} -> ${UNIVERSE_TEST} -> ${UNIVERSE_INITIAL}"
echo
echo "RS485-1 и RS485-2 нельзя соединять друг с другом."
echo "Одна DMX-линия физически переставляется между портами по подсказкам helper."
echo

if ! ask_yes_no "Один RGBW Fixture настроен на Start Address ${START_ADDRESS} и сейчас подключён ТОЛЬКО к ${PORT1}?"; then
    exit 2
fi
if ! ask_yes_no "Оба RS-485 порта свободны от Serial Device Driver/других приложений?"; then
    echo "Освободите оба порта перед F4B." >&2
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT полностью отключён?"; then
    echo "F4B использует только deterministic ArtDmx probe." >&2
    exit 2
fi

echo
echo "=== Local checks/build ==="
python3 "${REPO_ROOT}/tools/web/check_dev011f4_runtime_config_apply.py" | tee -a "${REPORT}"
record "local_f4_static: PASS"

bash "${BUILD_SCRIPT}" | tee -a "${REPORT}"
if [[ ! -x "${BINARY}" ]]; then
    echo "Target artifact не создан: ${BINARY}" >&2
    exit 1
fi
record "bullseye_arm64_build: PASS"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"

echo
echo "=== WB8 preflight ==="
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl ip nginx curl tar grep sed tail readlink python3; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet nginx"
remote "systemctl is-active --quiet mosquitto"
remote "nginx -t" 2>&1 | tee -a "${REPORT}"

remote "test -e '${PORT1}' && test -e '${PORT2}'"
PORT1_REAL="$(remote "readlink -f '${PORT1}'" | tr -d '\r')"
PORT2_REAL="$(remote "readlink -f '${PORT2}'" | tr -d '\r')"
if [[ -z "${PORT1_REAL}" || -z "${PORT2_REAL}" || "${PORT1_REAL}" == "${PORT2_REAL}" ]]; then
    echo "RS-485 port mapping некорректен: '${PORT1_REAL}' / '${PORT2_REAL}'" >&2
    exit 1
fi
record "port1_resolves: ${PORT1_REAL}"
record "port2_resolves: ${PORT2_REAL}"

RUNNING_DMXWB="$(remote '
for exe in /proc/[0-9]*/exe; do
    target="$(readlink "${exe}" 2>/dev/null || true)"
    name="${target##*/}"
    case "${name}" in
        dmxwb|dmxwb-dev010-source-acceptance)
            pid="${exe#/proc/}"
            pid="${pid%/exe}"
            printf "%s %s\n" "${pid}" "${target}"
            ;;
    esac
done
' | tr -d '\r')"
if [[ -n "${RUNNING_DMXWB}" ]]; then
    echo "На WB8 уже запущен DMXWB/acceptance executable:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
record "no_running_dmxwb_process: PASS"

WB_INTERFACE="$(remote "ip -o -4 addr show | awk -v ip='${WEB_HOST}' '{split(\$4,a,\"/\"); if(a[1]==ip){print \$2; exit}}'" | tr -d '\r')"
if [[ -z "${WB_INTERFACE}" ]]; then
    echo "Не найден WB8 interface с IPv4 ${WEB_HOST}." >&2
    exit 1
fi
WB_MAC="$(remote "cat '/sys/class/net/${WB_INTERFACE}/address'" | tr -d '\r')"
record "wb8_interface: ${WB_INTERFACE}"
record "wb8_mac: ${WB_MAC}"
record "wb8_arch: $(remote 'uname -m' | tr -d '\r')"
record "wb8_kernel: $(remote 'uname -r' | tr -d '\r')"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
echo "Сохраняем исходные retained topics..."
capture_retained
record "original_retained_topics_captured: PASS"

scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance'"

echo "Разворачиваем текущий static Web..."
tar -C "${REPO_ROOT}/www" -czf - dmxwb | remote "rm -rf /var/www/dmxwb && tar -m -C /var/www -xzf -"
remote "find /var/www/dmxwb -type f -exec chmod 0644 {} +; find /var/www/dmxwb -type d -exec chmod 0755 {} +"
remote "curl -fsS http://127.0.0.1/dmxwb/ | grep -q '<title>DMXWB</title>'"
record "wb_static_web_deployed: PASS"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-dev010-source-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' --development-oem-code FFFF --mac '${WB_MAC}' --status-interval-ms 200 >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2
assert_runtime_alive
record "runtime_pid: ${RUNTIME_PID}"

wait_topic "/devices/dmxwb/controls/status" "running"
wait_topic "/devices/dmxwb/controls/source" "mqtt"
wait_config_value config_revision 1
wait_config_value config_dmx_port "${PORT1}"
wait_config_value config_artnet_universe "${UNIVERSE_INITIAL}"
wait_runtime_value status_selected_source mqtt
wait_runtime_value status_dmx_output_running 1
wait_runtime_uint_ge status_dmx_frames_sent 5
record "initial_runtime_port1_universe0: PASS"

echo
echo "=== Browser baseline ==="
echo "Откройте штатный WB UI как Administrator, затем:"
echo "  http://${WEB_HOST}/dmxwb/"
echo
echo "В Settings ожидается:"
echo "  DMX Port = ${PORT1}"
echo "  Art-Net Universe = ${UNIVERSE_INITIAL}"
if ! ask_yes_no "Web подключён и показывает исходную конфигурацию?"; then
    record "browser_initial_config_user: FAIL"
    exit 1
fi
record "browser_initial_config_user: PASS"

echo "У Fixture включите Power, Color = RED (#ff0000), Brightness = 100."
if ! ask_yes_no "Fixture физически КРАСНЫЙ через ${PORT1}?"; then
    record "port1_initial_red_user: FAIL"
    exit 1
fi
wait_state_field requested_power true
wait_state_field red 255
wait_state_field green 0
wait_state_field blue 0
record "port1_initial_red_factual: PASS"

echo
echo "=== DMX Port Apply: port1 -> port2 ==="
echo "В Settings выберите ${PORT2} и нажмите «Применить»."
if ! ask_yes_no "Web показал успешное применение revision 2?"; then
    record "port2_apply_user: FAIL"
    exit 1
fi
wait_config_value config_revision 2
wait_config_value config_dmx_port "${PORT2}"
wait_log_exact "runtime_dmx_port_applied: ${PORT2}"
assert_runtime_alive
wait_runtime_value status_dmx_output_running 1
wait_runtime_uint_ge status_dmx_frames_sent 5
record "same_pid_port2_runtime_apply: PASS"

echo
echo "Сейчас runtime уже владеет ${PORT2}."
echo "ФИЗИЧЕСКИ переставьте DMX A/B с ${PORT1} на ${PORT2}."
echo "Не соединяйте два RS-485 порта параллельно."
if ! ask_yes_no "После перестановки Fixture получает DMX через ${PORT2}?"; then
    record "port2_line_present_user: FAIL"
    exit 1
fi

echo "Через Web измените Fixture на GREEN (#00ff00)."
if ! ask_yes_no "Fixture физически стал ЗЕЛЁНЫМ через ${PORT2}?"; then
    record "port2_green_user: FAIL"
    exit 1
fi
wait_state_field red 0
wait_state_field green 255
wait_state_field blue 0
assert_runtime_alive
record "port2_live_control_physical: PASS"

echo
echo "=== DMX Port Apply: port2 -> port1 ==="
echo "В Settings верните ${PORT1} и нажмите «Применить»."
if ! ask_yes_no "Web показал успешное применение revision 3?"; then
    record "port1_restore_apply_user: FAIL"
    exit 1
fi
wait_config_value config_revision 3
wait_config_value config_dmx_port "${PORT1}"
wait_log_exact "runtime_dmx_port_applied: ${PORT1}"
assert_runtime_alive
wait_runtime_value status_dmx_output_running 1
wait_runtime_uint_ge status_dmx_frames_sent 5
record "same_pid_port1_restore_runtime_apply: PASS"

echo "Переставьте DMX A/B с ${PORT2} обратно на ${PORT1}."
if ! ask_yes_no "Fixture снова получает DMX через ${PORT1}?"; then
    record "port1_restore_line_present_user: FAIL"
    exit 1
fi

echo "Через Web измените Fixture на BLUE (#0000ff)."
if ! ask_yes_no "Fixture физически стал СИНИМ через ${PORT1}?"; then
    record "port1_blue_after_restore_user: FAIL"
    exit 1
fi
wait_state_field red 0
wait_state_field green 0
wait_state_field blue 255
record "dmx_port_round_trip_physical: PASS"

echo
echo "=== Art-Net Universe baseline ==="
echo "Helper отправит GREEN ArtDmx в Universe 0, пока Source остаётся WB MQTT."
send_artdmx 0 0 255 0 0
wait_runtime_value status_has_artnet_snapshot 1
assert_runtime_alive
if ! ask_yes_no "Fixture остался СИНИМ (background Art-Net не перехватил Source)?"; then
    record "universe0_background_no_takeover_user: FAIL"
    exit 1
fi
record "universe0_background_snapshot: PASS"

echo "Helper переключит Source на ART-NET через штатную non-retained MQTT command."
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
wait_topic "/devices/dmxwb/controls/source" "artnet"
assert_runtime_alive
record "source_artnet_factual: PASS"

if ! ask_yes_no "Fixture стал ЗЕЛЁНЫМ от Universe 0?"; then
    record "universe0_selected_green_user: FAIL"
    exit 1
fi
record "universe0_selected_physical: PASS"

echo "Helper детерминированно задаст background WB MQTT Fixture = RED,"
echo "пока физический Source остаётся ART-NET."
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'"
wait_state_field red 255
wait_state_field green 0
wait_state_field blue 0
assert_runtime_alive
record "mqtt_background_red_factual: PASS"

if ! ask_yes_no "Физически Fixture остался ЗЕЛЁНЫМ (background MQTT не перехватил ART-NET)?"; then
    record "mqtt_background_while_artnet_user: FAIL"
    exit 1
fi
record "mqtt_background_while_artnet_user: PASS"

echo
echo "=== Art-Net Universe Apply: 0 -> 17 ==="
echo "В Settings установите Art-Net Universe = ${UNIVERSE_TEST} и нажмите «Применить»."
if ! ask_yes_no "Web показал успешное применение revision 4?"; then
    record "universe17_apply_user: FAIL"
    exit 1
fi
wait_config_value config_revision 4
wait_config_value config_artnet_universe "${UNIVERSE_TEST}"
wait_log_exact "runtime_artnet_port_address_applied: ${UNIVERSE_TEST}"
wait_runtime_value status_has_artnet_snapshot 0
assert_runtime_alive
record "same_pid_universe17_runtime_apply: PASS"

if ! ask_yes_no "После Apply физический Fixture остался ЗЕЛЁНЫМ (Hold Last, без blackout)?"; then
    record "universe_change_hold_last_user: FAIL"
    exit 1
fi
record "universe_change_hold_last_user: PASS"

echo "Helper отправит BLUE в СТАРЫЙ Universe 0. Он должен быть проигнорирован."
send_artdmx 0 0 0 255 0
wait_runtime_value status_has_artnet_snapshot 0
assert_runtime_alive
if ! ask_yes_no "Fixture всё ещё ЗЕЛЁНЫЙ; старый Universe 0 не управляет выходом?"; then
    record "old_universe_rejected_user: FAIL"
    exit 1
fi
record "old_universe_rejected_factual: PASS"

echo "Helper отправит BLUE в НОВЫЙ Universe ${UNIVERSE_TEST}."
send_artdmx "${UNIVERSE_TEST}" 0 0 255 0
wait_runtime_value status_has_artnet_snapshot 1
assert_runtime_alive
if ! ask_yes_no "Fixture физически стал СИНИМ от Universe ${UNIVERSE_TEST}?"; then
    record "new_universe_blue_user: FAIL"
    exit 1
fi
record "new_universe_selected_physical: PASS"

echo "Helper переключит Source обратно на WB MQTT через штатную non-retained MQTT command."
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt"
wait_topic "/devices/dmxwb/controls/source" "mqtt"
assert_runtime_alive
record "source_mqtt_factual: PASS"

if ! ask_yes_no "Fixture стал КРАСНЫМ — текущий background MQTT state?"; then
    record "artnet_to_mqtt_after_reconfigure_user: FAIL"
    exit 1
fi
record "artnet_to_mqtt_after_reconfigure: PASS"

echo
echo "=== Restore Art-Net Universe 17 -> 0 ==="
echo "В Settings верните Art-Net Universe = 0 и нажмите «Применить»."
if ! ask_yes_no "Web показал успешное применение revision 5?"; then
    record "universe0_restore_apply_user: FAIL"
    exit 1
fi
wait_config_value config_revision 5
wait_config_value config_artnet_universe 0
wait_log_exact "runtime_artnet_port_address_applied: 0"
wait_runtime_value status_has_artnet_snapshot 0
assert_runtime_alive
record "same_pid_universe0_restore_runtime_apply: PASS"

echo "Выключите Fixture Power через Web."
if ! ask_yes_no "Fixture физически OFF?"; then
    record "final_power_off_user: FAIL"
    exit 1
fi
wait_state_field requested_power false
record "final_power_off_factual: PASS"

echo
echo "=== Final same-process diagnostics ==="
assert_runtime_alive
record "same_runtime_pid_through_all_reconfiguration: PASS (${RUNTIME_PID})"

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
RUNTIME_STARTED=0

record "--- final runtime diagnostics ---"
remote "grep -E '^(final_|state_flush_action:|software_result:)' '${REMOTE_DIR}/runtime.log'" | tee -a "${REPORT}"

if [[ "$(final_value software_result || true)" != "PASS" ]]; then
    echo "software_result != PASS." >&2
    exit 1
fi
if [[ "$(final_value final_selected_source || true)" != "mqtt" ]]; then
    echo "Финальный Source должен быть mqtt." >&2
    exit 1
fi
if [[ "$(final_value final_applied_dmx_port || true)" != "${PORT1}" ]]; then
    echo "Финальный applied DMX port не ${PORT1}." >&2
    exit 1
fi
if [[ "$(final_value final_applied_artnet_port_address || true)" != "0" ]]; then
    echo "Финальный applied Art-Net Port-Address не 0." >&2
    exit 1
fi
if [[ "$(final_value final_dmx_port_reconfigurations || true)" != "2" ]]; then
    echo "Ожидалось ровно 2 DMX port reconfiguration." >&2
    exit 1
fi
if [[ "$(final_value final_artnet_universe_reconfigurations || true)" != "2" ]]; then
    echo "Ожидалось ровно 2 Art-Net universe reconfiguration." >&2
    exit 1
fi
if [[ "$(final_value final_dmx_active_refresh_hz || true)" != "44" ]]; then
    echo "Финальный DMX refresh должен быть 44 Hz." >&2
    exit 1
fi

require_final_zero final_dmx_port_reconfigure_failures
require_final_zero final_artnet_universe_reconfigure_failures
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

record "dmx_port_round_trip_same_process: PASS"
record "artnet_universe_round_trip_same_process: PASS"
record "old_universe_never_replayed: PASS"
record "universe_change_no_blackout: PASS"
record "fixed_44hz_after_reconfiguration: PASS"

echo "Восстанавливаем исходные retained topics..."
restore_retained
record "original_retained_topics_restored: PASS"
record "dev011f4b_real_transport_structural_apply_result: PASS"
record "=== DMXWB DEV-011F4B REAL DMX PORT + ART-NET UNIVERSE APPLY PASS ==="

echo
echo "=== DMXWB DEV-011F4B REAL DMX PORT + ART-NET UNIVERSE APPLY PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
