#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev012b_systemd_acceptance.sh SSH_TARGET [fixture-start-address]

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  fixture-start-address = 1

DEV-012B проверяет production systemd lifecycle и необходимые operational diagnostics:
  - Type=simple / Restart=on-failure / RestartSec=2s;
  - start / stop / restart;
  - clean stop без нежелательного restart;
  - SIGKILL -> automatic systemd recovery;
  - restart Mosquitto -> MQTT reconnect без restart DMXWB process;
  - physical DMX продолжает работать при broker restart;
  - /dmxwb/status содержит фактические DMX/MQTT/Art-Net/config diagnostics;
  - journald содержит bounded lifecycle/recovery events;
  - standard WB UI показывает только DMXWB Status/Source.

Helper временно использует production paths и затем восстанавливает исходные файлы,
unit state и затронутые retained MQTT topics.

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
PORT="/dev/ttyRS485-1"
PORT_ADDRESS=0

if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
UNIT_FILE="${REPO_ROOT}/deploy/dmxwb.service"
STATIC_CHECK="${REPO_ROOT}/tools/web/check_dev012b_operational_contract.py"
REPORT="${REPO_ROOT}/docs/DEV012B_SYSTEMD_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev012b-systemd"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev012b-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

REMOTE_BINARY="/usr/local/bin/dmxwb"
REMOTE_UNIT="/etc/systemd/system/dmxwb.service"
REMOTE_CONFIG="/etc/dmxwb/config.json"
REMOTE_STATE="/var/lib/dmxwb/state.json"

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

for command_name in ssh scp sha256sum git python3 readelf grep awk sed tr seq; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

for path in "${BINARY}" "${UNIT_FILE}" "${STATIC_CHECK}"; do
    if [[ ! -f "${path}" ]]; then
        echo "Не найден обязательный файл: ${path}" >&2
        exit 1
    fi
done
if [[ ! -x "${BINARY}" ]]; then
    echo "Production binary не executable: ${BINARY}" >&2
    exit 1
fi

for source_file in \
    "${REPO_ROOT}/src/production_main.cpp" \
    "${REPO_ROOT}/src/integrated_runtime.cpp"; do
    if [[ "${source_file}" -nt "${BINARY}" ]]; then
        echo "Production binary старее исходника: ${source_file}" >&2
        echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
        exit 1
    fi
done

if ! readelf -h "${BINARY}" | grep -q 'Machine:.*AArch64'; then
    echo "Production artifact не AArch64." >&2
    exit 1
fi
if ! readelf -d "${BINARY}" | grep -Fq 'libmosquitto.so.1'; then
    echo "Production artifact не содержит libmosquitto.so.1." >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPENED=0
RETAINED_CAPTURED=0
REMOTE_BACKUP_READY=0
CLEANED=0
PREV_SERVICE_ACTIVE=0
PREV_SERVICE_ENABLED=0
PREV_CONFIG_DIR=0
PREV_STATE_DIR=0

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
    local attempts="${3:-60}"
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

status_value() {
    local path="$1"
    mqtt_get "/dmxwb/status" | python3 -c '
import json, sys
path = sys.argv[1].split(".")
value = json.load(sys.stdin)
for part in path:
    if not isinstance(value, dict) or part not in value:
        raise SystemExit(2)
    value = value[part]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("null")
else:
    print(value)
' "${path}"
}

wait_status_value() {
    local path="$1"
    local expected="$2"
    local attempts="${3:-80}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(status_value "${path}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался /dmxwb/status ${path}='${expected}', получено '${value}'" >&2
    return 1
}

validate_operational_status() {
    mqtt_get "/dmxwb/status" | python3 -c '
import json, sys
s = json.load(sys.stdin)
for key in ("application", "dmx", "mqtt", "artnet", "configuration", "last_error", "diagnostics"):
    if key not in s:
        raise SystemExit(f"missing top-level {key}")
d = s["diagnostics"]
for key in ("selected_source", "dmx", "mqtt", "artnet", "configuration"):
    if key not in d:
        raise SystemExit(f"missing diagnostics.{key}")
for key in ("state", "port", "frames_sent", "last_error", "recovery_state"):
    if key not in d["dmx"]:
        raise SystemExit(f"missing diagnostics.dmx.{key}")
for key in ("state", "connected", "recovery_state", "successful_connections", "disconnects", "last_error"):
    if key not in d["mqtt"]:
        raise SystemExit(f"missing diagnostics.mqtt.{key}")
for key in ("state", "universe", "active_source_ip", "packets_received", "snapshots_superseded", "recovery_state"):
    if key not in d["artnet"]:
        raise SystemExit(f"missing diagnostics.artnet.{key}")
for key in ("state", "revision", "config_path", "state_path"):
    if key not in d["configuration"]:
        raise SystemExit(f"missing diagnostics.configuration.{key}")
if d["dmx"]["port"] != "/dev/ttyRS485-1":
    raise SystemExit("unexpected DMX port")
if d["artnet"]["universe"] != 0:
    raise SystemExit("unexpected Art-Net universe")
if d["mqtt"]["connected"] is not True:
    raise SystemExit("MQTT is not connected")
if not isinstance(d["dmx"]["frames_sent"], int) or d["dmx"]["frames_sent"] < 1:
    raise SystemExit("DMX frames_sent is not factual")
print("PASS")
'
}

state_fixture_field() {
    local field="$1"
    mqtt_get "/dmxwb/state" | python3 -c '
import json, sys
field = sys.argv[1]
doc = json.load(sys.stdin)
item = next((x for x in doc.get("fixtures", []) if x.get("id") == 1), None)
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
    local attempts="${3:-60}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(state_fixture_field "${field}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидался fixture 1 ${field}='${expected}', получено '${value}'" >&2
    return 1
}

service_pid() {
    remote "systemctl show -p MainPID --value dmxwb.service" | tr -d '\r'
}

wait_service_active() {
    for _ in $(seq 1 80); do
        if remote "systemctl is-active --quiet dmxwb.service"; then
            local pid
            pid="$(service_pid)"
            if [[ "${pid}" =~ ^[1-9][0-9]*$ ]]; then
                printf '%s' "${pid}"
                return 0
            fi
        fi
        sleep 0.25
    done
    echo "dmxwb.service не стал active." >&2
    remote "systemctl status dmxwb.service --no-pager -l || true" >&2
    remote "journalctl -u dmxwb.service -n 100 --no-pager || true" >&2
    return 1
}

backup_remote_file() {
    local path="$1"
    local key="$2"
    remote "
if [[ -e '${path}' || -L '${path}' ]]; then
    cp -a '${path}' '${REMOTE_DIR}/backup/${key}'
    printf '1\n' > '${REMOTE_DIR}/backup/${key}.present'
else
    printf '0\n' > '${REMOTE_DIR}/backup/${key}.present'
fi
"
}

restore_remote_file() {
    local path="$1"
    local key="$2"
    remote "
if [[ \"\$(cat '${REMOTE_DIR}/backup/${key}.present' 2>/dev/null)\" == 1 ]]; then
    rm -f '${path}'
    cp -a '${REMOTE_DIR}/backup/${key}' '${path}'
else
    rm -f '${path}'
fi
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
    if ! remote "systemctl is-active --quiet mosquitto"; then
        remote "systemctl start mosquitto" >/dev/null 2>&1 || true
        sleep 1
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

restore_remote_environment() {
    if (( REMOTE_BACKUP_READY == 0 )); then
        return 0
    fi

    remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
    restore_retained >/dev/null 2>&1 || true

    restore_remote_file "${REMOTE_BINARY}" binary
    restore_remote_file "${REMOTE_UNIT}" unit
    restore_remote_file "${REMOTE_CONFIG}" config
    restore_remote_file "${REMOTE_STATE}" state

    if (( PREV_CONFIG_DIR == 0 )); then
        remote "rmdir /etc/dmxwb >/dev/null 2>&1 || true"
    fi
    if (( PREV_STATE_DIR == 0 )); then
        remote "rmdir /var/lib/dmxwb >/dev/null 2>&1 || true"
    fi

    remote "systemctl daemon-reload"
    if (( PREV_SERVICE_ENABLED == 1 )); then
        remote "systemctl enable dmxwb.service >/dev/null 2>&1 || true"
    else
        remote "systemctl disable dmxwb.service >/dev/null 2>&1 || true"
    fi
    if (( PREV_SERVICE_ACTIVE == 1 )); then
        remote "systemctl start dmxwb.service >/dev/null 2>&1 || true"
    fi
    REMOTE_BACKUP_READY=0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e

    if (( CLEANED == 0 )); then
        if (( SSH_OPENED == 1 )); then
            remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
            restore_remote_environment >/dev/null 2>&1 || true
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV012B Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON
cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-012B SYSTEMD + ESSENTIAL DIAGNOSTICS ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-012B systemd + diagnostics acceptance"
echo "WB8:           ${TARGET}"
echo "DMX port:      ${PORT}"
echo "Fixture Start: ${START_ADDRESS}"
echo

if ! ask_yes_no "RGBW-светильник подключён к ${PORT} и имеет Start Address ${START_ADDRESS}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    exit 2
fi

echo
echo "=== Local static contract ==="
python3 "${STATIC_CHECK}" | tee -a "${REPORT}"
record "local_static_operational_contract: PASS"

echo
echo "=== WB8 preflight ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in systemctl journalctl mosquitto_pub mosquitto_sub timeout readlink python3 grep sed awk sha256sum; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto"
remote "test -e '${PORT}'"
record "target_identity: $(remote "tr -d '\\n' </etc/wb-release 2>/dev/null || true" | tr -d '\r')"

RUNNING_DMXWB="$(remote '
for exe in /proc/[0-9]*/exe; do
    target="$(readlink "${exe}" 2>/dev/null || true)"
    case "${target##*/}" in
        dmxwb|dmxwb-dev010-source-acceptance|dmxwb-mqtt-acceptance)
            pid="${exe#/proc/}"; pid="${pid%/exe}"
            printf "%s %s\n" "${pid}" "${target}"
            ;;
    esac
done
' | tr -d '\r')"
if [[ -n "${RUNNING_DMXWB}" ]]; then
    echo "На WB8 уже запущен DMXWB executable. Остановите его перед acceptance:" >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
record "no_running_dmxwb_process: PASS"

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}/backup'"
PREV_SERVICE_ACTIVE="$(remote "systemctl is-active --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
PREV_SERVICE_ENABLED="$(remote "systemctl is-enabled --quiet dmxwb.service >/dev/null 2>&1 && echo 1 || echo 0" | tr -d '\r')"
PREV_CONFIG_DIR="$(remote "test -d /etc/dmxwb && echo 1 || echo 0" | tr -d '\r')"
PREV_STATE_DIR="$(remote "test -d /var/lib/dmxwb && echo 1 || echo 0" | tr -d '\r')"

backup_remote_file "${REMOTE_BINARY}" binary
backup_remote_file "${REMOTE_UNIT}" unit
backup_remote_file "${REMOTE_CONFIG}" config
backup_remote_file "${REMOTE_STATE}" state
REMOTE_BACKUP_READY=1

capture_retained
record "original_retained_topics_captured: PASS"

remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
remote "mkdir -p /etc/dmxwb /var/lib/dmxwb"
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_BINARY}" >/dev/null
scp "${SCP_OPTS[@]}" "${UNIT_FILE}" "${TARGET}:${REMOTE_UNIT}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/config.json" "${TARGET}:${REMOTE_CONFIG}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_STATE}" >/dev/null
remote "chmod 0755 '${REMOTE_BINARY}'; chmod 0644 '${REMOTE_UNIT}' '${REMOTE_CONFIG}' '${REMOTE_STATE}'; systemctl daemon-reload"

REMOTE_SHA="$(remote "sha256sum '${REMOTE_BINARY}' | awk '{print \$1}'" | tr -d '\r')"
LOCAL_SHA="$(sha256sum "${BINARY}" | awk '{print $1}')"
if [[ "${REMOTE_SHA}" != "${LOCAL_SHA}" ]]; then
    echo "SHA256 production binary на WB8 не совпал." >&2
    exit 1
fi
record "production_binary_installed_sha256: PASS (${REMOTE_SHA})"

TEST_START_EPOCH="$(remote "date +%s" | tr -d '\r')"

# 1. start
remote "systemctl start dmxwb.service"
PID_START="$(wait_service_active)"
wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
wait_status_value application running 80 >/dev/null
wait_status_value mqtt connected 80 >/dev/null
wait_status_value diagnostics.mqtt.connected true 80 >/dev/null
wait_status_value diagnostics.dmx.state running 80 >/dev/null
validate_operational_status >/dev/null
record "systemctl_start: PASS (pid=${PID_START})"
record "retained_operational_status: PASS"

# RED ensures this service instance owns the real physical path.
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 1"
wait_state_field requested_power true 60 >/dev/null
wait_state_field red 255 60 >/dev/null
wait_state_field green 0 60 >/dev/null
wait_state_field blue 0 60 >/dev/null
if ask_yes_no "После systemctl start физический светильник стабильно КРАСНЫЙ?"; then
    record "systemd_start_physical_red_user: PASS"
else
    record "systemd_start_physical_red_user: FAIL"
    exit 1
fi

# 2. clean stop must stay stopped and flush state.
remote "systemctl stop dmxwb.service"
sleep 3
if remote "systemctl is-active --quiet dmxwb.service"; then
    echo "Clean systemctl stop вызвал нежелательный restart." >&2
    exit 1
fi
STOPPED_PID="$(service_pid)"
if [[ "${STOPPED_PID}" != "0" ]]; then
    echo "После clean stop MainPID должен быть 0, получено ${STOPPED_PID}." >&2
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "off" 40 >/dev/null
remote "python3 - '${REMOTE_STATE}' <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    state = json.load(f)
item = next(x for x in state['fixtures'] if x['id'] == 1)
assert item['requested_power'] is True
assert item['red'] == 255 and item['green'] == 0 and item['blue'] == 0
PY"
record "systemctl_clean_stop_no_restart: PASS"
record "systemctl_clean_stop_state_flush: PASS"

# 3. start after clean stop and then explicit restart.
remote "systemctl start dmxwb.service"
PID_SECOND="$(wait_service_active)"
if [[ "${PID_SECOND}" == "${PID_START}" ]]; then
    echo "После нового start ожидался новый PID." >&2
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
wait_state_field requested_power true 60 >/dev/null
if ask_yes_no "После повторного start сохранённый КРАСНЫЙ свет восстановился?"; then
    record "systemd_second_start_state_restore_user: PASS"
else
    record "systemd_second_start_state_restore_user: FAIL"
    exit 1
fi

PID_BEFORE_RESTART="${PID_SECOND}"
remote "systemctl restart dmxwb.service"
PID_AFTER_RESTART="$(wait_service_active)"
if [[ "${PID_AFTER_RESTART}" == "${PID_BEFORE_RESTART}" ]]; then
    echo "systemctl restart не сменил MainPID." >&2
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
wait_status_value diagnostics.mqtt.connected true 80 >/dev/null
record "systemctl_restart: PASS (${PID_BEFORE_RESTART} -> ${PID_AFTER_RESTART})"

# 4. deliberate process failure must be recovered by systemd.
NRESTARTS_BEFORE_CRASH="$(remote "systemctl show -p NRestarts --value dmxwb.service" | tr -d '\r')"
PID_BEFORE_CRASH="${PID_AFTER_RESTART}"
remote "kill -KILL '${PID_BEFORE_CRASH}'"
PID_AFTER_CRASH="$(wait_service_active)"
if [[ "${PID_AFTER_CRASH}" == "${PID_BEFORE_CRASH}" ]]; then
    echo "После SIGKILL systemd не создал новый процесс." >&2
    exit 1
fi
NRESTARTS_AFTER_CRASH="$(remote "systemctl show -p NRestarts --value dmxwb.service" | tr -d '\r')"
if [[ ! "${NRESTARTS_BEFORE_CRASH}" =~ ^[0-9]+$ || ! "${NRESTARTS_AFTER_CRASH}" =~ ^[0-9]+$ ]] ||
   (( NRESTARTS_AFTER_CRASH <= NRESTARTS_BEFORE_CRASH )); then
    echo "NRestarts не увеличился после SIGKILL: ${NRESTARTS_BEFORE_CRASH} -> ${NRESTARTS_AFTER_CRASH}" >&2
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
wait_status_value diagnostics.mqtt.connected true 80 >/dev/null
record "systemd_crash_recovery: PASS (${PID_BEFORE_CRASH} -> ${PID_AFTER_CRASH})"
if ask_yes_no "После automatic recovery светильник снова стабильно КРАСНЫЙ?"; then
    record "systemd_crash_recovery_physical_user: PASS"
else
    record "systemd_crash_recovery_physical_user: FAIL"
    exit 1
fi

# 5. broker restart must recover in-process without changing DMXWB PID.
PID_BEFORE_MQTT_RESTART="${PID_AFTER_CRASH}"
echo "Сейчас будет перезапущен Mosquitto. DMXWB process и физический DMX должны продолжить работу."
remote "systemctl restart mosquitto"
for _ in $(seq 1 80); do
    if remote "systemctl is-active --quiet mosquitto"; then
        break
    fi
    sleep 0.25
done
remote "systemctl is-active --quiet mosquitto"
wait_topic "/devices/dmxwb/controls/status" "running" 120 >/dev/null
wait_status_value diagnostics.mqtt.connected true 120 >/dev/null
PID_AFTER_MQTT_RESTART="$(service_pid)"
if [[ "${PID_AFTER_MQTT_RESTART}" != "${PID_BEFORE_MQTT_RESTART}" ]]; then
    echo "DMXWB PID изменился при restart Mosquitto: ${PID_BEFORE_MQTT_RESTART} -> ${PID_AFTER_MQTT_RESTART}" >&2
    exit 1
fi
DISCONNECTS="$(status_value diagnostics.mqtt.disconnects)"
CONNECTIONS="$(status_value diagnostics.mqtt.successful_connections)"
if [[ ! "${DISCONNECTS}" =~ ^[0-9]+$ || ! "${CONNECTIONS}" =~ ^[0-9]+$ ]] ||
   (( DISCONNECTS < 1 || CONNECTIONS < 2 )); then
    echo "MQTT diagnostics не подтверждают reconnect: disconnects=${DISCONNECTS}, connections=${CONNECTIONS}" >&2
    exit 1
fi
record "mosquitto_restart_without_dmxwb_restart: PASS (pid=${PID_AFTER_MQTT_RESTART})"
record "mqtt_reconnect_diagnostics: PASS (disconnects=${DISCONNECTS}, connections=${CONNECTIONS})"
if ask_yes_no "Во время restart Mosquitto физический свет оставался стабильным КРАСНЫМ без необходимости перезапускать DMXWB?"; then
    record "mqtt_restart_physical_continuity_user: PASS"
else
    record "mqtt_restart_physical_continuity_user: FAIL"
    exit 1
fi

# 6. standard WB HomeUI visual contract.
echo
echo "=== Standard WB UI check ==="
echo "Откройте стандартный интерфейс Wiren Board (не /dmxwb/)."
echo "Ожидается: от DMXWB видны только системные Status и Source; отдельный DEV012B Fixture не должен появляться как обычный WB control device."
if ask_yes_no "Стандартный WB UI соответствует этому?"; then
    record "standard_wb_homeui_status_source_only_user: PASS"
else
    record "standard_wb_homeui_status_source_only_user: FAIL"
    exit 1
fi

# 7. journal contract.
JOURNAL="$(remote "journalctl -u dmxwb.service --since '@${TEST_START_EPOCH}' --no-pager -o cat" | tr -d '\r')"
printf '%s\n' "${JOURNAL}" > "${LOCAL_TMP}/journal.txt"
for token in \
    "dmxwb event=startup" \
    "dmxwb event=shutdown reason=signal" \
    "dmxwb event=stopped result=ok" \
    "dmxwb event=mqtt_lost" \
    "dmxwb event=mqtt_recovered"; do
    if ! grep -Fq "${token}" "${LOCAL_TMP}/journal.txt"; then
        echo "В journald отсутствует ожидаемое событие: ${token}" >&2
        cat "${LOCAL_TMP}/journal.txt" >&2
        exit 1
    fi
done
STARTUP_COUNT="$(grep -Fc 'dmxwb event=startup ' "${LOCAL_TMP}/journal.txt" || true)"
if [[ ! "${STARTUP_COUNT}" =~ ^[0-9]+$ ]] || (( STARTUP_COUNT < 4 )); then
    echo "Ожидалось минимум 4 startup events, получено ${STARTUP_COUNT}." >&2
    exit 1
fi
record "journald_lifecycle_events: PASS"
record "journald_mqtt_recovery_events: PASS"
record "journald_startup_count: ${STARTUP_COUNT}"

# Final safe OFF and clean stop.
remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0"
wait_state_field requested_power false 60 >/dev/null
if ask_yes_no "Перед завершением acceptance физический светильник полностью выключен?"; then
    record "final_power_off_user: PASS"
else
    record "final_power_off_user: FAIL"
    exit 1
fi
remote "systemctl stop dmxwb.service"
sleep 1
if remote "systemctl is-active --quiet dmxwb.service"; then
    echo "Финальный clean stop не завершил service." >&2
    exit 1
fi
record "final_clean_service_stop: PASS"

restore_remote_environment
record "original_environment_restored: PASS"

record "dev012b_systemd_diagnostics_result: PASS"
record "=== DMXWB DEV-012B SYSTEMD + DIAGNOSTICS PASS ==="

echo
echo "=== DMXWB DEV-012B SYSTEMD + DIAGNOSTICS PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
