#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev011e2_web_scene_acceptance.sh SSH_TARGET WEB_HOST [fixture-start-address]

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  WEB_HOST   = 10.200.200.1
  fixture-start-address = 1

Нужны два RGBW Fixture:
  Fixture 1 start = fixture-start-address
  Fixture 2 start = fixture-start-address + 4

DEV-011E2 проверяет Scene lifecycle через реальный browser:
  Create from current state
  Apply
  Overwrite from current state
  Rename
  Delete

Scene Apply дополнительно проверяется как один logical MQTT DMX snapshot:
оба Fixture должны перейти одновременно, без последовательного визуального перебора.

QLC+/другой Art-Net OUTPUT на время теста должен быть полностью отключён.
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
if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 293 )); then
    echo "fixture-start-address должен быть целым числом 1..293." >&2
    exit 2
fi

FIXTURE1_START="${START_ADDRESS}"
FIXTURE2_START=$((START_ADDRESS + 4))

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-dev010-source-acceptance"
REPORT="${REPO_ROOT}/docs/DEV011E2_WEB_SCENE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev011e2-web-scene"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev011e2-ssh-${USER:-user}"
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

for command_name in ssh scp sha256sum awk grep git sed tail tr seq tar python3 cmp; do
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

config_probe() {
    local expression="$1"
    mqtt_get "/dmxwb/config" | python3 -c "${expression}"
}

config_revision() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["revision"])'
}

config_scene_count() {
    config_probe 'import json,sys; print(len(json.load(sys.stdin).get("scenes", [])))'
}

config_scene_name() {
    config_probe 'import json,sys; d=json.load(sys.stdin); s=next((x for x in d.get("scenes",[]) if x.get("id")==1),None); print("" if s is None else s.get("name",""))'
}

wait_config_value() {
    local getter="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(${getter} 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 0.25
    done
    echo "Ожидалось ${getter}='${expected}', получено '${value}'" >&2
    return 1
}

capture_current_scene_expectation() {
    local output_file="$1"
    mqtt_get "/dmxwb/state" | python3 -c '
import json,sys
state=json.load(sys.stdin)
rows=[]
for f in state.get("fixtures", []):
    rows.append({
        "fixture_id": f["id"],
        "red": f["red"],
        "green": f["green"],
        "blue": f["blue"],
        "white": f["white"],
        "brightness": f["brightness"],
        "requested_power": f["requested_power"],
    })
json.dump(rows, sys.stdout, sort_keys=True, separators=(",", ":"))
' > "${output_file}"
}

scene_snapshot_equals_file() {
    local expected_file="$1"
    local current_file="${LOCAL_TMP}/scene-current.json"
    mqtt_get "/dmxwb/config" | python3 -c '
import json,sys
cfg=json.load(sys.stdin)
scene=next((x for x in cfg.get("scenes",[]) if x.get("id")==1),None)
if scene is None:
    raise SystemExit(2)
rows=[]
for f in scene.get("fixtures", []):
    rows.append({
        "fixture_id": f["fixture_id"],
        "red": f["red"],
        "green": f["green"],
        "blue": f["blue"],
        "white": f["white"],
        "brightness": f["brightness"],
        "requested_power": f["requested_power"],
    })
json.dump(rows, sys.stdout, sort_keys=True, separators=(",", ":"))
' > "${current_file}" || return 1
    cmp -s "${expected_file}" "${current_file}"
}

wait_scene_snapshot_file() {
    local expected_file="$1"
    local attempts="${2:-40}"
    for ((i=1; i<=attempts; ++i)); do
        if scene_snapshot_equals_file "${expected_file}"; then
            return 0
        fi
        sleep 0.25
    done
    echo "Scene 1 snapshot не совпал с ожидаемым current-state snapshot." >&2
    echo "expected:" >&2
    cat "${expected_file}" >&2 || true
    echo >&2
    echo "actual config:" >&2
    mqtt_get "/dmxwb/config" >&2 || true
    return 1
}

state_matches_scene_file() {
    local expected_file="$1"
    local current_file="${LOCAL_TMP}/state-current.json"
    mqtt_get "/dmxwb/state" | python3 -c '
import json,sys
state=json.load(sys.stdin)
rows=[]
for f in state.get("fixtures", []):
    rows.append({
        "fixture_id": f["id"],
        "red": f["red"],
        "green": f["green"],
        "blue": f["blue"],
        "white": f["white"],
        "brightness": f["brightness"],
        "requested_power": f["requested_power"],
    })
json.dump(rows, sys.stdout, sort_keys=True, separators=(",", ":"))
' > "${current_file}" || return 1
    cmp -s "${expected_file}" "${current_file}"
}

wait_state_matches_scene_file() {
    local expected_file="$1"
    local attempts="${2:-40}"
    for ((i=1; i<=attempts; ++i)); do
        if state_matches_scene_file "${expected_file}"; then
            return 0
        fi
        sleep 0.25
    done
    echo "Runtime /dmxwb/state не совпал со snapshot сцены." >&2
    return 1
}

state_fixture_rgb() {
    local fixture_id="$1"
    mqtt_get "/dmxwb/state" | python3 -c '
import json,sys
fid=int(sys.argv[1])
d=json.load(sys.stdin)
f=next((x for x in d.get("fixtures",[]) if x.get("id")==fid),None)
if f is None: raise SystemExit(2)
print("{};{};{};{}".format(f["red"], f["green"], f["blue"], 1 if f["requested_power"] else 0))
' "${fixture_id}"
}

wait_state_fixture_rgb() {
    local fixture_id="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(state_fixture_rgb "${fixture_id}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Fixture ${fixture_id}: ожидалось ${expected}, получено '${value}'" >&2
    return 1
}

latest_value() {
    local key="$1"
    remote "grep '^${key}: ' '${REMOTE_DIR}/runtime.log' 2>/dev/null | tail -n1 | sed 's/^${key}: //'" | tr -d '\r'
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

RETAINED_TOPICS=(
    "/dmxwb/config"
    "/dmxwb/state"
    "/dmxwb/status"
    "/devices/dmxwb/meta"
    "/devices/dmxwb/controls/status/meta"
    "/devices/dmxwb/controls/source/meta"
    "/devices/dmxwb/controls/status"
    "/devices/dmxwb/controls/source"
)

for id in 1 2; do
    RETAINED_TOPICS+=(
        "/devices/dmxwb_fixture_${id}/meta"
        "/devices/dmxwb_fixture_${id}/controls/name/meta"
        "/devices/dmxwb_fixture_${id}/controls/name"
        "/devices/dmxwb_fixture_${id}/controls/power/meta"
        "/devices/dmxwb_fixture_${id}/controls/power"
        "/devices/dmxwb_fixture_${id}/controls/red/meta"
        "/devices/dmxwb_fixture_${id}/controls/red"
        "/devices/dmxwb_fixture_${id}/controls/green/meta"
        "/devices/dmxwb_fixture_${id}/controls/green"
        "/devices/dmxwb_fixture_${id}/controls/blue/meta"
        "/devices/dmxwb_fixture_${id}/controls/blue"
        "/devices/dmxwb_fixture_${id}/controls/color/meta"
        "/devices/dmxwb_fixture_${id}/controls/color"
        "/devices/dmxwb_fixture_${id}/controls/brightness/meta"
        "/devices/dmxwb_fixture_${id}/controls/brightness"
        "/devices/dmxwb_fixture_${id}/controls/temperature/meta"
        "/devices/dmxwb_fixture_${id}/controls/temperature"
        "/devices/dmxwb_fixture_${id}/controls/reset/meta"
    )
done

RETAINED_TOPICS+=(
    "/devices/dmxwb_scene_1/meta"
    "/devices/dmxwb_scene_1/controls/name/meta"
    "/devices/dmxwb_scene_1/controls/name"
    "/devices/dmxwb_scene_1/controls/apply/meta"
)

capture_retained() {
    remote "mkdir -p '${REMOTE_DIR}/retained-backup'"
    local index=0
    for topic in "${RETAINED_TOPICS[@]}"; do
        remote "
set +e
if timeout 0.5 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t '${topic}' > '${REMOTE_DIR}/retained-backup/${index}.payload' 2>/dev/null; then
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
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_2/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 0.5" >/dev/null 2>&1 || true
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":2,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"E2 Fixture 1"},{"id":2,"name":"E2 Fixture 2"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":3,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":2,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-011E2 REAL WEB SCENE ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "web_host: ${WEB_HOST}"
record "fixture_1_start_address: ${FIXTURE1_START}"
record "fixture_2_start_address: ${FIXTURE2_START}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-011E2 browser Scene lifecycle + atomic Apply acceptance"
echo "WB8/Web:       ${WEB_HOST}"
echo "Fixture 1:     Start Address ${FIXTURE1_START}"
echo "Fixture 2:     Start Address ${FIXTURE2_START}"
echo

if ! ask_yes_no "Два RGBW-светильника подключены к ${PORT} с Start Address ${FIXTURE1_START} и ${FIXTURE2_START}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    echo "Для E2 нужен чистый MQTT physical path." >&2
    exit 2
fi

echo
echo "=== Local checks ==="
python3 "${REPO_ROOT}/tools/web/check_dev011e1_scene_controls.py" | tee -a "${REPORT}"
record "local_dev011e1_checks: PASS"

python3 - "${REPO_ROOT}" <<'PY' | tee -a "${REPORT}"
from pathlib import Path
import sys

root = Path(sys.argv[1])
controller = (root / "src/mqtt_controller.cpp").read_text(encoding="utf-8")
runtime = (root / "src/mqtt_runtime.cpp").read_text(encoding="utf-8")

begin = controller.find("MqttControllerUpdate MqttController::apply_scene_apply(")
end = controller.find("MqttControllerUpdate MqttController::apply_scene_create(", begin)
if begin < 0 or end < 0:
    raise SystemExit("FAIL: cannot locate apply_scene_apply backend block")
block = controller[begin:end]

mutation = block.find("group_scene_.apply_scene(scene_id, now)")
snapshot = block.find("result.snapshot = build_next_snapshot()")
if mutation < 0 or snapshot < 0 or mutation >= snapshot:
    raise SystemExit("FAIL: Scene Apply does not mutate all Fixtures before snapshot build")
if block.count("build_next_snapshot()") != 1:
    raise SystemExit("FAIL: Scene Apply must build exactly one logical DMX snapshot")

begin = runtime.find("void MqttRuntimeCoordinator::publish_controller_update(")
end = runtime.find("void MqttRuntimeCoordinator::record_route_result(", begin)
if begin < 0 or end < 0:
    raise SystemExit("FAIL: cannot locate MQTT runtime publication block")
runtime_block = runtime[begin:end]
if runtime_block.count("dmx_router_.publish_mqtt_snapshot(*update.snapshot)") != 1:
    raise SystemExit("FAIL: one Controller snapshot must be routed exactly once")

print("dev011e2_scene_apply_mutate_then_single_snapshot_contract: PASS")
print("dev011e2_mqtt_runtime_single_snapshot_route_contract: PASS")
PY
record "local_scene_atomic_backend_contract: PASS"

echo
echo "=== WB8 preflight ==="
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

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
echo "Сохраняем retained topics..."
capture_retained
record "original_retained_topics_captured: PASS"

scp "${SCP_OPTS[@]}" "${BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/" >/dev/null
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-dev010-source-acceptance'"

echo "Разворачиваем текущий web в /var/www/dmxwb..."
tar -C "${REPO_ROOT}/www" -czf - dmxwb | remote "rm -rf /var/www/dmxwb && tar -m -C /var/www -xzf -"
remote "find /var/www/dmxwb -type f -exec chmod 0644 {} +; find /var/www/dmxwb -type d -exec chmod 0755 {} +"
remote "curl -fsS http://127.0.0.1/dmxwb/ | grep -q '<title>DMXWB</title>'"
record "wb_static_web_deployed: PASS"

RUNTIME_PID="$(remote "'${REMOTE_DIR}/dmxwb-dev010-source-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' --development-oem-code FFFF --mac '${WB_MAC}' --status-interval-ms 200 >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r')"
RUNTIME_STARTED=1
sleep 2
if ! remote "kill -0 '${RUNTIME_PID}' 2>/dev/null"; then
    remote "cat '${REMOTE_DIR}/runtime.log'" >&2 || true
    exit 1
fi
wait_topic "/devices/dmxwb/controls/status" "running" >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" >/dev/null
wait_config_value config_revision 1 >/dev/null
wait_config_value config_scene_count 0 >/dev/null
wait_runtime_value status_selected_source mqtt >/dev/null
wait_runtime_value status_dmx_output_running 1 >/dev/null
record "unified_runtime_started: PASS"
record "initial_scene_count_zero: PASS"

echo
echo "=== Browser initial state ==="
echo "Откройте штатный WB UI как Administrator, затем откройте/обновите один раз:"
echo "  http://${WEB_HOST}/dmxwb/"
echo "В разделе «Сцены» ожидается пустой список и поле создания новой сцены."
if ask_yes_no "Web подключён, видны два Fixture и пустой раздел «Сцены»?"; then
    record "browser_initial_scene_state_user: PASS"
else
    record "browser_initial_scene_state_user: FAIL"
    exit 1
fi

echo
echo "=== Prepare Scene A snapshot ==="
echo "Через индивидуальные Fixture controls выставьте:"
echo "  E2 Fixture 1: чистый RED (#ff0000), Brightness=100, Power ON"
echo "  E2 Fixture 2: чистый BLUE (#0000ff), Brightness=100, Power ON"
if ask_yes_no "Fixture 1 физически КРАСНЫЙ, Fixture 2 физически СИНИЙ и Web показывает эти состояния?"; then
    record "scene_a_prepare_physical_user: PASS"
else
    record "scene_a_prepare_physical_user: FAIL"
    exit 1
fi
wait_state_fixture_rgb 1 "255;0;0;1"
wait_state_fixture_rgb 2 "0;0;255;1"
capture_current_scene_expectation "${LOCAL_TMP}/scene-a.json"
record "scene_a_prepare_factual_state: PASS"

echo
echo "=== Scene Create ==="
echo "В разделе «Сцены» введите имя точно:"
echo "  DEV011 E2 Scene"
echo "и нажмите «Создать из текущего состояния»."
if ask_yes_no "Появилась карточка сцены и сообщение об успешной операции revision 2?"; then
    record "web_scene_create_user: PASS"
else
    record "web_scene_create_user: FAIL"
    exit 1
fi
wait_config_value config_revision 2 >/dev/null
wait_config_value config_scene_count 1 >/dev/null
wait_config_value config_scene_name "DEV011 E2 Scene" >/dev/null
wait_scene_snapshot_file "${LOCAL_TMP}/scene-a.json"
wait_topic "/devices/dmxwb_scene_1/controls/name" "DEV011 E2 Scene" >/dev/null
wait_topic "/devices/dmxwb/controls/source" "mqtt" >/dev/null
record "web_scene_create_factual_confirmation: PASS"

echo
echo "=== Scene Apply A — atomic physical transition ==="
echo "Сначала измените ОБА Fixture на чистый GREEN (#00ff00), Power ON, Brightness=100."
if ask_yes_no "Оба физических светильника сейчас стабильно ЗЕЛЁНЫЕ?"; then
    record "scene_a_apply_guard_green_user: PASS"
else
    record "scene_a_apply_guard_green_user: FAIL"
    exit 1
fi
wait_state_fixture_rgb 1 "0;255;0;1"
wait_state_fixture_rgb 2 "0;255;0;1"
sleep 0.5

echo "Теперь нажмите «Применить» у сцены DEV011 E2 Scene."
if ask_yes_no "Оба Fixture одним визуальным переходом (без последовательного перебора) вернулись: Fixture 1 RED, Fixture 2 BLUE, и Web сообщил подтверждение runtime state?"; then
    record "web_scene_apply_a_atomic_physical_user: PASS"
else
    record "web_scene_apply_a_atomic_physical_user: FAIL"
    exit 1
fi
wait_state_matches_scene_file "${LOCAL_TMP}/scene-a.json"
wait_topic "/devices/dmxwb/controls/source" "mqtt" >/dev/null
record "web_scene_apply_a_factual_confirmation: PASS"
record "web_scene_apply_a_atomic_contract: PASS (single backend snapshot + one physical transition)"
record "scene_apply_did_not_switch_source: PASS"

echo
echo "=== Scene Overwrite ==="
echo "Подготовьте новое состояние:"
echo "  E2 Fixture 1: GREEN (#00ff00), Power ON, Brightness=100"
echo "  E2 Fixture 2: RED   (#ff0000), Power ON, Brightness=100"
if ask_yes_no "Физически Fixture 1 GREEN, Fixture 2 RED?"; then
    record "scene_b_prepare_physical_user: PASS"
else
    record "scene_b_prepare_physical_user: FAIL"
    exit 1
fi
wait_state_fixture_rgb 1 "0;255;0;1"
wait_state_fixture_rgb 2 "255;0;0;1"
capture_current_scene_expectation "${LOCAL_TMP}/scene-b.json"

echo "Нажмите «Перезаписать» у сцены и подтвердите browser dialog."
if ask_yes_no "Web сообщил успешную операцию revision 3, а физический свет НЕ изменился?"; then
    record "web_scene_overwrite_user: PASS"
else
    record "web_scene_overwrite_user: FAIL"
    exit 1
fi
wait_config_value config_revision 3 >/dev/null
wait_scene_snapshot_file "${LOCAL_TMP}/scene-b.json"
wait_state_matches_scene_file "${LOCAL_TMP}/scene-b.json"
record "web_scene_overwrite_factual_confirmation: PASS"

echo
echo "=== Apply overwritten Scene B ==="
echo "Теперь выставьте ОБА Fixture на BLUE (#0000ff), Power ON, Brightness=100."
if ask_yes_no "Оба светильника сейчас BLUE?"; then
    record "scene_b_apply_guard_blue_user: PASS"
else
    record "scene_b_apply_guard_blue_user: FAIL"
    exit 1
fi
wait_state_fixture_rgb 1 "0;0;255;1"
wait_state_fixture_rgb 2 "0;0;255;1"
sleep 0.5

echo "Нажмите «Применить» у сцены."
if ask_yes_no "Одним визуальным переходом Fixture 1 стал GREEN, Fixture 2 RED, и Web подтвердил runtime state?"; then
    record "web_scene_apply_b_atomic_physical_user: PASS"
else
    record "web_scene_apply_b_atomic_physical_user: FAIL"
    exit 1
fi
wait_state_matches_scene_file "${LOCAL_TMP}/scene-b.json"
wait_topic "/devices/dmxwb/controls/source" "mqtt" >/dev/null
record "web_scene_apply_b_factual_confirmation: PASS"
record "web_scene_apply_b_atomic_contract: PASS (single backend snapshot + one physical transition)"

echo
echo "=== Scene Rename ==="
echo "В поле имени сцены введите точно:"
echo "  DEV011 E2 Scene Renamed"
echo "и нажмите Enter либо кликните вне поля."
if ask_yes_no "Имя обновилось без reload, Web показал «Имя сцены подтверждено backend.»?"; then
    record "web_scene_rename_user: PASS"
else
    record "web_scene_rename_user: FAIL"
    exit 1
fi
wait_config_value config_revision 4 >/dev/null
wait_config_value config_scene_name "DEV011 E2 Scene Renamed" >/dev/null
wait_topic "/devices/dmxwb_scene_1/controls/name" "DEV011 E2 Scene Renamed" >/dev/null
record "web_scene_rename_factual_confirmation: PASS"

echo
echo "=== Scene Delete ==="
echo "Нажмите «Удалить» у сцены и подтвердите browser dialog."
if ask_yes_no "Карточка сцены исчезла, Web сообщил успешную операцию revision 5, физический свет не изменился?"; then
    record "web_scene_delete_user: PASS"
else
    record "web_scene_delete_user: FAIL"
    exit 1
fi
wait_config_value config_revision 5 >/dev/null
wait_config_value config_scene_count 0 >/dev/null
if [[ -n "$(mqtt_get '/devices/dmxwb_scene_1/controls/name' || true)" ]]; then
    echo "Retained Scene name не очищен после Delete." >&2
    exit 1
fi
wait_topic "/devices/dmxwb/controls/source" "mqtt" >/dev/null
record "web_scene_delete_factual_confirmation: PASS"
record "scene_lifecycle_revision_sequence: PASS (1->2->3->4->5)"

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

MQTT_ROUTED="$(final_value final_router_mqtt_snapshots_received || true)"
ARTNET_ROUTED="$(final_value final_router_artnet_snapshots_received || true)"
DMX_FRAMES="$(final_value final_dmx_frames_sent || true)"
COMMANDS="$(final_value final_mqtt_runtime_commands_processed || true)"
for pair in "MQTT_ROUTED:${MQTT_ROUTED}" "ARTNET_ROUTED:${ARTNET_ROUTED}" "DMX_FRAMES:${DMX_FRAMES}" "COMMANDS:${COMMANDS}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done
if (( MQTT_ROUTED < 10 )); then
    echo "Слишком мало MQTT snapshots: ${MQTT_ROUTED}." >&2
    exit 1
fi
if (( ARTNET_ROUTED != 0 )); then
    echo "В E2 не ожидались Art-Net snapshots: ${ARTNET_ROUTED}." >&2
    exit 1
fi
if (( DMX_FRAMES < 1 )); then
    echo "DmxOutput не передал кадров." >&2
    exit 1
fi
if (( COMMANDS < 10 )); then
    echo "Слишком мало MQTT commands: ${COMMANDS}." >&2
    exit 1
fi
record "router_mqtt_snapshots: PASS (${MQTT_ROUTED})"
record "router_artnet_snapshots_absent: PASS (${ARTNET_ROUTED})"
record "mqtt_commands_processed: PASS (${COMMANDS})"
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
record "dev011e2_real_web_scene_result: PASS"
record "=== DMXWB DEV-011E2 REAL WEB SCENE LIFECYCLE + ATOMIC APPLY PASS ==="

echo
echo "=== DMXWB DEV-011E2 REAL WEB SCENE LIFECYCLE + ATOMIC APPLY PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
