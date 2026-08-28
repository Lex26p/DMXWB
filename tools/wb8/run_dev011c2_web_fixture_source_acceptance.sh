#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev011c2_web_fixture_source_acceptance.sh SSH_TARGET WEB_HOST [fixture-start-address]

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  WEB_HOST   = 10.200.200.1
  fixture-start-address = 1

DEV-011C2 проверяет реальный путь:
Browser -> nginx /mqtt -> Mosquitto -> DMXWB runtime -> DmxOutput -> RGBW fixture.

Также проверяется Web Source selector без ArtDmx:
MQTT BLUE
-> Web Source=ART-NET, physical BLUE Hold Current
-> Web меняет inactive MQTT logical state на RED
-> physical остаётся BLUE
-> Web Source=WB MQTT
-> physical становится RED.

QLC+/другой Art-Net OUTPUT на время этого теста должен быть полностью отключён.
Глобальный Blackout/зелёный глаз QLC+ для этого не использовать.
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
PORT="/dev/ttyRS485-1"
PORT_ADDRESS=0

if [[ ! "${WEB_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "WEB_HOST должен быть IPv4 address." >&2
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
REPORT="${REPO_ROOT}/docs/DEV011C2_WEB_FIXTURE_SOURCE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev011c2-web-fixture-source"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev011c2-ssh-${USER:-user}"
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

for command_name in ssh scp sha256sum awk grep git sed tail tr seq tar python3; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Не найден executable ${BINARY}" >&2
    echo "Сначала выполните: bash tools/wb8/build_dev010b_source_acceptance.sh" >&2
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
    local attempts="${3:-30}"
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
    local attempts="${3:-30}"
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

config_fixture_name() {
    mqtt_get "/dmxwb/config" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
items = doc.get("fixtures", {}).get("items", [])
item = next((x for x in items if x.get("id") == 1), None)
if item is None:
    raise SystemExit(2)
print(item.get("name", ""))
'
}

wait_config_name() {
    local expected="$1"
    local attempts="${2:-30}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(config_fixture_name 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидалось имя Fixture '${expected}', получено '${value}'" >&2
    return 1
}

latest_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
}

wait_runtime_value() {
    local key="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key}=${expected}, последнее '${value}'" >&2
    remote "tail -n 100 '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" >&2
    return 1
}

wait_runtime_uint_ge() {
    local key="$1"
    local minimum="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" =~ ^[0-9]+$ ]] && (( value >= minimum )); then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key} >= ${minimum}, последнее '${value}'" >&2
    return 1
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

# Topics modified by this one-Fixture acceptance runtime.
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
                # Best-effort safe final MQTT OFF; this is cleanup, not acceptance evidence.
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV011 C2 Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-011C2 REAL WEB FIXTURE + SOURCE ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "web_host: ${WEB_HOST}"
record "web_url: http://${WEB_HOST}/dmxwb/"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "fixture_channels_rgbw: ${R}/${G}/${B}/${W}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-011C2 browser Fixture/Source physical acceptance"
echo "WB8/Web:          ${WEB_HOST}"
echo "Fixture RGBW:     channels ${R}/${G}/${B}/${W}"
echo "DMX port:         ${PORT}"
echo

if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    echo "Для C2 нужен запуск без process-local ArtDmx snapshot." >&2
    exit 2
fi

echo
echo "=== Local checks ==="
python3 "${REPO_ROOT}/tools/web/check_dev011c1_fixture_controls.py" | tee -a "${REPORT}"
record "local_dev011c1_checks: PASS"

echo
echo "=== WB8 preflight ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl ip nginx curl tar grep sed tail readlink; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet nginx"
remote "systemctl is-active --quiet mosquitto"
remote "nginx -t" 2>&1 | tee -a "${REPORT}"

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
    echo "На WB8 уже запущен DMXWB/acceptance executable. Для C2 он должен быть остановлен:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
record "no_running_dmxwb_process: PASS"

if ! remote "test -e '${PORT}'"; then
    echo "На WB8 отсутствует ${PORT}." >&2
    exit 1
fi

WB_INTERFACE="$(remote "ip -o -4 addr show | awk -v ip='${WEB_HOST}' '{split(\$4,a,\"/\"); if(a[1]==ip){print \$2; exit}}'" | tr -d '\r')"
if [[ -z "${WB_INTERFACE}" ]]; then
    echo "Не найден WB8 interface с IPv4 ${WEB_HOST}." >&2
    exit 1
fi
WB_MAC="$(remote "cat '/sys/class/net/${WB_INTERFACE}/address'" | tr -d '\r')"
if [[ ! "${WB_MAC}" =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]]; then
    echo "Некорректный MAC interface ${WB_INTERFACE}: '${WB_MAC}'" >&2
    exit 1
fi
record "wb8_interface: ${WB_INTERFACE}"
record "wb8_mac: ${WB_MAC}"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
echo "Сохраняем retained topics, которые затронет acceptance..."
capture_retained
record "original_retained_topics_captured: PASS"

scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance'"

echo "Разворачиваем текущий web в /var/www/dmxwb..."
tar -C "${REPO_ROOT}/www" -czf - dmxwb | remote "rm -rf /var/www/dmxwb && tar -m -C /var/www -xzf -"
remote "find /var/www/dmxwb -type f -exec chmod 0644 {} +; find /var/www/dmxwb -type d -exec chmod 0755 {} +"
if ! remote "curl -fsS http://127.0.0.1/dmxwb/ | grep -q '<title>DMXWB</title>'"; then
    echo "nginx не отдаёт текущий /dmxwb/." >&2
    exit 1
fi
record "wb_static_web_deployed: PASS"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-dev010-source-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' --development-oem-code FFFF --mac '${WB_MAC}' --status-interval-ms 250 >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2

if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    echo "Unified runtime завершился раньше времени:" >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
if [[ "$(remote "grep -c '^runtime_started: PASS$' '${REMOTE_DIR}/runtime.log' 2>/dev/null || true" | tr -d '\r')" == "0" ]]; then
    echo "runtime_started: PASS не найден." >&2
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi

wait_topic "/devices/dmxwb/controls/status" "running" 30 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" 30 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_runtime_value status_has_mqtt_snapshot 1 >/dev/null
wait_runtime_value status_has_artnet_snapshot 0 >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "unified_runtime_started: PASS"
record "initial_source_mqtt: PASS"
record "no_process_local_artnet_snapshot: PASS"

echo
echo "=== Browser initial state ==="
echo "1. Откройте обычный Web UI WB8 и войдите как Administrator."
echo "2. Затем откройте/обновите один раз:"
echo "     http://${WEB_HOST}/dmxwb/"
echo "   Если WB переводит на HTTPS — оставайтесь на HTTPS."
echo
echo "Ожидается: «Связь установлена», Source=WB MQTT, один Fixture «DEV011 C2 Fixture», Power OFF."
if ask_yes_no "Начальное состояние в браузере соответствует этому?"; then
    record "browser_initial_state_user: PASS"
else
    record "browser_initial_state_user: FAIL"
    exit 1
fi

echo
echo "=== Fixture: Color picker + Power ==="
echo "Откройте «Светильники и группы»."
echo "Через Color picker выберите чистый RED (#ff0000), затем включите Power."
if ask_yes_no "Web показывает RED/Power ON, а физический светильник стабильно КРАСНЫЙ?"; then
    record "web_color_picker_red_physical_user: PASS"
else
    record "web_color_picker_red_physical_user: FAIL"
    exit 1
fi
wait_state_field requested_power true >/dev/null
wait_state_field red 255 >/dev/null
wait_state_field green 0 >/dev/null
wait_state_field blue 0 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 30 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "1" 30 >/dev/null
record "web_color_picker_red_factual_confirmation: PASS"

echo
echo "=== Fixture: RGB sliders ==="
echo "Ползунками выставьте: R=0, G=255, B=0."
if ask_yes_no "Web показывает 0/255/0, а физический светильник стабильно ЗЕЛЁНЫЙ?"; then
    record "web_rgb_sliders_green_physical_user: PASS"
else
    record "web_rgb_sliders_green_physical_user: FAIL"
    exit 1
fi
wait_state_field red 0 >/dev/null
wait_state_field green 255 >/dev/null
wait_state_field blue 0 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;255;0" 30 >/dev/null
record "web_rgb_sliders_factual_confirmation: PASS"

echo
echo "=== Fixture: Brightness ==="
echo "Установите Brightness=50."
if ask_yes_no "Светильник остался зелёным, но заметно стал примерно вдвое тусклее, а Web показывает Brightness=50?"; then
    record "web_brightness_50_physical_user: PASS"
else
    record "web_brightness_50_physical_user: FAIL"
    exit 1
fi
wait_state_field brightness 50 >/dev/null
wait_state_field green 255 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/brightness" "50" 30 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/green" "127" 30 >/dev/null
record "web_brightness_50_factual_confirmation: PASS"

echo
echo "=== Fixture: Power restore ==="
echo "Выключите Power."
if ask_yes_no "Физически свет полностью выключен, но Web сохраняет G=255 и Brightness=50?"; then
    record "web_power_off_saved_state_user: PASS"
else
    record "web_power_off_saved_state_user: FAIL"
    exit 1
fi
wait_state_field requested_power false >/dev/null
wait_state_field green 255 >/dev/null
wait_state_field brightness 50 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 30 >/dev/null
record "web_power_off_saved_state_factual_confirmation: PASS"

echo "Снова включите Power."
if ask_yes_no "Физически вернулся тот же тусклый ЗЕЛЁНЫЙ свет?"; then
    record "web_power_on_restore_physical_user: PASS"
else
    record "web_power_on_restore_physical_user: FAIL"
    exit 1
fi
wait_state_field requested_power true >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "1" 30 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/green" "127" 30 >/dev/null
record "web_power_on_restore_factual_confirmation: PASS"

echo
echo "=== Fixture: Temperature + Reset ==="
echo "Установите Temperature=100."
if ask_yes_no "Web показывает Temperature=100, физический свет стал белым/тёплым при текущей Brightness=50?"; then
    record "web_temperature_100_physical_user: PASS"
else
    record "web_temperature_100_physical_user: FAIL"
    exit 1
fi
wait_state_field temperature 100 >/dev/null
wait_state_field red 255 >/dev/null
wait_state_field green 255 >/dev/null
wait_state_field blue 255 >/dev/null
wait_state_field white 255 >/dev/null
wait_state_field brightness 50 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "127;127;127" 30 >/dev/null
record "web_temperature_100_factual_confirmation: PASS"

echo "Нажмите «Сброс»."
if ask_yes_no "После Reset Web показывает полный RGB и Brightness=100, а физический светильник включён на полном белом/тёплом?"; then
    record "web_reset_physical_user: PASS"
else
    record "web_reset_physical_user: FAIL"
    exit 1
fi
wait_state_field requested_power true >/dev/null
wait_state_field red 255 >/dev/null
wait_state_field green 255 >/dev/null
wait_state_field blue 255 >/dev/null
wait_state_field white 255 >/dev/null
wait_state_field brightness 100 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;255;255" 30 >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/brightness" "100" 30 >/dev/null
record "web_reset_factual_confirmation: PASS"

echo
echo "=== Fixture: Name ==="
echo "В поле имени введите точно:"
echo "  DEV011 C2 Fixture Renamed"
echo "После ввода нажмите Enter или кликните вне поля, чтобы сработал change."
if ask_yes_no "Имя карточки обновилось без reload страницы?"; then
    record "web_fixture_rename_user: PASS"
else
    record "web_fixture_rename_user: FAIL"
    exit 1
fi
wait_topic "/devices/dmxwb_fixture_1/controls/name" "DEV011 C2 Fixture Renamed" 30 >/dev/null
wait_config_name "DEV011 C2 Fixture Renamed" 30 >/dev/null
record "web_fixture_rename_factual_confirmation: PASS"

echo
echo "=== Web Source selector: no ArtDmx snapshot ==="
echo "Сначала через Color picker выставьте чистый BLUE (#0000ff), Brightness=100 и Power ON."
if ask_yes_no "Web показывает BLUE, а физический светильник стабильно СИНИЙ?"; then
    record "pre_source_switch_blue_physical_user: PASS"
else
    record "pre_source_switch_blue_physical_user: FAIL"
    exit 1
fi
wait_state_field red 0 >/dev/null
wait_state_field green 0 >/dev/null
wait_state_field blue 255 >/dev/null
wait_state_field brightness 100 >/dev/null
wait_state_field requested_power true >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 30 >/dev/null

wait_runtime_value status_has_artnet_snapshot 0 >/dev/null
record "no_artnet_snapshot_before_web_source_switch: PASS"

echo "Перейдите в «Управление» и нажмите ART-NET."
if ask_yes_no "Source в Web подтвердился как ART-NET, а физический свет остался СИНИМ без blackout/flicker?"; then
    record "web_mqtt_to_artnet_no_snapshot_hold_user: PASS"
else
    record "web_mqtt_to_artnet_no_snapshot_hold_user: FAIL"
    exit 1
fi
wait_topic "/devices/dmxwb/controls/source" "artnet" 30 >/dev/null
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_has_artnet_snapshot 0 >/dev/null
record "web_mqtt_to_artnet_no_snapshot_factual_confirmation: PASS"

echo
echo "Оставаясь Source=ART-NET, вернитесь в «Светильники и группы» и через Color picker выставьте RED (#ff0000)."
echo "Это должно изменить только логическое MQTT-состояние; физический источник всё ещё ART-NET без snapshot."
if ask_yes_no "Web уже показывает RED, но физический светильник всё ещё остаётся СИНИМ?"; then
    record "web_background_mqtt_red_while_artnet_physical_blue_user: PASS"
else
    record "web_background_mqtt_red_while_artnet_physical_blue_user: FAIL"
    exit 1
fi
wait_state_field red 255 >/dev/null
wait_state_field green 0 >/dev/null
wait_state_field blue 0 >/dev/null
wait_topic "/devices/dmxwb/controls/source" "artnet" 30 >/dev/null
wait_runtime_value status_selected_source artnet >/dev/null
wait_runtime_value status_has_artnet_snapshot 0 >/dev/null
record "web_background_mqtt_red_factual_confirmation: PASS"

echo
echo "Вернитесь в «Управление» и нажмите WB MQTT."
if ask_yes_no "Source подтвердился как WB MQTT, и физический светильник сразу стал текущим КРАСНЫМ?"; then
    record "web_artnet_to_mqtt_latest_red_physical_user: PASS"
else
    record "web_artnet_to_mqtt_latest_red_physical_user: FAIL"
    exit 1
fi
wait_topic "/devices/dmxwb/controls/source" "mqtt" 30 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 30 >/dev/null
record "web_artnet_to_mqtt_latest_red_factual_confirmation: PASS"

echo
echo "=== Final Web Power OFF ==="
echo "В «Светильники и группы» выключите Power."
if ask_yes_no "Физический светильник полностью выключен, а Web подтвердил Power OFF?"; then
    record "web_final_power_off_user: PASS"
else
    record "web_final_power_off_user: FAIL"
    exit 1
fi
wait_state_field requested_power false >/dev/null
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 30 >/dev/null
record "web_final_power_off_factual_confirmation: PASS"

FRAMES_BEFORE_STOP="$(wait_runtime_uint_ge status_dmx_frames_sent 1 40)"
record "continuous_dmx_frames_observed: PASS (${FRAMES_BEFORE_STOP})"

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
    echo "Unified runtime software_result != PASS." >&2
    exit 1
fi
if [[ "$(final_value final_selected_source || true)" != "mqtt" ]]; then
    echo "Финальный Source должен быть mqtt." >&2
    exit 1
fi
if [[ "$(final_value final_dmx_active_refresh_hz || true)" != "44" ]]; then
    echo "final_dmx_active_refresh_hz должен быть 44." >&2
    exit 1
fi

SWITCHES="$(final_value final_router_source_switches || true)"
NO_SNAPSHOT_SWITCHES="$(final_value final_router_source_switches_without_snapshot || true)"
ARTNET_ROUTED="$(final_value final_router_artnet_snapshots_received || true)"
MQTT_ROUTED="$(final_value final_router_mqtt_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"

for pair in \
    "SWITCHES:${SWITCHES}" \
    "NO_SNAPSHOT_SWITCHES:${NO_SNAPSHOT_SWITCHES}" \
    "ARTNET_ROUTED:${ARTNET_ROUTED}" \
    "MQTT_ROUTED:${MQTT_ROUTED}" \
    "DMX_FRAMES:${DMX_FRAMES}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( SWITCHES < 2 )); then
    echo "Ожидалось >=2 source switches, получено ${SWITCHES}." >&2
    exit 1
fi
if (( NO_SNAPSHOT_SWITCHES < 1 )); then
    echo "Не подтверждён MQTT->ARTNET switch без process-local ArtDmx snapshot." >&2
    exit 1
fi
if (( ARTNET_ROUTED != 0 )); then
    echo "В C2 не ожидались Art-Net snapshots, получено ${ARTNET_ROUTED}." >&2
    exit 1
fi
if (( MQTT_ROUTED < 8 )); then
    echo "Слишком мало MQTT snapshots для web control sequence: ${MQTT_ROUTED}." >&2
    exit 1
fi
if (( DMX_FRAMES < 1 )); then
    echo "Физический DmxOutput не передал ни одного кадра." >&2
    exit 1
fi

record "web_source_switch_count: PASS (${SWITCHES})"
record "web_source_switch_without_artnet_snapshot: PASS (${NO_SNAPSHOT_SWITCHES})"
record "router_artnet_snapshots_absent: PASS (${ARTNET_ROUTED})"
record "router_mqtt_snapshots: PASS (${MQTT_ROUTED})"
record "physical_dmx_frames: PASS (${DMX_FRAMES})"
record "fixed_44hz_physical_output: PASS"

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

echo "Восстанавливаем исходные retained topics..."
restore_retained
record "original_retained_topics_restored: PASS"

TEST_COMPLETE=1
record "dev011c2_real_web_fixture_source_result: PASS"
record "=== DMXWB DEV-011C2 REAL WEB FIXTURE + SOURCE PHYSICAL PASS ==="

echo
echo "=== DMXWB DEV-011C2 REAL WEB FIXTURE + SOURCE PHYSICAL PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
