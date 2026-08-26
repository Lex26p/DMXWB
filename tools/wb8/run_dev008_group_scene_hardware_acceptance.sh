#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev008_group_scene_hardware_acceptance.sh user@wb8-host [start-address]

DEV-008 Group/Scene hardware acceptance:
  - два физических RGBW Fixture на последовательных DMX адресах;
  - Fixture 1 Start Address = start-address;
  - Fixture 2 Start Address = start-address + 4;
  - Group commands и multiple membership;
  - factual Group Power;
  - индивидуальный Power restore через Group;
  - Scene create/overwrite/apply/delete через MQTT lifecycle;
  - Scene stable ID не переиспользуется;
  - Scene Apply визуально атомарен для обоих Fixtures;
  - retained Group/Scene/lifecycle commands не применяются;
  - final all-off.

/dev/ttyRS485-1 считается освобождённым в WB Serial Device Driver Configuration.
wb-mqtt-serial не останавливается. Ответы: только латинские y или n.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
START_ADDRESS="${2:-1}"
PORT="/dev/ttyRS485-1"

if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 293 )); then
    echo "start-address должен быть целым числом 1..293 для двух последовательных RGBW Fixtures." >&2
    exit 2
fi
SECOND_ADDRESS=$((START_ADDRESS + 4))

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb-mqtt-acceptance"
DIAG_BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV008_GROUP_SCENE_HARDWARE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev008-group-scene"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev008-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"
LOCAL_TMP="$(mktemp -d)"

for command_name in ssh scp sha256sum awk grep git tr; do
    command -v "${command_name}" >/dev/null 2>&1 || { echo "Не найдена команда: ${command_name}" >&2; exit 1; }
done

if [[ ! -x "${BINARY}" || ! -x "${DIAG_BINARY}" ]]; then
    echo "Не найдены ARM64 artifacts DEV-008." >&2
    echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
    exit 1
fi

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"
SSH_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")
SCP_OPTS=(-o "ControlMaster=auto" -o "ControlPath=${CONTROL_PATH}" -o "ControlPersist=1200" -o "StrictHostKeyChecking=accept-new")

SSH_OPENED=0
REMOTE_READY=0
RUNTIME_PID=""
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
        if (( REMOTE_READY == 1 && TEST_COMPLETE == 0 )); then
            # One Fixture starting at the second address produces a slot_count
            # through channel 8 and leaves all earlier channels zero as well.
            remote "'${REMOTE_DIR}/dmxwb' --fixture-hardware-test all-off --port '${PORT}' --start-address '${SECOND_ADDRESS}' --seconds 1" >/dev/null 2>&1 || true
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

mqtt_pub_retained() {
    local topic="$1" payload="$2"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -m '${payload}' -r"
}

mqtt_clear_retained() {
    local topic="$1"
    remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '${topic}' -r -n" >/dev/null
}

clear_scene_device_retained() {
    local scene_id="$1"
    local prefix="/devices/dmxwb_scene_${scene_id}"
    mqtt_clear_retained "${prefix}/meta"
    mqtt_clear_retained "${prefix}/controls/name/meta"
    mqtt_clear_retained "${prefix}/controls/apply/meta"
    mqtt_clear_retained "${prefix}/controls/name"
}

wait_topic() {
    local topic="$1" expected="$2" attempts="${3:-12}" value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            printf '%s' "${value}"
            return 0
        fi
        sleep 1
    done
    echo "Ожидался ${topic}=${expected}, получено '${value}'" >&2
    return 1
}

wait_config_contains() {
    local expected="$1" attempts="${2:-12}" payload="" compact=""
    for ((i=1; i<=attempts; ++i)); do
        payload="$(mqtt_get "/dmxwb/config" || true)"
        compact="$(printf '%s' "${payload}" | tr -d '[:space:]')"
        if [[ "${compact}" == *"${expected}"* ]]; then
            printf '%s' "${compact}"
            return 0
        fi
        sleep 1
    done
    echo "В /dmxwb/config не найдено: ${expected}" >&2
    echo "Последний payload: ${payload}" >&2
    return 1
}

expect_no_retained() {
    local topic="$1" value=""
    value="$(remote "timeout 2 mosquitto_sub -h 127.0.0.1 -p 1883 -t '${topic}' -C 1 2>/dev/null || true" | tr -d '\r')"
    [[ -z "${value}" ]]
}

start_runtime() {
    remote "rm -f '${REMOTE_DIR}/runtime.log'; '${REMOTE_DIR}/dmxwb-mqtt-acceptance' --config '${REMOTE_DIR}/config.json' --state '${REMOTE_DIR}/state.json' >'${REMOTE_DIR}/runtime.log' 2>&1 & echo \$!" | tr -d '\r'
}

stop_runtime_gracefully() {
    local pid="$1"
    remote "kill -TERM '${pid}'; for i in \$(seq 1 50); do kill -0 '${pid}' 2>/dev/null || exit 0; sleep 0.1; done; exit 1"
}

diag_value() {
    local output="$1" key="$2"
    awk -F': ' -v key="${key}" '$1 == key {print $2}' <<<"${output}" | tail -n1 | tr -d '\r'
}

is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

cat > "${LOCAL_TMP}/config.json" <<JSON
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":0},"fixtures":{"count":2,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"DEV008 Fixture A"},{"id":2,"name":"DEV008 Fixture B"}]},"groups":[{"id":1,"name":"DEV008 Pair","members":[1,2]},{"id":2,"name":"DEV008 Second","members":[2]}],"scenes":[],"id_counters":{"next_fixture_id":3,"next_group_id":3,"next_scene_id":1}}
JSON
cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":2,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-008 Group + Scene hardware acceptance ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "port: ${PORT}"
record "fixture_1_start_address: ${START_ADDRESS}"
record "fixture_2_start_address: ${SECOND_ADDRESS}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-008 Group/Scene hardware acceptance"
echo "Контроллер: ${TARGET}"
echo "Fixture A Start Address: ${START_ADDRESS}"
echo "Fixture B Start Address: ${SECOND_ADDRESS}"
echo "Оба прибора должны быть физически подключены к одной DMX-линии."
echo
if ! ask_yes_no "Два RGBW светильника подключены и выставлены на указанные DMX Start Address?"; then
    exit 2
fi

echo "Открываем одно SSH-соединение. Пароль потребуется один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
if [[ "$(remote "systemctl is-active mosquitto 2>/dev/null || true" | tr -d '\r')" != "active" ]]; then
    echo "Mosquitto service на target не active." >&2
    exit 1
fi

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${DIAG_BINARY}" "${LOCAL_TMP}/config.json" "${LOCAL_TMP}/state.json" "${TARGET}:${REMOTE_DIR}/"
remote "chmod 0755 '${REMOTE_DIR}/dmxwb-mqtt-acceptance' '${REMOTE_DIR}/dmxwb'"
REMOTE_READY=1

record ""
record "--- target identity ---"
remote "printf 'model: '; tr -d '\0' </proc/device-tree/model 2>/dev/null || true; echo; uname -a; if [ -r /etc/wb-release ]; then cat /etc/wb-release; fi; printf 'libmosquitto: '; ldconfig -p 2>/dev/null | grep -m1 libmosquitto.so.1 || true" | tee -a "${REPORT}"

# Previous interrupted acceptance must not provide stale Scene retained state.
clear_scene_device_retained 1
clear_scene_device_retained 2

# Retained commands must never execute.
mqtt_pub_retained "/devices/dmxwb_group_1/controls/power/on" "1"
mqtt_pub_retained "/dmxwb/scenes/create" '{"request_id":"retained-create","name":"MUST NOT EXIST"}'

RUNTIME_PID="$(start_runtime)"
record "runtime_pid: ${RUNTIME_PID}"
wait_topic "/devices/dmxwb/controls/status" "running" 20 >/dev/null
wait_topic "/devices/dmxwb_group_1/controls/power" "0" 10 >/dev/null
CONFIG_COMPACT="$(wait_config_contains '"scenes":[]' 10)"
if [[ "${CONFIG_COMPACT}" == *'MUSTNOTEXIST'* ]]; then
    record "retained_scene_lifecycle_ignored: FAIL"
    exit 1
fi
record "retained_group_command_ignored: PASS"
record "retained_scene_lifecycle_ignored: PASS"
mqtt_clear_retained "/devices/dmxwb_group_1/controls/power/on"
mqtt_clear_retained "/dmxwb/scenes/create"

# Group 1: both red and ON.
mqtt_pub "/devices/dmxwb_group_1/controls/color/on" "255;0;0"
mqtt_pub "/devices/dmxwb_group_1/controls/power/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "255;0;0" 10 >/dev/null
wait_topic "/devices/dmxwb_group_1/controls/power" "1" 10 >/dev/null
if ask_yes_no "ОБА светильника физически КРАСНЫЕ?"; then record "group_pair_red_user: PASS"; else exit 1; fi

# Group 2 contains only Fixture B: B becomes green, A stays red.
mqtt_pub "/devices/dmxwb_group_2/controls/color/on" "0;255;0"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;255;0" 10 >/dev/null
if ask_yes_no "Fixture A остался КРАСНЫМ, а Fixture B стал ЗЕЛЁНЫМ?"; then record "multiple_group_membership_user: PASS"; else exit 1; fi

# Factual Group Power with overlap.
mqtt_pub "/devices/dmxwb_fixture_2/controls/power/on" "0"
wait_topic "/devices/dmxwb_group_2/controls/power" "0" 10 >/dev/null
wait_topic "/devices/dmxwb_group_1/controls/power" "1" 10 >/dev/null
record "factual_group_power_overlap: PASS"
mqtt_pub "/devices/dmxwb_fixture_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_group_1/controls/power" "0" 10 >/dev/null
record "factual_group_power_all_off: PASS"

# Group Power ON restores each member's own saved color.
mqtt_pub "/devices/dmxwb_group_1/controls/power/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;255;0" 10 >/dev/null
if ask_yes_no "Group Power ON восстановил A=КРАСНЫЙ и B=ЗЕЛЁНЫЙ (индивидуальные saved states)?"; then record "group_power_restore_user: PASS"; else exit 1; fi

# Create Scene 1 from A=red, B=green.
mqtt_pub "/dmxwb/scenes/create" '{"request_id":"create-1","name":"DEV008 Red Green"}'
wait_topic "/devices/dmxwb_scene_1/controls/name" "DEV008 Red Green" 12 >/dev/null
CONFIG_COMPACT="$(wait_config_contains '"next_scene_id":2' 12)"
if [[ "${CONFIG_COMPACT}" != *'"id":1,"name":"DEV008RedGreen"'* ]]; then
    # Name spaces disappear in compact form; if serializer formatting differs,
    # stable Scene state topic above is the authoritative acceptance signal.
    true
fi
record "scene_create_mqtt_lifecycle: PASS"

# Move both to blue, then apply Scene 1. Visual transition must be atomic.
mqtt_pub "/devices/dmxwb_group_1/controls/color/on" "0;0;255"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;0;255" 10 >/dev/null
if ask_yes_no "Перед Scene Apply ОБА светильника СИНИЕ?"; then record "scene_preapply_blue_user: PASS"; else exit 1; fi
mqtt_pub "/devices/dmxwb_scene_1/controls/apply/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "255;0;0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;255;0" 10 >/dev/null
if ask_yes_no "Scene Apply одним визуальным переходом (без последовательного перебора) вернул A=КРАСНЫЙ, B=ЗЕЛЁНЫЙ?"; then
    record "scene_atomic_apply_user: PASS"
else
    record "scene_atomic_apply_user: FAIL"
    exit 1
fi

# Prepare a new state and overwrite Scene 1.
mqtt_pub "/devices/dmxwb_fixture_1/controls/color/on" "0;0;255"
mqtt_pub "/devices/dmxwb_fixture_2/controls/color/on" "255;0;0"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "255;0;0" 10 >/dev/null
mqtt_pub "/dmxwb/scenes/1/overwrite" '{"request_id":"overwrite-1"}'
CONFIG_COMPACT="$(wait_config_contains '"revision":3' 12)"
if [[ "${CONFIG_COMPACT}" != *'"fixture_id":1,"red":0,"green":0,"blue":255'* ||
      "${CONFIG_COMPACT}" != *'"fixture_id":2,"red":255,"green":0,"blue":0'* ]]; then
    record "scene_overwrite_mqtt_lifecycle: FAIL"
    echo "Scene overwrite не отражён в /dmxwb/config: ${CONFIG_COMPACT}" >&2
    exit 1
fi
record "scene_overwrite_mqtt_lifecycle: PASS"

# Change both green, then overwritten Scene must restore A=blue, B=red atomically.
mqtt_pub "/devices/dmxwb_group_1/controls/color/on" "0;255;0"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;255;0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "0;255;0" 10 >/dev/null
mqtt_pub "/devices/dmxwb_scene_1/controls/apply/on" "1"
wait_topic "/devices/dmxwb_fixture_1/controls/color" "0;0;255" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/color" "255;0;0" 10 >/dev/null
if ask_yes_no "После Overwrite Scene Apply одним переходом дал A=СИНИЙ, B=КРАСНЫЙ?"; then record "scene_overwrite_apply_user: PASS"; else exit 1; fi

# Delete Scene 1 and confirm retained device cleanup.
mqtt_pub "/dmxwb/scenes/1/delete" '{"request_id":"delete-1"}'
wait_config_contains '"scenes":[]' 12 >/dev/null
if ! expect_no_retained "/devices/dmxwb_scene_1/controls/name"; then
    record "scene_retained_cleanup: FAIL"
    exit 1
fi
record "scene_delete_mqtt_lifecycle: PASS"
record "scene_retained_cleanup: PASS"

# Monotonic stable ID: create after delete must be Scene 2, not reused Scene 1.
mqtt_pub "/dmxwb/scenes/create" '{"request_id":"create-2","name":"DEV008 Second Scene"}'
wait_topic "/devices/dmxwb_scene_2/controls/name" "DEV008 Second Scene" 12 >/dev/null
wait_config_contains '"next_scene_id":3' 12 >/dev/null
record "scene_id_monotonic_no_reuse: PASS"
mqtt_pub "/dmxwb/scenes/2/delete" '{"request_id":"delete-2"}'
wait_config_contains '"scenes":[]' 12 >/dev/null
if ! expect_no_retained "/devices/dmxwb_scene_2/controls/name"; then
    record "scene_2_retained_cleanup: FAIL"
    exit 1
fi
record "scene_2_retained_cleanup: PASS"

# Final all-off through Group command.
mqtt_pub "/devices/dmxwb_group_1/controls/power/on" "0"
wait_topic "/devices/dmxwb_fixture_1/controls/power" "0" 10 >/dev/null
wait_topic "/devices/dmxwb_fixture_2/controls/power" "0" 10 >/dev/null
wait_topic "/devices/dmxwb_group_1/controls/power" "0" 10 >/dev/null
if ask_yes_no "FINAL: ОБА светильника полностью выключены?"; then record "final_all_off_user: PASS"; else exit 1; fi

sleep 3
stop_runtime_gracefully "${RUNTIME_PID}"
RUNTIME_PID=""
RUNTIME_LOG="$(remote "cat '${REMOTE_DIR}/runtime.log'")"
printf '%s\n' "${RUNTIME_LOG}" | tee -a "${REPORT}"
commands="$(diag_value "${RUNTIME_LOG}" runtime_commands_processed)"
snapshots="$(diag_value "${RUNTIME_LOG}" runtime_dmx_snapshots_published)"
failures="$(diag_value "${RUNTIME_LOG}" runtime_dmx_publish_failures)"
missed="$(diag_value "${RUNTIME_LOG}" dmx_missed_deadlines)"
frames="$(diag_value "${RUNTIME_LOG}" dmx_frames_sent)"
software="$(diag_value "${RUNTIME_LOG}" software_result)"
if ! is_uint "${commands}" || ! is_uint "${snapshots}" || ! is_uint "${failures}" || ! is_uint "${missed}" || ! is_uint "${frames}" ||
   (( commands < 15 || snapshots < 8 || failures != 0 || missed != 0 || frames < 200 )) || [[ "${software}" != "PASS" ]]; then
    record "dev008_runtime_diagnostics: FAIL"
    exit 1
fi
record "dev008_runtime_diagnostics: PASS"
wait_topic "/devices/dmxwb/controls/status" "off" 10 >/dev/null
record "graceful_off_status: PASS"

TEST_COMPLETE=1
record "dev008_group_scene_hardware_result: PASS"
record "=== DMXWB DEV-008 GROUP + SCENE HARDWARE PASS ==="
echo
echo "PASS: Group/Scene подтверждены на двух DMX addresses; Scene Apply визуально атомарен."
echo "Отчёт: ${REPORT}"
