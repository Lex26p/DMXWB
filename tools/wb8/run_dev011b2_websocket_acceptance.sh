#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev011b2_websocket_acceptance.sh SSH_TARGET WEB_HOST

Для текущего стенда:
  SSH_TARGET = root@10.200.200.1
  WEB_HOST   = 10.200.200.1

Проверяет:
  - штатный nginx /var/www и /mqtt на WB8;
  - реальную загрузку /dmxwb/ через nginx;
  - browser -> nginx /mqtt -> Mosquitto WebSocket;
  - retained /dmxwb/config, /dmxwb/state, /dmxwb/status;
  - реальный broker stop/start;
  - automatic reconnect + resubscribe без reload страницы.

Helper не меняет nginx/mosquitto config.
Исходные retained /dmxwb/config|state|status сохраняются и восстанавливаются.
Ответы на визуальные вопросы: только латинские y или n.
USAGE
}

if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi

SSH_TARGET="$1"
WEB_HOST="$2"

REPO_ROOT="$(git rev-parse --show-toplevel)"
REPORT="${REPO_ROOT}/docs/DEV011B2_MQTT_WEBSOCKET_REPORT.txt"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev011b2-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control"
LOCAL_TMP="$(mktemp -d)"
REMOTE_TMP="/tmp/dmxwb-dev011b2"

SSH_OPTS=(
    -o ControlMaster=auto
    -o ControlPersist=120
    -o "ControlPath=${CONTROL_PATH}"
    -o ServerAliveInterval=10
    -o ServerAliveCountMax=3
)

SSH_OPENED=0
ORIGINALS_CAPTURED=0
MOSQUITTO_STOPPED=0
CLEANED=0

mkdir -p "$(dirname "${REPORT}")" "${CONTROL_DIR}"
: > "${REPORT}"

record() {
    printf '%s\n' "$*" | tee -a "${REPORT}"
}

ask_yes_no() {
    local prompt="$1"
    local answer
    while true; do
        read -r -p "${prompt} (y/n): " answer
        case "${answer}" in
            y|Y) return 0 ;;
            n|N) return 1 ;;
            *) echo "Введите только y или n." ;;
        esac
    done
}

remote() {
    ssh "${SSH_OPTS[@]}" "${SSH_TARGET}" "$@"
}

restore_originals() {
    if (( ORIGINALS_CAPTURED == 0 )); then
        return 0
    fi

    remote "
set -eu
restore_one() {
    topic=\"\$1\"
    file=\"\$2\"
    present=\"\$3\"
    if [[ \"\$(cat \"\${present}\")\" == 1 ]]; then
        mosquitto_pub -h 127.0.0.1 -p 1883 -r -t \"\${topic}\" -f \"\${file}\"
    else
        mosquitto_pub -h 127.0.0.1 -p 1883 -r -n -t \"\${topic}\"
    fi
}
restore_one /dmxwb/config ${REMOTE_TMP}/original-config.json ${REMOTE_TMP}/original-config.present
restore_one /dmxwb/state  ${REMOTE_TMP}/original-state.json  ${REMOTE_TMP}/original-state.present
restore_one /dmxwb/status ${REMOTE_TMP}/original-status.json ${REMOTE_TMP}/original-status.present
"
    ORIGINALS_CAPTURED=0
}

cleanup() {
    local status=$?
    set +e

    if (( CLEANED == 0 )); then
        if (( SSH_OPENED == 1 )); then
            if (( MOSQUITTO_STOPPED == 1 )); then
                remote "systemctl start mosquitto" >/dev/null 2>&1 || true
                MOSQUITTO_STOPPED=0
            fi
            restore_originals >/dev/null 2>&1 || true
            remote "rm -rf '${REMOTE_TMP}'" >/dev/null 2>&1 || true
            ssh "${SSH_OPTS[@]}" -O exit "${SSH_TARGET}" >/dev/null 2>&1 || true
        fi
        rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}" >/dev/null 2>&1 || true
        CLEANED=1
    fi

    exit "${status}"
}
trap cleanup EXIT INT TERM

publish_phase() {
    local phase="$1"
    remote "
set -eu
mosquitto_pub -h 127.0.0.1 -p 1883 -r -t /dmxwb/config -f '${REMOTE_TMP}/${phase}-config.json'
mosquitto_pub -h 127.0.0.1 -p 1883 -r -t /dmxwb/state  -f '${REMOTE_TMP}/${phase}-state.json'
mosquitto_pub -h 127.0.0.1 -p 1883 -r -t /dmxwb/status -f '${REMOTE_TMP}/${phase}-status.json'
"
}

record "=== DMXWB DEV-011B2 REAL WB8 MQTT WEBSOCKET ACCEPTANCE ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
record "source_worktree: $([[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && echo modified || echo clean)"
record "ssh_target: ${SSH_TARGET}"
record "web_host: ${WEB_HOST}"
record "web_path: /dmxwb/"
record "mqtt_websocket_path: /mqtt"

echo "=== Local static checks ==="
python3 "${REPO_ROOT}/tools/web/check_dev011b_mqtt.py" | tee -a "${REPORT}"
record "local_static_checks: PASS"

echo
echo "=== WB8 preflight ==="
ssh "${SSH_OPTS[@]}" -MNf "${SSH_TARGET}"
SSH_OPENED=1

remote "for c in nginx mosquitto_pub mosquitto_sub systemctl timeout tar grep curl; do command -v \$c >/dev/null || { echo missing:\$c >&2; exit 1; }; done"
remote "systemctl is-active --quiet nginx"
remote "systemctl is-active --quiet mosquitto"
remote "nginx -t" 2>&1 | tee -a "${REPORT}"

WB_ARCH="$(remote "uname -m" | tr -d '\r')"
WB_KERNEL="$(remote "uname -r" | tr -d '\r')"
WB_OS="$(remote ". /etc/os-release; printf '%s' \"\${PRETTY_NAME:-unknown}\"" | tr -d '\r')"
record "wb_arch: ${WB_ARCH}"
record "wb_kernel: ${WB_KERNEL}"
record "wb_os: ${WB_OS}"

if remote "test -f /etc/wb-release"; then
    WB_RELEASE="$(remote "tr '\n' ' ' < /etc/wb-release" | tr -d '\r')"
    record "wb_release: ${WB_RELEASE}"
fi

if ! remote "grep -Eq '^[[:space:]]*root[[:space:]]+/var/www;' /usr/share/wb-mqtt-homeui/nginx/default.conf"; then
    echo "Штатный nginx config не подтверждает root /var/www." >&2
    exit 1
fi
if ! remote "grep -Eq '^[[:space:]]*location[[:space:]]+/mqtt[[:space:]]*\\{' /usr/share/wb-mqtt-homeui/nginx/default.conf"; then
    echo "Штатный nginx config не содержит location /mqtt." >&2
    exit 1
fi
record "wb_nginx_var_www_root: PASS"
record "wb_nginx_mqtt_location: PASS"

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
    echo "На WB8 уже запущен реальный DMXWB/acceptance executable. Для B2 transport-test он должен быть остановлен." >&2
    printf '%s\n' "${RUNNING_DMXWB}" >&2
    exit 1
fi
record "no_running_dmxwb_process: PASS"

remote "rm -rf '${REMOTE_TMP}' && mkdir -p '${REMOTE_TMP}'"

echo "Сохраняем исходные retained web snapshots..."
remote "
set -eu
capture_one() {
    topic=\"\$1\"
    file=\"\$2\"
    present=\"\$3\"
    : > \"\${file}\"
    if timeout 2 mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t \"\${topic}\" > \"\${file}\" 2>/dev/null; then
        printf '1\n' > \"\${present}\"
    else
        : > \"\${file}\"
        printf '0\n' > \"\${present}\"
    fi
}
capture_one /dmxwb/config ${REMOTE_TMP}/original-config.json ${REMOTE_TMP}/original-config.present
capture_one /dmxwb/state  ${REMOTE_TMP}/original-state.json  ${REMOTE_TMP}/original-state.present
capture_one /dmxwb/status ${REMOTE_TMP}/original-status.json ${REMOTE_TMP}/original-status.present
"
ORIGINALS_CAPTURED=1
record "original_retained_snapshots_captured: PASS"

cat > "${LOCAL_TMP}/phase1-config.json" <<'JSON'
{"version":1,"revision":110201,"dmx":{"port":"/dev/ttyRS485-1"},"artnet":{"universe":0},"fixtures":{"count":2,"start_address":1,"items":[{"id":9101,"name":"Светильник 1"},{"id":9102,"name":"Светильник 2"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":9103,"next_group_id":1,"next_scene_id":1}}
JSON
cat > "${LOCAL_TMP}/phase1-state.json" <<'JSON'
{"version":1,"source":"mqtt","fixtures":[{"id":9101,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":9102,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON
cat > "${LOCAL_TMP}/phase1-status.json" <<'JSON'
{"application":"running","dmx":"44 Hz","mqtt":"connected","artnet":"idle","configuration":"loaded","last_error":""}
JSON

cat > "${LOCAL_TMP}/phase2-config.json" <<'JSON'
{"version":1,"revision":110202,"dmx":{"port":"/dev/ttyRS485-1"},"artnet":{"universe":0},"fixtures":{"count":3,"start_address":1,"items":[{"id":9101,"name":"Светильник 1"},{"id":9102,"name":"Светильник 2"},{"id":9103,"name":"Светильник 3"}]},"groups":[],"scenes":[],"id_counters":{"next_fixture_id":9104,"next_group_id":1,"next_scene_id":1}}
JSON
cat > "${LOCAL_TMP}/phase2-state.json" <<'JSON'
{"version":1,"source":"artnet","fixtures":[{"id":9101,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":9102,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100},{"id":9103,"requested_power":false,"red":255,"green":255,"blue":255,"white":255,"brightness":100,"temperature":100}]}
JSON
cat > "${LOCAL_TMP}/phase2-status.json" <<'JSON'
{"application":"running","dmx":"44 Hz","mqtt":"connected","artnet":"active","configuration":"loaded","last_error":""}
JSON

for file in "${LOCAL_TMP}"/phase*.json; do
    scp "${SSH_OPTS[@]}" -q "${file}" "${SSH_TARGET}:${REMOTE_TMP}/$(basename "${file}")"
done
record "acceptance_snapshots_staged: PASS"

echo "Разворачиваем текущий static web в /var/www/dmxwb..."
tar -C "${REPO_ROOT}/www" -czf - dmxwb | remote "rm -rf /var/www/dmxwb && tar -C /var/www -xzf -"
remote "find /var/www/dmxwb -type f -exec chmod 0644 {} +"
remote "find /var/www/dmxwb -type d -exec chmod 0755 {} +"

if ! remote "curl -fsS http://127.0.0.1/dmxwb/ | grep -q '<title>DMXWB</title>'"; then
    echo "nginx не отдаёт /dmxwb/ из /var/www/dmxwb." >&2
    exit 1
fi
record "wb_static_web_http_load: PASS"

publish_phase phase1
record "phase1_retained_published: PASS (revision 110201, source mqtt, fixtures 2)"

echo
echo "=== Browser phase 1 ==="
echo "1. Откройте штатный Web UI контроллера на ${WEB_HOST} и войдите как Administrator."
echo "2. Не выходя из этой browser session, откройте:"
echo "     http://${WEB_HOST}/dmxwb/"
echo "   Если контроллер переведёт на HTTPS — оставайтесь на HTTPS."
echo
echo "На странице DMXWB ожидается:"
echo "  Связь установлена"
echo "  WebSocket = ws(s)://${WEB_HOST}/mqtt"
echo "  Конфигурация = revision 110201"
echo "  Runtime state = получен"
echo "  Source: mqtt"
echo "  2 светильников · 0 групп"
if ask_yes_no "Все перечисленные значения видны в браузере?"; then
    record "phase1_browser_retained_snapshots_user: PASS"
else
    record "phase1_browser_retained_snapshots_user: FAIL"
    exit 1
fi

echo
echo "=== Real Mosquitto outage ==="
echo "Сейчас broker будет остановлен на несколько секунд. Страницу НЕ перезагружайте."
remote "systemctl stop mosquitto"
MOSQUITTO_STOPPED=1
sleep 2

if remote "systemctl is-active --quiet mosquitto"; then
    echo "Mosquitto неожиданно остался active после stop." >&2
    exit 1
fi
record "mosquitto_stopped: PASS"

if ask_yes_no "Без reload страница показала «Переподключение…» или «Нет связи»?"; then
    record "browser_disconnect_state_user: PASS"
else
    record "browser_disconnect_state_user: FAIL"
    exit 1
fi

sleep 3
remote "systemctl start mosquitto"
MOSQUITTO_STOPPED=0

for _ in $(seq 1 30); do
    if remote "systemctl is-active --quiet mosquitto"; then
        break
    fi
    sleep 0.5
done
if ! remote "systemctl is-active --quiet mosquitto"; then
    echo "Mosquitto не вернулся в active после start." >&2
    exit 1
fi
record "mosquitto_restarted: PASS"

publish_phase phase2
record "phase2_retained_published: PASS (revision 110202, source artnet, fixtures 3)"

echo
echo "Не перезагружайте страницу. После automatic reconnect ожидается:"
echo "  Связь установлена"
echo "  Конфигурация = revision 110202"
echo "  Runtime state = получен"
echo "  Source: artnet"
echo "  3 светильников · 0 групп"
echo "  Art-Net diagnostic = active"
if ask_yes_no "Новые retained значения появились сами, без reload страницы?"; then
    record "browser_reconnect_resubscribe_user: PASS"
else
    record "browser_reconnect_resubscribe_user: FAIL"
    exit 1
fi

echo
echo "Восстанавливаем исходные retained snapshots..."
restore_originals
record "original_retained_snapshots_restored: PASS"

remote "rm -rf '${REMOTE_TMP}'"
record "temporary_remote_acceptance_files_removed: PASS"

record "dev011b2_real_wb8_mqtt_websocket_result: PASS"
record "=== DMXWB DEV-011B2 REAL WB8 MQTT WEBSOCKET RECONNECT PASS ==="

echo
echo "=== DMXWB DEV-011B2 REAL WB8 MQTT WEBSOCKET RECONNECT PASS ==="
echo "Report: ${REPORT}"

ssh "${SSH_OPTS[@]}" -O exit "${SSH_TARGET}" >/dev/null 2>&1 || true
SSH_OPENED=0
rm -rf "${LOCAL_TMP}" "${CONTROL_DIR}"
CLEANED=1
trap - EXIT INT TERM
