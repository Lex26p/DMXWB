#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev011f3_web_config_acceptance.sh SSH_TARGET WEB_HOST [fixture-start-address]

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  WEB_HOST   = 10.200.200.1
  fixture-start-address = 1

Нужны два RGBW Fixture:
  Fixture 1 start = fixture-start-address
  Fixture 2 start = fixture-start-address + 4

DEV-011F3 проверяет:
  - real browser /dmxwb/config/set Apply;
  - Group membership structural draft;
  - factual physical Group control после config Apply;
  - two-tab stale revision conflict;
  - сохранение dirty draft при приходе нового retained config;
  - backend rejection адресно недопустимого full config;
  - старая рабочая config остаётся целой после reject;
  - последующий valid Apply после conflict;
  - continuous physical DMX 44 Hz без ошибок.

Этот подшаг НЕ заявляет динамический rebind DMX Port или Art-Net Port-Address.
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
REPORT="${REPO_ROOT}/docs/DEV011F3_WEB_CONFIG_TRANSACTION_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev011f3-web-config"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev011f3-ssh-${USER:-user}"
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

config_probe() {
    local code="$1"
    mqtt_get "/dmxwb/config" | python3 -c "${code}"
}

config_revision() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["revision"])'
}

config_fixture_count() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["fixtures"]["count"])'
}

config_start_address() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["fixtures"]["start_address"])'
}

config_artnet_universe() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["artnet"]["universe"])'
}

config_dmx_port() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["dmx"]["port"])'
}

config_group_count() {
    config_probe 'import json,sys; print(len(json.load(sys.stdin).get("groups", [])))'
}

config_group1_members() {
    config_probe 'import json,sys; d=json.load(sys.stdin); g=next((x for x in d.get("groups",[]) if x.get("id")==1),None); print("" if g is None else ",".join(str(x) for x in g.get("members",[])))'
}

config_fixture_ids() {
    config_probe 'import json,sys; d=json.load(sys.stdin); print(",".join(str(x["id"]) for x in d["fixtures"]["items"]))'
}

config_next_fixture_id() {
    config_probe 'import json,sys; print(json.load(sys.stdin)["id_counters"]["next_fixture_id"])'
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

state_fixture_field() {
    local fixture_id="$1"
    local field="$2"
    mqtt_get "/dmxwb/state" | python3 -c '
import json, sys
fixture_id = int(sys.argv[1])
field = sys.argv[2]
doc = json.load(sys.stdin)
item = next((x for x in doc.get("fixtures", []) if x.get("id") == fixture_id), None)
if item is None or field not in item:
    raise SystemExit(2)
value = item[field]
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
' "${fixture_id}" "${field}"
}

wait_state_field() {
    local fixture_id="$1"
    local field="$2"
    local expected="$3"
    local attempts="${4:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(state_fixture_field "${fixture_id}" "${field}" 2>/dev/null || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "Fixture ${fixture_id}: ожидалось ${field}='${expected}', получено '${value}'" >&2
    return 1
}

wait_topic() {
    local topic="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(mqtt_get "${topic}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.25
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
    local attempts="${3:-40}"
    local value=""
    for ((i=1; i<=attempts; ++i)); do
        value="$(latest_value "${key}" || true)"
        if [[ "${value}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "Ожидался ${key}=${expected}, последнее '${value}'" >&2
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

for id in 1 2 3; do
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
    "/devices/dmxwb_group_1/meta"
    "/devices/dmxwb_group_1/controls/name/meta"
    "/devices/dmxwb_group_1/controls/name"
    "/devices/dmxwb_group_1/controls/power/meta"
    "/devices/dmxwb_group_1/controls/power"
    "/devices/dmxwb_group_1/controls/red/meta"
    "/devices/dmxwb_group_1/controls/red"
    "/devices/dmxwb_group_1/controls/green/meta"
    "/devices/dmxwb_group_1/controls/green"
    "/devices/dmxwb_group_1/controls/blue/meta"
    "/devices/dmxwb_group_1/controls/blue"
    "/devices/dmxwb_group_1/controls/color/meta"
    "/devices/dmxwb_group_1/controls/color"
    "/devices/dmxwb_group_1/controls/brightness/meta"
    "/devices/dmxwb_group_1/controls/brightness"
    "/devices/dmxwb_group_1/controls/temperature/meta"
    "/devices/dmxwb_group_1/controls/temperature"
    "/devices/dmxwb_group_1/controls/reset/meta"
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
                remote "mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb/controls/source/on' -m mqtt >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_group_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_1/controls/power/on' -m 0 >/dev/null 2>&1 || true; mosquitto_pub -h 127.0.0.1 -p 1883 -t '/devices/dmxwb_fixture_2/controls/power/on' -m 0 >/dev/null 2>&1 || true; sleep 0.5" >/dev/null 2>&1 || true
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
{"version":1,"revision":1,"dmx":{"port":"${PORT}"},"artnet":{"universe":${PORT_ADDRESS}},"fixtures":{"count":2,"start_address":${START_ADDRESS},"items":[{"id":1,"name":"F3 Fixture 1"},{"id":2,"name":"F3 Fixture 2"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":3,"next_group_id":1,"next_scene_id":1}}
JSON

cat > "${LOCAL_TMP}/state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":1,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":2,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON

: > "${REPORT}"
record "=== DMXWB DEV-011F3 REAL WEB CONFIG TRANSACTION ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "web_host: ${WEB_HOST}"
record "fixture_1_start_address: ${FIXTURE1_START}"
record "fixture_2_start_address: ${FIXTURE2_START}"
record "fixed_refresh_hz: 44"

echo "DMXWB — DEV-011F3 browser config transaction acceptance"
echo "WB8/Web:       ${WEB_HOST}"
echo "Fixture 1:     Start Address ${FIXTURE1_START}"
echo "Fixture 2:     Start Address ${FIXTURE2_START}"
echo

if ! ask_yes_no "Два RGBW-светильника подключены к ${PORT} с Start Address ${FIXTURE1_START} и ${FIXTURE2_START}?"; then
    exit 2
fi
if ! ask_yes_no "QLC+/другой Art-Net OUTPUT сейчас полностью отключён?"; then
    echo "Для F3 нужен чистый MQTT physical path." >&2
    exit 2
fi

echo
echo "=== Local checks ==="
python3 "${REPO_ROOT}/tools/web/check_dev011f2_group_membership.py" | tee -a "${REPORT}"
record "local_dev011f2_checks: PASS"

echo
echo "=== WB8 preflight ==="
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "for c in mosquitto_pub mosquitto_sub timeout systemctl ip nginx curl tar grep sed tail readlink python3; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
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
wait_topic "/devices/dmxwb/controls/status" "running"
wait_topic "/devices/dmxwb/controls/source" "mqtt"
wait_config_value config_revision 1 >/dev/null
wait_config_value config_fixture_count 2 >/dev/null
wait_config_value config_group_count 0 >/dev/null
wait_runtime_value status_selected_source mqtt
wait_runtime_value status_dmx_output_running 1
record "unified_runtime_started: PASS"
record "initial_config_revision_1: PASS"

echo
echo "=== Browser normal structural Apply ==="
echo "Откройте штатный WB UI как Administrator, затем одну вкладку DMXWB (Tab A):"
echo "  http://${WEB_HOST}/dmxwb/"
echo
echo "В «Настройки» должно быть:"
echo "  revision 1"
echo "  DMX Port ${PORT}"
echo "  Fixture Count 2"
echo "  Start Address ${START_ADDRESS}"
echo "  Art-Net Universe 0"
echo "  групп нет"
if ! ask_yes_no "Tab A показывает эту исходную конфигурацию и «Связь установлена»?"; then
    record "browser_initial_config_user: FAIL"
    exit 1
fi
record "browser_initial_config_user: PASS"

echo
echo "В Tab A сделайте draft:"
echo "  Art-Net Universe = 17"
echo "  «Добавить группу»"
echo "  в новой Group 1 включите F3 Fixture 1 и F3 Fixture 2"
echo "Остальные structural fields не меняйте."
echo "Затем нажмите «Применить»."
if ! ask_yes_no "Web показал «Конфигурация применена · revision 2», а Group 1 появилась с двумя участниками?"; then
    record "browser_config_apply_revision2_user: FAIL"
    exit 1
fi

wait_config_value config_revision 2 >/dev/null
wait_config_value config_fixture_count 2 >/dev/null
wait_config_value config_start_address "${START_ADDRESS}" >/dev/null
wait_config_value config_artnet_universe 17 >/dev/null
wait_config_value config_dmx_port "${PORT}" >/dev/null
wait_config_value config_group_count 1 >/dev/null
wait_config_value config_group1_members "1,2" >/dev/null
record "browser_config_apply_revision2_factual: PASS"
record "group_membership_config_transaction: PASS (1,2)"

echo
echo "=== Physical proof of applied Group membership ==="
echo "В «Светильники и группы» у Group 1 выберите RED (#ff0000), Brightness=100 и Power ON."
if ! ask_yes_no "Оба физических светильника одновременно КРАСНЫЕ, а Group Power ON?"; then
    record "group_membership_physical_user: FAIL"
    exit 1
fi
for id in 1 2; do
    wait_state_field "${id}" requested_power true
    wait_state_field "${id}" red 255
    wait_state_field "${id}" green 0
    wait_state_field "${id}" blue 0
done
wait_topic "/devices/dmxwb_group_1/controls/power" "1"
record "group_membership_physical_factual: PASS"

echo
echo "=== Two-tab revision conflict ==="
echo "Откройте ВТОРУЮ вкладку того же URL (Tab B) и дождитесь revision 2 в обеих."
if ! ask_yes_no "Tab A и Tab B обе показывают текущую revision 2?"; then
    record "two_tab_revision2_ready_user: FAIL"
    exit 1
fi
record "two_tab_revision2_ready_user: PASS"

echo
echo "Tab A:"
echo "  в «Настройки» выставьте Art-Net Universe = 23"
echo "  НЕ нажимайте «Применить»."
echo "Ожидается локальный draft на base revision 2."
if ! ask_yes_no "В Tab A Universe=23 и draft виден, но backend revision всё ещё 2?"; then
    record "tab_a_dirty_draft_user: FAIL"
    exit 1
fi
wait_config_value config_revision 2 >/dev/null
wait_config_value config_artnet_universe 17 >/dev/null
record "tab_a_dirty_draft_not_published: PASS"

echo
echo "Tab B:"
echo "  Fixture Count = 3"
echo "  нажмите «Применить»."
if ! ask_yes_no "Tab B показал success revision 3 и появился новый Fixture 3?"; then
    record "tab_b_revision3_apply_user: FAIL"
    exit 1
fi
wait_config_value config_revision 3 >/dev/null
wait_config_value config_fixture_count 3 >/dev/null
wait_config_value config_artnet_universe 17 >/dev/null
wait_config_value config_group1_members "1,2" >/dev/null
wait_config_value config_fixture_ids "1,2,3" >/dev/null
wait_config_value config_next_fixture_id 4 >/dev/null
mqtt_get "/dmxwb/config" > "${LOCAL_TMP}/config-revision3.json"
record "tab_b_revision3_apply_factual: PASS"
record "stable_fixture_id_growth: PASS (1,2,3; next=4)"

echo
echo "Теперь вернитесь в Tab A, НЕ перезагружая страницу."
echo "Её local draft должен сохраниться:"
echo "  Art-Net Universe = 23"
echo "  Fixture Count = 2"
echo "  badge: base 2 / current 3"
if ! ask_yes_no "Tab A сохранила старый draft и показывает base 2 / current 3?"; then
    record "tab_a_dirty_draft_preserved_user: FAIL"
    exit 1
fi
record "tab_a_dirty_draft_preserved_user: PASS"

echo
echo "В Tab A нажмите «Применить»."
echo "Ожидается отказ revision_conflict; никакая config не должна измениться."
if ! ask_yes_no "Tab A показала «Конфликт revision: backend уже revision 3»?"; then
    record "tab_a_revision_conflict_user: FAIL"
    exit 1
fi
sleep 0.5
mqtt_get "/dmxwb/config" > "${LOCAL_TMP}/config-after-conflict.json"
python3 - "${LOCAL_TMP}/config-revision3.json" "${LOCAL_TMP}/config-after-conflict.json" <<'PY'
import json, sys
before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
if before != after:
    raise SystemExit("FAIL: stale browser Apply changed canonical config")
PY
wait_config_value config_revision 3 >/dev/null
record "two_tab_revision_conflict_rejected: PASS"
record "stale_apply_left_config_unchanged: PASS"

echo
echo "=== Backend invalid-config rejection ==="
echo "Helper сейчас напрямую отправит full config с текущей revision 3,"
echo "но Start Address=299 при 3 RGBW Fixture. Это выходит за physical slot 300"
echo "и должно быть отклонено backend без изменения рабочей config."

INVALID_REQUEST_ID="dev011f3-invalid-$(date +%s)-$$"
python3 - "${LOCAL_TMP}/config-revision3.json" "${LOCAL_TMP}/invalid-config-set.json" "${INVALID_REQUEST_ID}" <<'PY'
import json, sys
source, output, request_id = sys.argv[1:4]
with open(source, encoding="utf-8") as f:
    cfg = json.load(f)
revision = cfg["revision"]
cfg["fixtures"]["start_address"] = 299
payload = {
    "request_id": request_id,
    "expected_revision": revision,
    "config": cfg,
}
with open(output, "w", encoding="utf-8") as f:
    json.dump(payload, f, ensure_ascii=False, separators=(",", ":"))
PY

scp "${SCP_OPTS[@]}" "${LOCAL_TMP}/invalid-config-set.json" "${TARGET}:${REMOTE_DIR}/invalid-config-set.json" >/dev/null
INVALID_RESULT="$(remote "
rm -f '${REMOTE_DIR}/invalid-result.json'
(timeout 6 mosquitto_sub -h 127.0.0.1 -p 1883 -t '/dmxwb/config/result' -C 1 > '${REMOTE_DIR}/invalid-result.json') &
subpid=\$!
sleep 0.25
mosquitto_pub -h 127.0.0.1 -p 1883 -t '/dmxwb/config/set' -f '${REMOTE_DIR}/invalid-config-set.json'
wait \$subpid
cat '${REMOTE_DIR}/invalid-result.json'
" | tr -d '\r')"

printf '%s' "${INVALID_RESULT}" > "${LOCAL_TMP}/invalid-result.json"
python3 - "${LOCAL_TMP}/invalid-result.json" "${INVALID_REQUEST_ID}" <<'PY'
import json, sys
path, request_id = sys.argv[1:3]
with open(path, encoding="utf-8") as f:
    result = json.load(f)
if result.get("request_id") != request_id:
    raise SystemExit(f"FAIL: wrong request_id in invalid result: {result}")
if result.get("ok") is not False:
    raise SystemExit(f"FAIL: invalid config unexpectedly accepted: {result}")
if result.get("revision") != 3:
    raise SystemExit(f"FAIL: invalid reject returned unexpected revision: {result}")
if result.get("error_code") != "validation":
    raise SystemExit(f"FAIL: expected validation error_code: {result}")
print("invalid_config_result: PASS (validation)")
PY

mqtt_get "/dmxwb/config" > "${LOCAL_TMP}/config-after-invalid.json"
python3 - "${LOCAL_TMP}/config-revision3.json" "${LOCAL_TMP}/config-after-invalid.json" <<'PY'
import json, sys
before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
if before != after:
    raise SystemExit("FAIL: invalid config damaged canonical config")
PY
record "backend_invalid_address_rejected: PASS (validation)"
record "invalid_config_left_old_config_running: PASS"

echo
echo "=== Recover after conflict with valid browser Apply ==="
echo "В Tab A нажмите «Сбросить draft»."
echo "Должны появиться current revision 3: Fixture Count=3, Universe=17."
if ! ask_yes_no "После Reset draft Tab A показывает current revision 3, Count=3, Universe=17?"; then
    record "tab_a_reset_to_current_user: FAIL"
    exit 1
fi

echo "Теперь в Tab A:"
echo "  Fixture Count = 2"
echo "  Art-Net Universe = 0"
echo "  Group 1 оставьте с Fixture 1 и 2"
echo "  нажмите «Применить»."
if ! ask_yes_no "Web показал success revision 4, Fixture 3 исчез, Group 1 сохранилась?"; then
    record "browser_recovery_apply_revision4_user: FAIL"
    exit 1
fi

wait_config_value config_revision 4 >/dev/null
wait_config_value config_fixture_count 2 >/dev/null
wait_config_value config_artnet_universe 0 >/dev/null
wait_config_value config_group1_members "1,2" >/dev/null
wait_config_value config_fixture_ids "1,2" >/dev/null
wait_config_value config_next_fixture_id 4 >/dev/null
record "browser_recovery_apply_revision4_factual: PASS"
record "removed_fixture_id_not_reused: PASS (next_fixture_id=4)"
record "config_revision_sequence: PASS (1->2->3->conflict3->invalid3->4)"

echo
echo "=== Final physical guard ==="
echo "В Group 1 выберите GREEN (#00ff00), Brightness=100, Power ON."
if ! ask_yes_no "Оба физических светильника стабильно ЗЕЛЁНЫЕ после всех config transactions?"; then
    record "final_group_green_physical_user: FAIL"
    exit 1
fi
for id in 1 2; do
    wait_state_field "${id}" requested_power true
    wait_state_field "${id}" red 0
    wait_state_field "${id}" green 255
    wait_state_field "${id}" blue 0
done
record "final_group_green_factual: PASS"

echo "Выключите Group Power."
if ! ask_yes_no "Оба физических светильника полностью OFF, Group Power OFF?"; then
    record "final_group_power_off_user: FAIL"
    exit 1
fi
for id in 1 2; do
    wait_state_field "${id}" requested_power false
done
wait_topic "/devices/dmxwb_group_1/controls/power" "0"
record "final_group_power_off_factual: PASS"

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

for pair in \
    "MQTT_ROUTED:${MQTT_ROUTED}" \
    "ARTNET_ROUTED:${ARTNET_ROUTED}" \
    "DMX_FRAMES:${DMX_FRAMES}" \
    "COMMANDS:${COMMANDS}"; do
    name="${pair%%:*}"
    value="${pair#*:}"
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "Некорректный numeric diagnostic ${name}='${value}'" >&2
        exit 1
    fi
done

if (( MQTT_ROUTED < 5 )); then
    echo "Слишком мало MQTT snapshots: ${MQTT_ROUTED}." >&2
    exit 1
fi
if (( ARTNET_ROUTED != 0 )); then
    echo "В F3 не ожидались Art-Net snapshots: ${ARTNET_ROUTED}." >&2
    exit 1
fi
if (( DMX_FRAMES < 1 )); then
    echo "DmxOutput не передал кадров." >&2
    exit 1
fi
if (( COMMANDS < 5 )); then
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
record "dev011f3_real_web_config_transaction_result: PASS"
record "=== DMXWB DEV-011F3 REAL WEB CONFIG TRANSACTION + REVISION CONFLICT PASS ==="

echo
echo "=== DMXWB DEV-011F3 REAL WEB CONFIG TRANSACTION + REVISION CONFLICT PASS ==="
echo "Report: ${REPORT}"

remote "rm -rf '${REMOTE_DIR}'"
ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
