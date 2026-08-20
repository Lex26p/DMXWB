#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev004b_fast_transport_acceptance.sh user@wb8-host [start-channel]

DEV-004B acceptance нового production DMX transport на WB8.
Проверяет обычный dmxwb, а не standalone probe.

Фиксированные проверки:
  - 512 slots / 30 Hz / 60 s — КРАСНЫЙ;
  - 512 slots / 30 Hz / 10 s — ЗЕЛЁНЫЙ;
  - 512 slots / 30 Hz / 10 s — СИНИЙ;
  - 512 slots / 30 Hz / 10 s — БЕЛЫЙ;
  - 240 slots / 44 Hz / 30 s — СИНИЙ (60 RGBW fixtures worst configured address);
  - 512 slots / requested 44 Hz — ожидаем корректный measured rejection без missed deadlines;
  - финальный all-off на 512 slots.

Ответы на визуальные вопросы: только y или n.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
START_CHANNEL="${2:-1}"
PORT="/dev/ttyRS485-1"
LONG_DURATION=60
SHORT_DURATION=10
REAL_LOAD_DURATION=30
REJECTION_DURATION=3

if [[ "${TARGET}" == *"<"* || "${TARGET}" == *">"* ]]; then
    echo "Укажите реальный адрес контроллера, например root@10.200.200.1" >&2
    exit 2
fi
if [[ ! "${START_CHANNEL}" =~ ^[0-9]+$ ]] || (( START_CHANNEL < 1 || START_CHANNEL > 509 )); then
    echo "start-channel должен быть целым числом 1..509" >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV004B_FAST_TRANSPORT_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev004b-fast"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev004b-fast-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

for command_name in ssh scp sha256sum grep awk git; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    fi
done

if [[ ! -x "${BINARY}" ]]; then
    echo "Не найден ARM64 binary: ${BINARY}" >&2
    echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
    exit 1
fi

for source_file in \
    "${REPO_ROOT}/src/dmx_transport_linux.cpp" \
    "${REPO_ROOT}/src/dmx_output.cpp" \
    "${REPO_ROOT}/src/main.cpp" \
    "${REPO_ROOT}/include/dmxwb/dmx_transport.hpp"; do
    if [[ "${source_file}" -nt "${BINARY}" ]]; then
        echo "ARM64 binary старее изменённых исходников: ${source_file}" >&2
        echo "Сначала выполните: bash tools/wb8/build_bullseye_arm64.sh" >&2
        exit 1
    fi
done

mkdir -p "${CONTROL_DIR}"
chmod 0700 "${CONTROL_DIR}"

SSH_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=300"
    -o "StrictHostKeyChecking=accept-new"
)
SCP_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=300"
    -o "StrictHostKeyChecking=accept-new"
)

SSH_OPENED=0
SERVICE_WAS_ACTIVE=0
SERVICE_STOPPED=0
SERVICE_RESTORED=0
PORT_DECLARED_FREE=0
REMOTE_READY=0
TEST_SUCCEEDED=0

remote() {
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$@"
}

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

restore_service() {
    if (( SERVICE_WAS_ACTIVE == 1 && SERVICE_STOPPED == 1 && SERVICE_RESTORED == 0 )); then
        echo "Восстанавливаем wb-mqtt-serial..."
        if remote "systemctl start wb-mqtt-serial"; then
            SERVICE_RESTORED=1
            echo "wb-mqtt-serial восстановлен."
        else
            echo "ВНИМАНИЕ: не удалось запустить wb-mqtt-serial обратно." >&2
            return 1
        fi
    fi
    return 0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM

    if (( SSH_OPENED == 1 )); then
        if (( TEST_SUCCEEDED == 0 && REMOTE_READY == 1 )); then
            echo
            echo "Best-effort: выключаем свет после прерванного теста..."
            remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 512 --frames 8" >/dev/null 2>&1 || true
        fi
        restore_service || status=1
        remote "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
        ssh "${SSH_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi

    rmdir "${CONTROL_DIR}" >/dev/null 2>&1 || true
    exit "${status}"
}
trap cleanup EXIT INT TERM

diag_value() {
    local output="$1"
    local key="$2"
    awk -F': ' -v key="${key}" '$1 ~ "^[[:space:]]*" key "$" {print $2}' <<<"${output}" | tail -n1 | tr -d '\r'
}

validate_numeric_diag() {
    local value="$1"
    [[ "${value}" =~ ^[0-9]+$ ]]
}

run_visual_case() {
    local label="$1"
    local color_ru="$2"
    local pattern="$3"
    local slots="$4"
    local refresh="$5"
    local duration="$6"
    local expected_frames="$7"
    local output rc
    local frames open_failures send_failures missed refresh_rejections active_refresh max_send_us
    local period_us

    echo
    echo "============================================================"
    echo "${label}"
    echo "============================================================"
    echo "Ожидаемый цвет: ${color_ru}"
    echo "Полный кадр: ${slots} slots"
    echo "Частота: ${refresh} Hz"
    echo "Длительность: ${duration} секунд"
    echo "Ожидается примерно: ${expected_frames} кадров"
    echo "Если цвет неправильный, света нет или есть заметное мерцание — ответьте n."
    read -r -p "Нажмите Enter, когда готовы смотреть на светильник... " _
    echo ">>> СМОТРИМ ${color_ru}: ${duration} СЕКУНД <<<"

    record ""
    record "--- ${label} ---"
    record "pattern: ${pattern}"
    record "slots: ${slots}"
    record "refresh_hz: ${refresh}"
    record "duration_seconds: ${duration}"

    set +e
    output="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-continuous-test '${pattern}' --port '${PORT}' --start-channel '${START_CHANNEL}' --slots '${slots}' --refresh '${refresh}' --seconds '${duration}'" 2>&1)"
    rc=$?
    set -e
    printf '%s\n' "${output}" | tee -a "${REPORT}"

    if (( rc != 0 )); then
        record "software_result: FAIL (exit=${rc})"
        return 1
    fi
    if ! grep -Fq "DEV-004 continuous DMX diagnostic completed; serial port closed cleanly." <<<"${output}"; then
        record "software_result: FAIL (clean close marker missing)"
        return 1
    fi

    frames="$(diag_value "${output}" frames_sent)"
    open_failures="$(diag_value "${output}" open_failures)"
    send_failures="$(diag_value "${output}" send_failures)"
    missed="$(diag_value "${output}" missed_deadlines)"
    refresh_rejections="$(diag_value "${output}" refresh_rejections)"
    active_refresh="$(diag_value "${output}" active_refresh_hz)"
    max_send_us="$(diag_value "${output}" max_send_us)"

    for value in "${frames}" "${open_failures}" "${send_failures}" "${missed}" "${refresh_rejections}" "${active_refresh}" "${max_send_us}"; do
        if ! validate_numeric_diag "${value}"; then
            record "software_result: FAIL (diagnostic parse error)"
            return 1
        fi
    done

    period_us=$(( 1000000 / refresh ))
    if (( frames < expected_frames * 95 / 100 )); then
        record "software_result: FAIL (frames_sent=${frames}, expected~${expected_frames})"
        return 1
    fi
    if (( open_failures != 0 || send_failures != 0 || missed != 0 || refresh_rejections != 0 )); then
        record "software_result: FAIL (open=${open_failures} send=${send_failures} missed=${missed} rejected=${refresh_rejections})"
        return 1
    fi
    if (( active_refresh != refresh )); then
        record "software_result: FAIL (active_refresh_hz=${active_refresh})"
        return 1
    fi
    if (( max_send_us >= period_us )); then
        record "software_result: FAIL (max_send_us=${max_send_us} >= period_us=${period_us})"
        return 1
    fi

    record "software_result: PASS"
    if ask_yes_no "Все ${duration} секунд светильник был ${color_ru}, без заметного мерцания?"; then
        record "user_observation: PASS"
    else
        record "user_observation: FAIL"
        return 1
    fi
}

run_expected_512_44_rejection() {
    local output rc
    local missed rejected active_refresh send_failures open_failures

    echo
    echo "============================================================"
    echo "АВТОПРОВЕРКА — 512 slots / запрос 44 Hz"
    echo "============================================================"
    echo "Это НЕ визуальный тест. Отправляется all-off."
    echo "Для полного 512-slot кадра текущий WB8 должен после первого измерения"
    echo "корректно отклонить 44 Hz и остаться на безопасных 30 Hz без missed deadlines."

    record ""
    record "--- expected measured rejection: 512 slots / requested 44 Hz ---"

    set +e
    output="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-continuous-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 512 --refresh 44 --seconds '${REJECTION_DURATION}'" 2>&1)"
    rc=$?
    set -e
    printf '%s\n' "${output}" | tee -a "${REPORT}"

    missed="$(diag_value "${output}" missed_deadlines)"
    rejected="$(diag_value "${output}" refresh_rejections)"
    active_refresh="$(diag_value "${output}" active_refresh_hz)"
    send_failures="$(diag_value "${output}" send_failures)"
    open_failures="$(diag_value "${output}" open_failures)"

    for value in "${missed}" "${rejected}" "${active_refresh}" "${send_failures}" "${open_failures}"; do
        if ! validate_numeric_diag "${value}"; then
            record "expected_rejection_result: FAIL (diagnostic parse error)"
            return 1
        fi
    done

    if (( rc == 0 )); then
        record "expected_rejection_result: FAIL (44 Hz unexpectedly accepted for measured 512-slot transport)"
        return 1
    fi
    if (( missed != 0 || send_failures != 0 || open_failures != 0 )); then
        record "expected_rejection_result: FAIL (open=${open_failures} send=${send_failures} missed=${missed})"
        return 1
    fi
    if (( rejected < 1 || active_refresh != 30 )); then
        record "expected_rejection_result: FAIL (refresh_rejections=${rejected}, active_refresh_hz=${active_refresh})"
        return 1
    fi

    record "expected_rejection_result: PASS"
    record "expected_behavior: requested 44 Hz rejected after measurement; active refresh remains 30 Hz"
}

echo "DMXWB DEV-004B — production fast transport acceptance"
echo "Контроллер: ${TARGET}"
echo "Порт: ${PORT}"
echo "RGBW start channel: ${START_CHANNEL}"
echo
echo "Сначала проверим production dmxwb на полном 512-slot кадре,"
echo "затем 240 slots / 44 Hz как нагрузку минимум 60 RGBW светильников."
echo

if ! ask_yes_no "Светильник подключён безопасно и готов к тесту?"; then
    echo "Тест отменён до подключения к контроллеру."
    exit 2
fi

echo
echo "Открываем одно SSH-соединение с ${TARGET}."
echo "Пароль контроллера потребуется ввести один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_DIR}/dmxwb"
remote "chmod 0755 '${REMOTE_DIR}/dmxwb'"
REMOTE_READY=1

SERVICE_STATE="$(remote "systemctl is-active wb-mqtt-serial 2>/dev/null || true" | tr -d '\r')"
if [[ "${SERVICE_STATE}" == "active" ]]; then
    SERVICE_WAS_ACTIVE=1
    echo
    echo "Сервис wb-mqtt-serial активен. Нужно освободить ${PORT}:"
    echo "  s — временно остановить весь wb-mqtt-serial;"
    echo "  p — ${PORT} уже отключён в Serial Device Driver Configuration;"
    echo "  q — отменить тест."
    read -r -p "Выбор [s/p/q]: " FREE_ANSWER
    case "${FREE_ANSWER}" in
        s|S)
            remote "systemctl stop wb-mqtt-serial"
            SERVICE_STOPPED=1
            ;;
        p|P)
            PORT_DECLARED_FREE=1
            ;;
        *)
            echo "Тест отменён: выбранный RS-485 порт должен быть свободен." >&2
            exit 2
            ;;
    esac
else
    PORT_DECLARED_FREE=1
fi

if (( PORT_DECLARED_FREE == 0 )) && [[ "$(remote "systemctl is-active wb-mqtt-serial 2>/dev/null || true" | tr -d '\r')" == "active" ]]; then
    echo "wb-mqtt-serial всё ещё активен; direct DMX test остановлен." >&2
    exit 1
fi

mkdir -p "$(dirname -- "${REPORT}")"
: > "${REPORT}"
record "=== DMXWB DEV-004B production fast transport report ==="
record "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
record "source_head: $(git -C "${REPO_ROOT}" rev-parse HEAD)"
if [[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]]; then
    record "source_worktree: modified"
else
    record "source_worktree: clean"
fi
record "artifact_sha256: $(sha256sum "${BINARY}" | awk '{print $1}')"
record "target: ${TARGET}"
record "port: ${PORT}"
record "start_channel: ${START_CHANNEL}"
record "wb_mqtt_serial_initial_state: ${SERVICE_STATE:-unknown}"
record "port_declared_free_in_driver_config: ${PORT_DECLARED_FREE}"
record ""
record "--- target identity ---"
remote "printf 'model: '; tr -d '\\0' </proc/device-tree/model 2>/dev/null || true; echo; uname -a; if [ -r /etc/wb-release ]; then cat /etc/wb-release; fi" | tee -a "${REPORT}"

run_visual_case "ТЕСТ 1/5 — 512 slots / default 30 Hz" "КРАСНЫЙ" red 512 30 "${LONG_DURATION}" $((30 * LONG_DURATION))
run_visual_case "ТЕСТ 2/5 — 512 slots / 30 Hz" "ЗЕЛЁНЫЙ" green 512 30 "${SHORT_DURATION}" $((30 * SHORT_DURATION))
run_visual_case "ТЕСТ 3/5 — 512 slots / 30 Hz" "СИНИЙ" blue 512 30 "${SHORT_DURATION}" $((30 * SHORT_DURATION))
run_visual_case "ТЕСТ 4/5 — 512 slots / 30 Hz" "БЕЛЫЙ" white 512 30 "${SHORT_DURATION}" $((30 * SHORT_DURATION))
run_visual_case "ТЕСТ 5/5 — 240 slots / 44 Hz / 60 RGBW" "СИНИЙ" blue 240 44 "${REAL_LOAD_DURATION}" $((44 * REAL_LOAD_DURATION))

run_expected_512_44_rejection

record ""
record "--- final all-off / reopen check / 512 slots ---"
FINAL_OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 512 --frames 20" 2>&1)"
printf '%s\n' "${FINAL_OUTPUT}" | tee -a "${REPORT}"
if ! grep -Fq "DMX test completed; serial port closed cleanly." <<<"${FINAL_OUTPUT}"; then
    record "final_all_off_reopen_check: FAIL"
    record "=== DEV-004B PRODUCTION FAST TRANSPORT FAIL ==="
    exit 1
fi
record "final_all_off_reopen_check: PASS"
record "serial_reopen_across_separate_runs: PASS"

if (( SERVICE_WAS_ACTIVE == 1 )); then
    restore_service
    record "wb_mqtt_serial_restore: PASS"
else
    record "wb_mqtt_serial_restore: not-required"
fi

record "kernel_patch_required: NO"
record "legacy_transport_fallback_retained: YES"
record "overall_result: PASS"
record "=== DEV-004B PRODUCTION FAST TRANSPORT PASS ==="
TEST_SUCCEEDED=1

echo
echo "Отчёт сохранён: ${REPORT}"
echo "PASS: production dmxwb подтверждён на 512 slots / 30 Hz и 240 slots / 44 Hz."
echo "Git пока не выполняйте; пришлите полный вывод."
