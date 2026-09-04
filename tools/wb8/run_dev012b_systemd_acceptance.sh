#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev012b_systemd_acceptance.sh SSH_TARGET [fixture-start-address]
  bash tools/wb8/run_dev012b3_counter_isolation_regression.sh SSH_TARGET [fixture-start-address]
  bash tools/wb8/run_dev012b4_focused_regression.sh SSH_TARGET [fixture-start-address]
  bash tools/wb8/run_dev012b5_focused_regression.sh SSH_TARGET [fixture-start-address]

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
  - standard WB UI показывает DMXWB Status/Source и созданные устройства.

DEV-012B3 wrapper дополнительно проверяет MQTT -> DMX, Art-Net -> DMX,
явное переключение Source, factual status без cumulative telemetry и освобождение
RS-485 после clean stop.

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
ACCEPTANCE_VARIANT="${DMXWB_SYSTEMD_ACCEPTANCE_VARIANT:-dev012b}"
ARTNET_HOST="${TARGET#*@}"
PORT="/dev/ttyRS485-1"
ALT_PORT="/dev/ttyRS485-2"
PORT_ADDRESS=0

case "${ACCEPTANCE_VARIANT}" in
    dev012b)
        ACCEPTANCE_LABEL="DEV-012B"
        ACCEPTANCE_RESULT="DEV-012B SYSTEMD + DIAGNOSTICS"
        REPORT_NAME="DEV012B_SYSTEMD_REPORT.txt"
        REMOTE_SUFFIX="dev012b-systemd"
        RUN_COUNTER_ISOLATION_REGRESSION=0
        RUN_B4_FOCUSED_REGRESSION=0
        RUN_B5_FOCUSED_REGRESSION=0
        STATIC_CHECK_REL="tools/web/check_dev012b_operational_contract.py"
        ;;
    dev012b3)
        ACCEPTANCE_LABEL="DEV-012B3"
        ACCEPTANCE_RESULT="DEV-012B3 COUNTER ISOLATION REGRESSION"
        REPORT_NAME="DEV012B3_COUNTER_ISOLATION_REGRESSION_REPORT.txt"
        REMOTE_SUFFIX="dev012b3-regression"
        RUN_COUNTER_ISOLATION_REGRESSION=1
        RUN_B4_FOCUSED_REGRESSION=0
        RUN_B5_FOCUSED_REGRESSION=0
        STATIC_CHECK_REL="tools/web/check_dev012b_operational_contract.py"
        ;;
    dev012b4)
        ACCEPTANCE_LABEL="DEV-012B4"
        ACCEPTANCE_RESULT="DEV-012B4 FOCUSED CORRECTIVE REGRESSION"
        REPORT_NAME="DEV012B4_FOCUSED_CORRECTIVE_REGRESSION_REPORT.txt"
        REMOTE_SUFFIX="dev012b4-focused"
        RUN_COUNTER_ISOLATION_REGRESSION=0
        RUN_B4_FOCUSED_REGRESSION=1
        RUN_B5_FOCUSED_REGRESSION=0
        STATIC_CHECK_REL="tools/web/check_dev011f1_config_settings.py"
        ;;
    dev012b5)
        ACCEPTANCE_LABEL="DEV-012B5"
        ACCEPTANCE_RESULT="DEV-012B5 FOCUSED CORRECTIVE REGRESSION"
        REPORT_NAME="DEV012B5_FOCUSED_CORRECTIVE_REGRESSION_REPORT.txt"
        REMOTE_SUFFIX="dev012b5-focused"
        RUN_COUNTER_ISOLATION_REGRESSION=0
        RUN_B4_FOCUSED_REGRESSION=0
        RUN_B5_FOCUSED_REGRESSION=1
        STATIC_CHECK_REL="tools/web/check_dev012b5_numeric_validation.py"
        ;;
    *)
        echo "Неизвестный acceptance variant: ${ACCEPTANCE_VARIANT}" >&2
        exit 2
        ;;
esac

if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "fixture-start-address должен быть целым числом 1..297." >&2
    exit 2
fi
if (( RUN_COUNTER_ISOLATION_REGRESSION == 1 ||
      RUN_B4_FOCUSED_REGRESSION == 1 ||
      RUN_B5_FOCUSED_REGRESSION == 1 )) &&
   [[ ! "${ARTNET_HOST}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "Для Art-Net regression SSH_TARGET должен содержать IPv4, например root@10.200.200.1." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
UNIT_FILE="${REPO_ROOT}/deploy/dmxwb.service"
STATIC_CHECK="${REPO_ROOT}/${STATIC_CHECK_REL}"
REPORT="${REPO_ROOT}/docs/${REPORT_NAME}"
REMOTE_DIR="/tmp/dmxwb-${REMOTE_SUFFIX}"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-${REMOTE_SUFFIX}-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

REMOTE_BINARY="/usr/local/bin/dmxwb"
REMOTE_UNIT="/etc/systemd/system/dmxwb.service"
REMOTE_CONFIG="/etc/dmxwb/config.json"
REMOTE_STATE="/var/lib/dmxwb/state.json"
REMOTE_WEB="/var/www/dmxwb"

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

for command_name in ssh scp sha256sum python3 readelf grep awk sed tr seq; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    }
done

for path in \
    "${BINARY}" \
    "${UNIT_FILE}" \
    "${STATIC_CHECK}" \
    "${REPO_ROOT}/www/dmxwb/index.html" \
    "${REPO_ROOT}/www/dmxwb/app.js"; do
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
ARTNET_SENDER_PID=0
REMOTE_ALT_PORT_MOVED=0
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
for key in ("state", "port", "slot_count", "refresh_hz", "physical_slot_limit", "active_generation", "last_error", "recovery_state"):
    if key not in d["dmx"]:
        raise SystemExit(f"missing diagnostics.dmx.{key}")
for key in ("state", "connected", "recovery_state", "last_error"):
    if key not in d["mqtt"]:
        raise SystemExit(f"missing diagnostics.mqtt.{key}")
for key in (
    "state", "universe", "active_source_ip", "active_source_physical",
    "last_packet_age_ms", "last_sequence", "sync_mode", "last_sync_age_ms",
    "conflicting_source_ip", "conflicting_source_physical", "output_mode",
    "transport_open", "committed_revision", "last_error", "recovery_state",
):
    if key not in d["artnet"]:
        raise SystemExit(f"missing diagnostics.artnet.{key}")
for key in ("state", "revision", "dmx_port", "artnet_universe", "config_path", "state_path"):
    if key not in d["configuration"]:
        raise SystemExit(f"missing diagnostics.configuration.{key}")

forbidden = {
    "frames_sent", "deadlines_missed", "packets_received", "datagrams_received",
    "commands_processed", "commands_rejected", "publications", "republishes",
    "snapshots_published", "snapshots_routed", "snapshots_superseded",
    "source_switches", "successful_connections", "disconnects", "open_failures",
    "send_failures", "publish_failures", "recoveries",
}
def reject_cumulative_fields(value, path="status"):
    if isinstance(value, dict):
        for key, child in value.items():
            if key in forbidden:
                raise SystemExit(f"forbidden cumulative field {path}.{key}")
            reject_cumulative_fields(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_cumulative_fields(child, f"{path}[{index}]")
reject_cumulative_fields(s)

if d["dmx"]["port"] != "/dev/ttyRS485-1":
    raise SystemExit("unexpected DMX port")
if d["dmx"]["state"] != "running":
    raise SystemExit("DMX is not running")
slot_count = d["dmx"]["slot_count"]
if type(slot_count) is not int or not 1 <= slot_count <= 300:
    raise SystemExit(f"unexpected active DMX slot_count: {slot_count!r}")
if d["dmx"]["refresh_hz"] != 44 or d["dmx"]["physical_slot_limit"] != 300:
    raise SystemExit("unexpected physical DMX refresh/limit contract")
if d["artnet"]["universe"] != 0:
    raise SystemExit("unexpected Art-Net universe")
if d["artnet"]["transport_open"] is not True:
    raise SystemExit("Art-Net transport is not open")
if d["mqtt"]["connected"] is not True:
    raise SystemExit("MQTT is not connected")
print("PASS")
'
}

send_artdmx() {
    local red="$1"
    local green="$2"
    local blue="$3"
    local white="$4"
    python3 - "${ARTNET_HOST}" "${PORT_ADDRESS}" "${START_ADDRESS}" \
        "${red}" "${green}" "${blue}" "${white}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
start_index = int(sys.argv[3]) - 1
values = bytes(int(x) for x in sys.argv[4:8])
dmx = bytearray(300)
dmx[start_index:start_index + 4] = values
sub_uni = port_address & 0xff
net = (port_address >> 8) & 0x7f
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    for sequence in range(1, 9):
        packet = (
            b"Art-Net\x00"
            + struct.pack("<H", 0x5000)
            + bytes((0, 14, sequence, 0, sub_uni, net))
            + struct.pack(">H", len(dmx))
            + dmx
        )
        sock.sendto(packet, (host, 6454))
        time.sleep(0.05)
finally:
    sock.close()
PY
}

send_artdmx_stream() {
    local red="$1"
    local green="$2"
    local blue="$3"
    local white="$4"
    python3 - "${ARTNET_HOST}" "${PORT_ADDRESS}" "${START_ADDRESS}" \
        "${red}" "${green}" "${blue}" "${white}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
start_index = int(sys.argv[3]) - 1
values = bytes(int(x) for x in sys.argv[4:8])
dmx = bytearray(300)
dmx[start_index:start_index + 4] = values
sub_uni = port_address & 0xff
net = (port_address >> 8) & 0x7f
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    deadline = time.monotonic() + 20.0
    sequence = 1
    while time.monotonic() < deadline:
        packet = (
            b"Art-Net\x00"
            + struct.pack("<H", 0x5000)
            + bytes((0, 14, sequence, 0, sub_uni, net))
            + struct.pack(">H", len(dmx))
            + dmx
        )
        sock.sendto(packet, (host, 6454))
        sequence = 1 if sequence == 255 else sequence + 1
        time.sleep(0.05)
finally:
    sock.close()
PY
}

send_artdmx_fixed_sequence() {
    local sequence="$1"
    local red="$2"
    local green="$3"
    local blue="$4"
    local white="$5"
    local duration="$6"
    python3 - "${ARTNET_HOST}" "${PORT_ADDRESS}" "${START_ADDRESS}" \
        "${sequence}" "${red}" "${green}" "${blue}" "${white}" "${duration}" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port_address = int(sys.argv[2])
start_index = int(sys.argv[3]) - 1
sequence = int(sys.argv[4])
values = bytes(int(x) for x in sys.argv[5:9])
duration = float(sys.argv[9])
dmx = bytearray(300)
dmx[start_index:start_index + 4] = values
sub_uni = port_address & 0xff
net = (port_address >> 8) & 0x7f
packet = (
    b"Art-Net\x00"
    + struct.pack("<H", 0x5000)
    + bytes((0, 14, sequence, 0, sub_uni, net))
    + struct.pack(">H", len(dmx))
    + dmx
)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        sock.sendto(packet, (host, 6454))
        time.sleep(0.2)
finally:
    sock.close()
PY
}

publish_scene_create_and_get_result() {
    local request_id="$1"
    local name="$2"
    remote "python3 - '${request_id}' '${name}' <<'PY'
import json
import subprocess
import sys
import time

request_id, name = sys.argv[1:3]
subscriber = subprocess.Popen([
    'mosquitto_sub', '-h', '127.0.0.1', '-p', '1883',
    '-t', '/dmxwb/config/result', '-C', '1',
], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    time.sleep(0.2)
    payload = json.dumps(
        {'request_id': request_id, 'name': name},
        ensure_ascii=False,
        separators=(',', ':'),
    )
    subprocess.run([
        'mosquitto_pub', '-h', '127.0.0.1', '-p', '1883',
        '-t', '/dmxwb/scenes/create', '-m', payload,
    ], check=True)
    stdout, stderr = subscriber.communicate(timeout=10)
except BaseException:
    subscriber.kill()
    subscriber.wait()
    raise
if subscriber.returncode != 0:
    raise SystemExit(stderr)
result = json.loads(stdout)
if result.get('request_id') != request_id:
    raise SystemExit('uncorrelated Scene Create result')
print(json.dumps(result, separators=(',', ':'), sort_keys=True))
PY"
}

publish_config_port() {
    local port="$1"
    local request_id="$2"
    remote "python3 - '${port}' '${request_id}' <<'PY'
import json
import subprocess
import sys

port, request_id = sys.argv[1:3]
raw = subprocess.check_output([
    'timeout', '5', 'mosquitto_sub', '-h', '127.0.0.1', '-p', '1883',
    '-t', '/dmxwb/config', '-C', '1',
], text=True)
config = json.loads(raw)
payload = {
    'request_id': request_id,
    'expected_revision': config['revision'],
    'config': config,
}
payload['config']['dmx']['port'] = port
subprocess.run([
    'mosquitto_pub', '-h', '127.0.0.1', '-p', '1883',
    '-t', '/dmxwb/config/set', '-m', json.dumps(payload, separators=(',', ':')),
], check=True)
PY"
}

wait_config_port() {
    local expected="$1"
    for _ in $(seq 1 80); do
        local actual
        actual="$(mqtt_get '/dmxwb/config' | python3 -c 'import json,sys; print(json.load(sys.stdin)["dmx"]["port"])' 2>/dev/null || true)"
        if [[ "${actual}" == "${expected}" ]]; then
            printf '%s\n' "${actual}"
            return 0
        fi
        sleep 0.25
    done
    echo "Config DMX Port не стал ${expected}." >&2
    return 1
}

serial_port_is_released() {
    local checked_port="${1:-${PORT}}"
    remote "
for fd in /proc/[0-9]*/fd/*; do
    target=\"\$(readlink \"\${fd}\" 2>/dev/null || true)\"
    if [[ \"\${target}\" == '${checked_port}' ]]; then
        printf '%s -> %s\n' \"\${fd}\" \"\${target}\" >&2
        exit 1
    fi
done
exit 0
"
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

wait_service_pid_change() {
    local previous_pid="$1"
    for _ in $(seq 1 80); do
        if remote "systemctl is-active --quiet dmxwb.service"; then
            local pid
            pid="$(service_pid)"
            if [[ "${pid}" =~ ^[1-9][0-9]*$ && "${pid}" != "${previous_pid}" ]]; then
                printf '%s' "${pid}"
                return 0
            fi
        fi
        sleep 0.25
    done
    echo "dmxwb.service не создал новый MainPID после ${previous_pid}." >&2
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
    rm -rf '${path}'
    cp -a '${REMOTE_DIR}/backup/${key}' '${path}'
else
    rm -rf '${path}'
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

if (( RUN_B5_FOCUSED_REGRESSION == 1 )); then
    for prefix in \
        "/devices/dmxwb_fixture_901" \
        "/devices/dmxwb_group_901"; do
        RETAINED_TOPICS+=("${prefix}/meta")
        for control in name power red green blue color brightness temperature reset; do
            RETAINED_TOPICS+=("${prefix}/controls/${control}/meta")
            if [[ "${control}" != "reset" ]]; then
                RETAINED_TOPICS+=("${prefix}/controls/${control}")
            fi
        done
    done
    for scene_id in 901 902; do
        RETAINED_TOPICS+=(
            "/devices/dmxwb_scene_${scene_id}/meta"
            "/devices/dmxwb_scene_${scene_id}/controls/name/meta"
            "/devices/dmxwb_scene_${scene_id}/controls/name"
            "/devices/dmxwb_scene_${scene_id}/controls/apply/meta"
        )
    done
fi

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
    restore_remote_file "${REMOTE_WEB}" web

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
        if (( ARTNET_SENDER_PID > 0 )); then
            kill "${ARTNET_SENDER_PID}" >/dev/null 2>&1 || true
            wait "${ARTNET_SENDER_PID}" >/dev/null 2>&1 || true
            ARTNET_SENDER_PID=0
        fi
        if (( SSH_OPENED == 1 )); then
            if (( REMOTE_ALT_PORT_MOVED == 1 )); then
                remote "if test -e '${ALT_PORT}.dmxwb-test' && ! test -e '${ALT_PORT}'; then mv '${ALT_PORT}.dmxwb-test' '${ALT_PORT}'; fi" >/dev/null 2>&1 || true
                REMOTE_ALT_PORT_MOVED=0
            fi
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

if (( RUN_B5_FOCUSED_REGRESSION == 1 )); then
    cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"${ACCEPTANCE_LABEL} Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":902,"next_group_id":902,"next_scene_id":902}}
JSON
    cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}],"mqtt_retained_cleanup":{"fixture_ids":[901],"group_ids":[901],"scene_ids":[901]},"scene_create_idempotency":[]}
JSON
else
    cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":1,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"${ACCEPTANCE_LABEL} Fixture"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":2,"next_group_id":1,"next_scene_id":1}}
JSON
    cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON
fi

: > "${REPORT}"
record "=== DMXWB ${ACCEPTANCE_RESULT} ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if (( RUN_B4_FOCUSED_REGRESSION == 0 && RUN_B5_FOCUSED_REGRESSION == 0 )); then
    record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
    record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
fi
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "dmx_port: ${PORT}"
record "fixture_start_address: ${START_ADDRESS}"
record "artnet_port_address: ${PORT_ADDRESS}"
record "fixed_refresh_hz: 44"

echo "DMXWB — ${ACCEPTANCE_RESULT} acceptance"
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
if (( RUN_B4_FOCUSED_REGRESSION == 1 )) &&
   ! ask_yes_no "${ALT_PORT} сейчас свободен и может быть временно использован/скрыт для serial recovery test?"; then
    exit 2
fi

echo
echo "=== Local static contract ==="
if (( RUN_B4_FOCUSED_REGRESSION == 1 || RUN_B5_FOCUSED_REGRESSION == 1 )); then
    record "local_static_contract: covered_by_prior_host_regression"
else
    python3 "${STATIC_CHECK}" | tee -a "${REPORT}"
    record "local_static_operational_contract: PASS"
fi

echo
echo "=== WB8 preflight ==="
echo "Открываем одно SSH-соединение. Пароль может потребоваться один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in systemctl journalctl mosquitto_pub mosquitto_sub timeout readlink python3 grep sed awk sha256sum; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet mosquitto"
remote "test -e '${PORT}'"
if (( RUN_B4_FOCUSED_REGRESSION == 1 )); then
    remote "test -e '${ALT_PORT}'"
    remote "test ! -e '${ALT_PORT}.dmxwb-test'"
    if ! serial_port_is_released "${ALT_PORT}"; then
        echo "Альтернативный порт ${ALT_PORT} уже используется другим процессом." >&2
        exit 1
    fi
fi
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
backup_remote_file "${REMOTE_WEB}" web
REMOTE_BACKUP_READY=1

capture_retained
record "original_retained_topics_captured: PASS"

remote "systemctl stop dmxwb.service >/dev/null 2>&1 || true"
remote "mkdir -p /etc/dmxwb /var/lib/dmxwb"
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_BINARY}" >/dev/null
scp "${SCP_OPTS[@]}" "${UNIT_FILE}" "${TARGET}:${REMOTE_UNIT}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/config.json" "${TARGET}:${REMOTE_CONFIG}" >/dev/null
scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_STATE}" >/dev/null
scp -r "${SCP_OPTS[@]}" "${REPO_ROOT}/www/dmxwb" "${TARGET}:${REMOTE_DIR}/web" >/dev/null
remote "rm -rf '${REMOTE_WEB}'; cp -a '${REMOTE_DIR}/web' '${REMOTE_WEB}'; chmod 0755 '${REMOTE_BINARY}'; chmod 0644 '${REMOTE_UNIT}' '${REMOTE_CONFIG}' '${REMOTE_STATE}'; systemctl daemon-reload"

REMOTE_SHA="$(remote "sha256sum '${REMOTE_BINARY}' | awk '{print \$1}'" | tr -d '\r')"
LOCAL_SHA="$(sha256sum "${BINARY}" | awk '{print $1}')"
if [[ "${REMOTE_SHA}" != "${LOCAL_SHA}" ]]; then
    echo "SHA256 production binary на WB8 не совпал." >&2
    exit 1
fi
record "production_binary_installed_sha256: PASS (${REMOTE_SHA})"
remote "test \"\$(sha256sum '${REMOTE_WEB}/app.js' | awk '{print \$1}')\" = '$(sha256sum "${REPO_ROOT}/www/dmxwb/app.js" | awk '{print $1}')'"
record "current_static_web_installed_sha256: PASS"

TEST_START_EPOCH="$(remote "date +%s" | tr -d '\r')"

# 1. start
if (( RUN_B5_FOCUSED_REGRESSION == 1 )); then
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -r -t '/devices/dmxwb_fixture_901/meta' -m stale-fixture; mosquitto_pub -h 127.0.0.1 -p 1883 -r -t '/devices/dmxwb_group_901/meta' -m stale-group; mosquitto_pub -h 127.0.0.1 -p 1883 -r -t '/devices/dmxwb_scene_901/meta' -m stale-scene"
    remote "systemctl stop mosquitto"
    remote "systemctl start dmxwb.service"
    PID_START="$(wait_service_active)"
    sleep 1
    record "systemctl_start_while_mqtt_offline: PASS (pid=${PID_START})"
else
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
fi

if (( RUN_B4_FOCUSED_REGRESSION == 1 )); then
    echo
    echo "=== DEV-012B4 focused physical/runtime regression ==="

    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
    wait_topic "/devices/dmxwb/controls/source" "artnet" 40 >/dev/null
    wait_status_value diagnostics.selected_source artnet 80 >/dev/null

    send_artdmx_stream 0 0 255 0 &
    ARTNET_SENDER_PID=$!
    sleep 1
    wait_status_value diagnostics.artnet.state ACTIVE 40 >/dev/null
    wait_status_value diagnostics.artnet.output_mode live 40 >/dev/null

    publish_config_port "${ALT_PORT}" "dev012b4-port-2"
    wait_config_port "${ALT_PORT}" >/dev/null
    wait_status_value diagnostics.dmx.port "${ALT_PORT}" 80 >/dev/null
    wait_status_value diagnostics.dmx.state running 80 >/dev/null

    publish_config_port "${PORT}" "dev012b4-port-1"
    wait_config_port "${PORT}" >/dev/null
    wait_status_value diagnostics.dmx.port "${PORT}" 80 >/dev/null
    wait_status_value diagnostics.dmx.state running 80 >/dev/null

    wait "${ARTNET_SENDER_PID}"
    ARTNET_SENDER_PID=0
    if [[ "$(service_pid)" != "${PID_START}" ]]; then
        echo "DMXWB PID изменился во время Art-Net traffic + DMX Port reconfiguration." >&2
        exit 1
    fi
    if ask_yes_no "После возврата на ${PORT} последний цельный Art-Net кадр восстановил стабильный СИНИЙ свет?"; then
        record "artnet_dmx_port_reconfiguration_latest_snapshot_user: PASS"
    else
        record "artnet_dmx_port_reconfiguration_latest_snapshot_user: FAIL"
        exit 1
    fi
    record "artnet_dmx_port_reconfiguration_same_process: PASS (pid=${PID_START})"

    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 1"
    wait_topic "/devices/dmxwb/controls/source" "mqtt" 40 >/dev/null
    wait_status_value diagnostics.selected_source mqtt 80 >/dev/null
    wait_state_field red 255 60 >/dev/null
    wait_state_field green 0 60 >/dev/null
    wait_state_field blue 0 60 >/dev/null
    if ask_yes_no "После explicit Source=WB MQTT светильник снова стабильно КРАСНЫЙ?"; then
        record "focused_mqtt_artnet_source_switch_user: PASS"
    else
        record "focused_mqtt_artnet_source_switch_user: FAIL"
        exit 1
    fi

    CONFIG_REVISION_BEFORE_RESTART="$(mqtt_get '/dmxwb/config' | python3 -c 'import json,sys; print(json.load(sys.stdin)["revision"])')"
    PID_BEFORE_PERSISTENCE_RESTART="$(service_pid)"
    remote "systemctl restart dmxwb.service"
    PID_AFTER_PERSISTENCE_RESTART="$(wait_service_active)"
    if [[ "${PID_AFTER_PERSISTENCE_RESTART}" == "${PID_BEFORE_PERSISTENCE_RESTART}" ]]; then
        echo "Persistence restart не сменил MainPID." >&2
        exit 1
    fi
    wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
    wait_status_value diagnostics.dmx.state running 80 >/dev/null
    wait_status_value diagnostics.dmx.port "${PORT}" 80 >/dev/null
    wait_state_field requested_power true 60 >/dev/null
    wait_state_field red 255 60 >/dev/null
    CONFIG_REVISION_AFTER_RESTART="$(mqtt_get '/dmxwb/config' | python3 -c 'import json,sys; print(json.load(sys.stdin)["revision"])')"
    if [[ "${CONFIG_REVISION_AFTER_RESTART}" != "${CONFIG_REVISION_BEFORE_RESTART}" ]]; then
        echo "Config revision изменилась при restart: ${CONFIG_REVISION_BEFORE_RESTART} -> ${CONFIG_REVISION_AFTER_RESTART}" >&2
        exit 1
    fi
    remote "python3 - '${REMOTE_CONFIG}' '${REMOTE_STATE}' '${PORT}' <<'PY'
import json
import sys

config_path, state_path, expected_port = sys.argv[1:4]
with open(config_path, encoding='utf-8') as stream:
    config = json.load(stream)
with open(state_path, encoding='utf-8') as stream:
    state = json.load(stream)
fixture = next(item for item in state['fixtures'] if item['id'] == 1)
assert config['dmx']['port'] == expected_port
assert fixture['requested_power'] is True
assert fixture['red'] == 255 and fixture['green'] == 0 and fixture['blue'] == 0
PY"
    if ask_yes_no "После service restart согласованная конфигурация и сохранённый КРАСНЫЙ свет восстановились?"; then
        record "coherent_config_state_restart_user: PASS"
    else
        record "coherent_config_state_restart_user: FAIL"
        exit 1
    fi
    record "coherent_config_state_revision_restart: PASS (revision=${CONFIG_REVISION_AFTER_RESTART})"

    PID_SERIAL_RECOVERY="$(service_pid)"
    REMOTE_ALT_PORT_MOVED=1
    remote "mv '${ALT_PORT}' '${ALT_PORT}.dmxwb-test'"
    publish_config_port "${ALT_PORT}" "dev012b4-serial-failure"
    wait_config_port "${ALT_PORT}" >/dev/null
    wait_status_value diagnostics.dmx.port "${ALT_PORT}" 80 >/dev/null
    wait_status_value application error 80 >/dev/null
    wait_status_value diagnostics.dmx.state reconnecting 80 >/dev/null
    wait_status_value diagnostics.dmx.recovery_state reconnecting 80 >/dev/null
    record "serial_missing_device_factual_reconnecting_status: PASS"

    remote "mv '${ALT_PORT}.dmxwb-test' '${ALT_PORT}'"
    REMOTE_ALT_PORT_MOVED=0
    wait_status_value application running 120 >/dev/null
    wait_status_value diagnostics.dmx.state running 120 >/dev/null
    wait_status_value diagnostics.dmx.recovery_state ok 120 >/dev/null
    if [[ "$(service_pid)" != "${PID_SERIAL_RECOVERY}" ]]; then
        echo "Serial recovery потребовал restart процесса." >&2
        exit 1
    fi
    publish_config_port "${PORT}" "dev012b4-serial-return"
    wait_config_port "${PORT}" >/dev/null
    wait_status_value diagnostics.dmx.port "${PORT}" 80 >/dev/null
    wait_status_value diagnostics.dmx.state running 80 >/dev/null
    if ask_yes_no "После освобождения serial port тот же процесс восстановил стабильный КРАСНЫЙ физический DMX?"; then
        record "serial_recovery_physical_user: PASS"
    else
        record "serial_recovery_physical_user: FAIL"
        exit 1
    fi
    record "serial_recovery_same_process_factual_status: PASS (pid=${PID_SERIAL_RECOVERY})"
    validate_operational_status >/dev/null
elif (( RUN_B5_FOCUSED_REGRESSION == 1 )); then
    echo
    echo "=== DEV-012B5 focused recovery/durability regression ==="

    # Pending retained tombstones must survive both broker loss and process restart.
    remote "python3 - '${REMOTE_STATE}' <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
cleanup = state['mqtt_retained_cleanup']
assert cleanup == {'fixture_ids': [901], 'group_ids': [901], 'scene_ids': [901]}
PY"
    PID_CLEANUP_BEFORE_CRASH="$(service_pid)"
    remote "kill -KILL '${PID_CLEANUP_BEFORE_CRASH}'"
    PID_CLEANUP_AFTER_CRASH="$(wait_service_active)"
    if [[ "${PID_CLEANUP_AFTER_CRASH}" == "${PID_CLEANUP_BEFORE_CRASH}" ]]; then
        echo "DMXWB не перезапустился во время offline retained-cleanup test." >&2
        exit 1
    fi
    remote "python3 - '${REMOTE_STATE}' <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
cleanup = state['mqtt_retained_cleanup']
assert cleanup == {'fixture_ids': [901], 'group_ids': [901], 'scene_ids': [901]}
PY"
    remote "systemctl start mosquitto"
    for _ in $(seq 1 80); do
        remote "systemctl is-active --quiet mosquitto" && break
        sleep 0.25
    done
    remote "systemctl is-active --quiet mosquitto"
    wait_topic "/devices/dmxwb/controls/status" "running" 120 >/dev/null
    wait_status_value diagnostics.mqtt.connected true 120 >/dev/null
    for topic in \
        "/devices/dmxwb_fixture_901/meta" \
        "/devices/dmxwb_group_901/meta" \
        "/devices/dmxwb_scene_901/meta"; do
        if mqtt_get "${topic}" >/dev/null 2>&1; then
            echo "Retained tombstone не удалил stale topic: ${topic}" >&2
            exit 1
        fi
    done
    CLEANUP_CLEARED=0
    for _ in $(seq 1 80); do
        if remote "python3 - '${REMOTE_STATE}' <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    cleanup = json.load(stream)['mqtt_retained_cleanup']
raise SystemExit(0 if cleanup == {'fixture_ids': [], 'group_ids': [], 'scene_ids': []} else 1)
PY"; then
            CLEANUP_CLEARED=1
            break
        fi
        sleep 0.25
    done
    if (( CLEANUP_CLEARED == 0 )); then
        echo "Durable retained cleanup не был подтверждён в state.json." >&2
        exit 1
    fi
    record "retained_cleanup_disconnect_restart_delivery: PASS (${PID_CLEANUP_BEFORE_CRASH} -> ${PID_CLEANUP_AFTER_CRASH})"

    # Establish a visible MQTT baseline after the offline recovery.
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 1"
    wait_state_field requested_power true 60 >/dev/null
    wait_state_field red 255 60 >/dev/null
    wait_state_field green 0 60 >/dev/null
    wait_state_field blue 0 60 >/dev/null
    if ask_yes_no "После MQTT recovery физический светильник стабильно КРАСНЫЙ?"; then
        record "mqtt_recovery_physical_red_user: PASS"
    else
        record "mqtt_recovery_physical_red_user: FAIL"
        exit 1
    fi

    # The exact same Scene Create request must replay across a process restart.
    SCENE_REQUEST_ID="dev012b5-scene-create-retry"
    SCENE_NAME="DEV012B5 Durable Scene"
    FIRST_SCENE_RESULT="$(publish_scene_create_and_get_result "${SCENE_REQUEST_ID}" "${SCENE_NAME}" | tr -d '\r')"
    FIRST_SCENE_ID="$(printf '%s' "${FIRST_SCENE_RESULT}" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["ok"] is True; print(r["entity_id"])')"
    FIRST_SCENE_REVISION="$(printf '%s' "${FIRST_SCENE_RESULT}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["revision"])')"
    if [[ "${FIRST_SCENE_ID}" != "902" ]]; then
        echo "Scene Create выделил неожиданный stable ID: ${FIRST_SCENE_ID}." >&2
        exit 1
    fi
    PID_SCENE_BEFORE_RESTART="$(service_pid)"
    remote "systemctl restart dmxwb.service"
    PID_SCENE_AFTER_RESTART="$(wait_service_active)"
    wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
    SECOND_SCENE_RESULT="$(publish_scene_create_and_get_result "${SCENE_REQUEST_ID}" "${SCENE_NAME}" | tr -d '\r')"
    if [[ "${SECOND_SCENE_RESULT}" != "${FIRST_SCENE_RESULT}" ]]; then
        echo "Scene Create replay вернул другой correlated result." >&2
        printf 'first:  %s\nsecond: %s\n' "${FIRST_SCENE_RESULT}" "${SECOND_SCENE_RESULT}" >&2
        exit 1
    fi
    mqtt_get "/dmxwb/config" | python3 -c '
import json, sys
config = json.load(sys.stdin)
scenes = [scene for scene in config["scenes"] if scene["id"] == 902]
assert config["revision"] == int(sys.argv[1])
assert len(scenes) == 1 and scenes[0]["name"] == sys.argv[2]
' "${FIRST_SCENE_REVISION}" "${SCENE_NAME}"
    remote "python3 - '${REMOTE_STATE}' '${SCENE_REQUEST_ID}' '${FIRST_SCENE_REVISION}' <<'PY'
import json, sys
state_path, request_id, revision = sys.argv[1:4]
with open(state_path, encoding='utf-8') as stream:
    state = json.load(stream)
records = [item for item in state['scene_create_idempotency'] if item['request_id'] == request_id]
assert len(records) == 1
assert records[0]['scene_id'] == 902 and records[0]['revision'] == int(revision)
PY"
    record "scene_create_idempotency_process_restart: PASS (${PID_SCENE_BEFORE_RESTART} -> ${PID_SCENE_AFTER_RESTART}, scene=902, revision=${FIRST_SCENE_REVISION})"

    # A restarted controller repeating Sequence=1 must replace the old frame after LOST.
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
    wait_topic "/devices/dmxwb/controls/source" "artnet" 40 >/dev/null
    wait_status_value diagnostics.selected_source artnet 80 >/dev/null
    send_artdmx_fixed_sequence 128 0 0 255 0 0.8
    wait_status_value diagnostics.artnet.state ACTIVE 40 >/dev/null
    wait_status_value diagnostics.artnet.last_sequence 128 40 >/dev/null
    if ask_yes_no "Перед имитацией restart Art-Net controller светильник стабильно СИНИЙ?"; then
        record "artnet_sequence_128_blue_user: PASS"
    else
        record "artnet_sequence_128_blue_user: FAIL"
        exit 1
    fi
    sleep 3.2
    wait_status_value diagnostics.artnet.state LOST 40 >/dev/null
    send_artdmx_fixed_sequence 128 0 0 255 0 0.3
    wait_status_value diagnostics.artnet.state ACTIVE 40 >/dev/null
    wait_status_value diagnostics.artnet.last_sequence 128 40 >/dev/null
    send_artdmx_fixed_sequence 1 0 255 0 0 4.8 &
    ARTNET_RESTART_SENDER_PID=$!
    wait_status_value diagnostics.artnet.last_sequence 1 40 >/dev/null
    wait_status_value diagnostics.artnet.state ACTIVE 40 >/dev/null
    wait "${ARTNET_RESTART_SENDER_PID}"
    if ask_yes_no "Не позднее timeout после restart Sequence=1 светильник стал стабильно ЗЕЛЁНЫМ?"; then
        record "artnet_restarted_sequence_new_baseline_user: PASS"
    else
        record "artnet_restarted_sequence_new_baseline_user: FAIL"
        exit 1
    fi
    record "artnet_rejected_traffic_lost_recovery: PASS"

    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/color/on' -m '255;0;0'"
    wait_topic "/devices/dmxwb/controls/source" "mqtt" 40 >/dev/null
    wait_state_field red 255 60 >/dev/null

    # Corrupt config fallback must leave the last valid state byte-for-byte intact.
    remote "systemctl stop dmxwb.service"
    wait_topic "/devices/dmxwb/controls/status" "off" 40 >/dev/null
    remote "cp -a '${REMOTE_CONFIG}' '${REMOTE_DIR}/valid-config.json'"
    STATE_SHA_BEFORE_CORRUPT="$(remote "sha256sum '${REMOTE_STATE}' | awk '{print \$1}'" | tr -d '\r')"
    remote "printf '%s\n' '{corrupt-config' > '${REMOTE_CONFIG}'"
    remote "systemctl start dmxwb.service"
    wait_service_active >/dev/null
    wait_topic "/devices/dmxwb/controls/status" "error" 80 >/dev/null
    wait_status_value configuration fallback 80 >/dev/null
    wait_status_value diagnostics.configuration.state fallback 80 >/dev/null
    sleep 3
    remote "systemctl stop dmxwb.service"
    wait_topic "/devices/dmxwb/controls/status" "off" 40 >/dev/null
    STATE_SHA_AFTER_CORRUPT="$(remote "sha256sum '${REMOTE_STATE}' | awk '{print \$1}'" | tr -d '\r')"
    if [[ "${STATE_SHA_AFTER_CORRUPT}" != "${STATE_SHA_BEFORE_CORRUPT}" ]]; then
        echo "Corrupt config fallback изменил ранее корректный state.json." >&2
        exit 1
    fi
    remote "cp -a '${REMOTE_DIR}/valid-config.json' '${REMOTE_CONFIG}'"
    remote "systemctl start dmxwb.service"
    wait_service_active >/dev/null
    wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
    wait_status_value diagnostics.configuration.state ok 80 >/dev/null
    wait_state_field requested_power true 60 >/dev/null
    wait_state_field red 255 60 >/dev/null
    record "corrupt_config_preserves_state_bytes_and_recovers: PASS (${STATE_SHA_BEFORE_CORRUPT})"

    # Real retained/LWT transitions are paired with the current Web availability gate.
    echo "Откройте http://${ARTNET_HOST}/dmxwb/, обновите страницу через Ctrl+F5 и дождитесь состояния «Связь установлена»."
    if ask_yes_no "При работающей службе команды Web доступны?"; then
        record "web_running_commands_available_user: PASS"
    else
        record "web_running_commands_available_user: FAIL"
        exit 1
    fi
    remote "systemctl stop dmxwb.service"
    wait_topic "/devices/dmxwb/controls/status" "off" 40 >/dev/null
    if ask_yes_no "После stop Web без reload показал недоступность DMXWB и заблокировал команды?"; then
        record "web_clean_stop_commands_blocked_user: PASS"
    else
        record "web_clean_stop_commands_blocked_user: FAIL"
        exit 1
    fi
    remote "systemctl start dmxwb.service"
    wait_service_active >/dev/null
    wait_topic "/devices/dmxwb/controls/status" "running" 80 >/dev/null
    if ask_yes_no "После start Web без reload снова разрешил команды?"; then
        record "web_start_commands_restored_user: PASS"
    else
        record "web_start_commands_restored_user: FAIL"
        exit 1
    fi

    STATUS_TRACE="${LOCAL_TMP}/dev012b5-status-trace.txt"
    remote "timeout 20 mosquitto_sub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/status' | awk '
BEGIN { stage = 0 }
\$0 == \"running\" && stage == 0 { print; stage = 1; next }
\$0 == \"off\" && stage == 1 { print; stage = 2; next }
\$0 == \"running\" && stage == 2 { print; exit }
'" > "${STATUS_TRACE}" &
    STATUS_TRACE_PID=$!
    sleep 0.5
    PID_WEB_BEFORE_CRASH="$(service_pid)"
    remote "kill -KILL '${PID_WEB_BEFORE_CRASH}'"
    PID_WEB_AFTER_CRASH="$(wait_service_pid_change "${PID_WEB_BEFORE_CRASH}")"
    wait "${STATUS_TRACE_PID}"
    python3 - "${STATUS_TRACE}" <<'PY'
import sys
values = [line.strip() for line in open(sys.argv[1], encoding='utf-8') if line.strip()]
if values != ['running', 'off', 'running']:
    raise SystemExit(f"unexpected daemon availability trace: {values!r}")
PY
    if ask_yes_no "После crash/recovery Web без reload вернул доступность команд?"; then
        record "web_crash_recovery_commands_restored_user: PASS"
    else
        record "web_crash_recovery_commands_restored_user: FAIL"
        exit 1
    fi
    record "web_daemon_lwt_stop_crash_restart: PASS (${PID_WEB_BEFORE_CRASH} -> ${PID_WEB_AFTER_CRASH})"
else
if (( RUN_COUNTER_ISOLATION_REGRESSION == 1 )); then
    echo
    echo "=== DEV-012B3 MQTT / Art-Net / Source regression ==="
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m artnet"
    wait_topic "/devices/dmxwb/controls/source" "artnet" 40 >/dev/null
    wait_status_value diagnostics.selected_source artnet 80 >/dev/null
    send_artdmx 0 0 255 0
    wait_status_value diagnostics.artnet.state ACTIVE 40 >/dev/null
    wait_status_value diagnostics.artnet.output_mode live 40 >/dev/null
    if [[ "$(service_pid)" != "${PID_START}" ]]; then
        echo "DMXWB PID изменился при переходе на Art-Net." >&2
        exit 1
    fi
    if ask_yes_no "Source=ART-NET: физический светильник стабильно СИНИЙ?"; then
        record "artnet_blue_physical_user: PASS"
    else
        record "artnet_blue_physical_user: FAIL"
        exit 1
    fi
    record "artnet_factual_status_live: PASS"

    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt"
    wait_topic "/devices/dmxwb/controls/source" "mqtt" 40 >/dev/null
    wait_status_value diagnostics.selected_source mqtt 80 >/dev/null
    wait_status_value diagnostics.artnet.output_mode inactive 80 >/dev/null
    if [[ "$(service_pid)" != "${PID_START}" ]]; then
        echo "DMXWB PID изменился при возврате на WB MQTT." >&2
        exit 1
    fi
    if ask_yes_no "После возврата Source=WB MQTT светильник снова стабильно КРАСНЫЙ?"; then
        record "artnet_to_mqtt_red_physical_user: PASS"
    else
        record "artnet_to_mqtt_red_physical_user: FAIL"
        exit 1
    fi
    validate_operational_status >/dev/null
    record "explicit_source_switch_same_process: PASS (pid=${PID_START})"
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
wait_status_value diagnostics.mqtt.state connected 120 >/dev/null
wait_status_value diagnostics.mqtt.recovery_state ok 120 >/dev/null
validate_operational_status >/dev/null
record "mosquitto_restart_without_dmxwb_restart: PASS (pid=${PID_AFTER_MQTT_RESTART})"
record "mqtt_reconnect_operational_state: PASS (state=connected, recovery_state=ok)"
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
echo "Ожидается: видны системные Status/Source и отдельное устройство DEV012B Fixture с его командами."
if ask_yes_no "Стандартный WB UI соответствует этому?"; then
    record "standard_wb_homeui_visible_fixture_user: PASS"
else
    record "standard_wb_homeui_visible_fixture_user: FAIL"
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
if (( RUN_COUNTER_ISOLATION_REGRESSION == 1 )); then
    for token in \
        "dmxwb event=artnet_source state=ACTIVE" \
        "dmxwb event=source_selected source=artnet" \
        "dmxwb event=source_selected source=mqtt"; do
        if ! grep -Fq "${token}" "${LOCAL_TMP}/journal.txt"; then
            echo "В journald отсутствует ожидаемое DEV-012B3 событие: ${token}" >&2
            cat "${LOCAL_TMP}/journal.txt" >&2
            exit 1
        fi
    done
fi
STARTUP_COUNT="$(grep -Fc 'dmxwb event=startup ' "${LOCAL_TMP}/journal.txt" || true)"
if [[ ! "${STARTUP_COUNT}" =~ ^[0-9]+$ ]] || (( STARTUP_COUNT < 4 )); then
    echo "Ожидалось минимум 4 startup events, получено ${STARTUP_COUNT}." >&2
    exit 1
fi
EVENT_COUNT="$(grep -Fc 'dmxwb event=' "${LOCAL_TMP}/journal.txt" || true)"
if [[ ! "${EVENT_COUNT}" =~ ^[0-9]+$ ]] || (( EVENT_COUNT > 100 )); then
    echo "Journald event log не bounded: ${EVENT_COUNT} событий за acceptance." >&2
    exit 1
fi
record "journald_lifecycle_events: PASS"
record "journald_mqtt_recovery_events: PASS"
record "journald_startup_count: ${STARTUP_COUNT}"
record "journald_bounded_event_count: PASS (${EVENT_COUNT})"
fi

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
remote "python3 - '${REMOTE_STATE}' <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    state = json.load(f)
item = next(x for x in state['fixtures'] if x['id'] == 1)
assert item['requested_power'] is False
PY"
record "final_state_flush: PASS"
if ! serial_port_is_released; then
    echo "После clean stop порт ${PORT} остался открыт." >&2
    exit 1
fi
record "final_serial_port_release: PASS"

restore_remote_environment
record "original_environment_restored: PASS"

record "${ACCEPTANCE_VARIANT}_result: PASS"
record "=== DMXWB ${ACCEPTANCE_RESULT} PASS ==="

echo
echo "=== DMXWB ${ACCEPTANCE_RESULT} PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
