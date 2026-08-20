#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ROOTFS="${DMXWB_WB8_ROOTFS:-/opt/dmxwb/wb8-bullseye-cross-arm64}"
MARKER="${ROOTFS}/.dmxwb-bullseye-cross-arm64-ready"
SOURCE="${SCRIPT_DIR}/dev004_manual_de_visual_probe.cpp"
BUILD_DIR="${ROOTFS}/work/dmxwb-manual-de-visual-fixed"
OUTPUT_DIR="${REPO_ROOT}/artifacts/wb8-bullseye-arm64"
OUTPUT_BIN="${OUTPUT_DIR}/wb8_manual_de_visual_probe"
DMXWB_BIN="${OUTPUT_DIR}/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV004_MANUAL_DE_VISUAL_512_REPORT.txt"
TARGET="${1:-}"
PORT="${DMXWB_MANUAL_DE_PORT:-/dev/ttyRS485-1}"

# Acceptance parameters are intentionally fixed. Do not use Bash's special
# SECONDS variable here: it is an automatically increasing shell timer.
readonly REFRESH_HZ=30
readonly DURATION_SECONDS=10
readonly SLOT_COUNT=512
readonly EXPECTED_FRAMES=300

REMOTE_DIR="/tmp/dmxwb-manual-de-visual-512-fixed"
CONTROL_PATH="/tmp/dmxwb-manual-de-visual-fixed-%C"
SERVICE_STOPPED=0
MASTER_OPEN=0

if [[ -z "${TARGET}" ]]; then
    echo "Использование:" >&2
    echo "  bash tools/wb8/run_dev004_manual_de_visual_512.sh root@10.200.200.1" >&2
    exit 2
fi

if [[ $((REFRESH_HZ * DURATION_SECONDS)) -ne ${EXPECTED_FRAMES} ]]; then
    echo "Внутренняя ошибка test helper: 30 Hz * 10 s должно быть 300 кадров." >&2
    exit 1
fi

if [[ ! -f "${MARKER}" ]]; then
    echo "Не найден подготовленный Bullseye ARM64 rootfs: ${ROOTFS}" >&2
    exit 1
fi
if [[ ! -f "${SOURCE}" ]]; then
    echo "Не найден исходник probe: ${SOURCE}" >&2
    exit 1
fi
for command_name in sudo chroot file readelf sha256sum ssh scp git awk grep; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Не найдена команда: ${command_name}" >&2
        exit 1
    fi
done

cleanup() {
    set +e
    if [[ ${SERVICE_STOPPED} -eq 1 && ${MASTER_OPEN} -eq 1 ]]; then
        ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" 'systemctl start wb-mqtt-serial' >/dev/null 2>&1 || true
    fi
    if [[ ${MASTER_OPEN} -eq 1 ]]; then
        ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" "rm -rf '${REMOTE_DIR}'" >/dev/null 2>&1 || true
        ssh -o ControlPath="${CONTROL_PATH}" -O exit "${TARGET}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

ask_yes_no() {
    local prompt="$1"
    local answer
    while true; do
        read -r -p "${prompt} (y/n): " answer
        case "${answer}" in
            y|Y) return 0 ;;
            n|N) return 1 ;;
            *) echo "Введите только латинскую букву y или n." ;;
        esac
    done
}

printf '%s\n' \
    "DMXWB DEV-004 — визуальная проверка быстрого DMX transport" \
    "Контроллер: ${TARGET}" \
    "Порт: ${PORT}" \
    "Размер каждого кадра: ${SLOT_COUNT} DMX slots" \
    "Частота: ${REFRESH_HZ} Hz" \
    "Длительность каждого цвета: ${DURATION_SECONDS} секунд" \
    "Кадров каждого цвета: ${EXPECTED_FRAMES}" \
    "" \
    "Проверяем только перспективный режим:" \
    "  - kernel RS-485 временно OFF;" \
    "  - DE вручную через RTS;" \
    "  - аппаратный BREAK через TIOCSBRK/TIOCCBRK;" \
    "  - полный DMX кадр 512 slots." \
    "" \
    "Будут четыре независимых теста: КРАСНЫЙ, ЗЕЛЁНЫЙ, СИНИЙ, БЕЛЫЙ." \
    "Каждый тест длится ровно 10 секунд и отправляет 300 кадров." \
    "После каждого теста ответьте только y или n."

echo
if ! ask_yes_no "Светильник подключён безопасно и готов к тесту?"; then
    echo "Тест отменён."
    exit 2
fi

echo
echo "=== Сборка standalone ARM64 probe ==="
sudo rm -rf "${BUILD_DIR}"
sudo mkdir -p "${BUILD_DIR}"
sudo cp "${SOURCE}" "${BUILD_DIR}/dev004_manual_de_visual_probe.cpp"
sudo chroot "${ROOTFS}" /bin/bash -lc '
    set -euo pipefail
    cd /work/dmxwb-manual-de-visual-fixed
    aarch64-linux-gnu-g++ \
        -std=c++20 -O2 \
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
        -static-libstdc++ -static-libgcc \
        dev004_manual_de_visual_probe.cpp -o wb8_manual_de_visual_probe
'
mkdir -p "${OUTPUT_DIR}"
sudo cp "${BUILD_DIR}/wb8_manual_de_visual_probe" "${OUTPUT_BIN}"
sudo chown "$(id -u):$(id -g)" "${OUTPUT_BIN}"
chmod 0755 "${OUTPUT_BIN}"
file "${OUTPUT_BIN}"
if ! readelf -h "${OUTPUT_BIN}" | grep -q 'Machine:.*AArch64'; then
    echo "Ошибка: probe не AArch64." >&2
    exit 1
fi
if readelf -d "${OUTPUT_BIN}" | grep -Eq 'libstdc\+\+|libgcc_s'; then
    echo "Ошибка: probe неожиданно зависит от динамического GNU C++ runtime." >&2
    exit 1
fi

PROBE_SHA="$(sha256sum "${OUTPUT_BIN}" | awk '{print $1}')"
SOURCE_HEAD="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
if [[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]]; then
    SOURCE_WORKTREE=modified
else
    SOURCE_WORKTREE=clean
fi

echo
echo "Открываем одно SSH-соединение с ${TARGET}."
echo "Пароль контроллера потребуется ввести один раз."
ssh -MNf -o ControlMaster=yes -o ControlPersist=120 -o ControlPath="${CONTROL_PATH}" "${TARGET}"
MASTER_OPEN=1
ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp -o ControlPath="${CONTROL_PATH}" "${OUTPUT_BIN}" "${TARGET}:${REMOTE_DIR}/wb8_manual_de_visual_probe"
if [[ -f "${DMXWB_BIN}" ]]; then
    scp -o ControlPath="${CONTROL_PATH}" "${DMXWB_BIN}" "${TARGET}:${REMOTE_DIR}/dmxwb" >/dev/null
fi
ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" \
    "chmod 0755 '${REMOTE_DIR}/wb8_manual_de_visual_probe'; [[ ! -f '${REMOTE_DIR}/dmxwb' ]] || chmod 0755 '${REMOTE_DIR}/dmxwb'"

INITIAL_STATE="$(ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" 'systemctl is-active wb-mqtt-serial 2>/dev/null || true')"
PORT_DECLARED_FREE=0
if [[ "${INITIAL_STATE}" == "active" ]]; then
    echo
    echo "Сервис wb-mqtt-serial активен. Нужно освободить ${PORT}:"
    echo "  s — временно остановить весь wb-mqtt-serial;"
    echo "  p — ${PORT} уже отключён в Serial Device Driver Configuration;"
    echo "  q — отменить тест."
    while true; do
        read -r -p "Выбор [s/p/q]: " CHOICE
        case "${CHOICE}" in
            s|S)
                ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" 'systemctl stop wb-mqtt-serial'
                SERVICE_STOPPED=1
                break
                ;;
            p|P)
                PORT_DECLARED_FREE=1
                break
                ;;
            q|Q)
                echo "Тест отменён."
                exit 2
                ;;
            *) echo "Введите только s, p или q." ;;
        esac
    done
fi

mkdir -p "$(dirname "${REPORT}")"
{
    echo "=== DMXWB DEV-004 manual-DE visual 512-slot report ==="
    echo "generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_head: ${SOURCE_HEAD}"
    echo "source_worktree: ${SOURCE_WORKTREE}"
    echo "probe_sha256: ${PROBE_SHA}"
    echo "target: ${TARGET}"
    echo "port: ${PORT}"
    echo "slots: ${SLOT_COUNT}"
    echo "refresh_hz: ${REFRESH_HZ}"
    echo "seconds_per_color: ${DURATION_SECONDS}"
    echo "expected_frames_per_color: ${EXPECTED_FRAMES}"
    echo "wb_mqtt_serial_initial_state: ${INITIAL_STATE:-unknown}"
    echo "port_declared_free_in_driver_config: ${PORT_DECLARED_FREE}"
} > "${REPORT}"

run_visual_case() {
    local number="$1"
    local active_channel="$2"
    local color_ru="$3"
    local color_key="$4"
    local case_output
    local status
    local frames_sent
    local requested_seconds
    local target_frames
    local missed_deadlines
    local restore_status

    echo
    echo "============================================================"
    echo "ТЕСТ ${number}/4 — ${color_ru}"
    echo "============================================================"
    echo "Ожидаемый цвет: ${color_ru}"
    echo "Время: ${DURATION_SECONDS} секунд"
    echo "Частота: ${REFRESH_HZ} Hz"
    echo "Полный кадр: ${SLOT_COUNT} slots"
    echo "Ожидается: ${EXPECTED_FRAMES} кадров"
    echo "Если цвет другой, света нет или есть заметное мерцание — отвечайте n."
    read -r -p "Нажмите Enter, когда готовы смотреть на светильник... " _
    echo "Старт через 3..."
    sleep 1
    echo "2..."
    sleep 1
    echo "1..."
    sleep 1
    echo ">>> СМОТРИМ ${color_ru}: ${DURATION_SECONDS} СЕКУНД <<<"

    echo >> "${REPORT}"
    echo "--- visual case ${number}: ${color_key} ---" | tee -a "${REPORT}"

    set +e
    case_output="$(ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" \
        "'${REMOTE_DIR}/wb8_manual_de_visual_probe' --port '${PORT}' --active-channel '${active_channel}' --refresh '${REFRESH_HZ}' --seconds '${DURATION_SECONDS}'" 2>&1)"
    status=$?
    set -e
    printf '%s\n' "${case_output}" | tee -a "${REPORT}"
    echo "probe_exit_code: ${status}" | tee -a "${REPORT}"

    if [[ ${status} -ne 0 ]]; then
        echo "ПРОГРАММНЫЙ FAIL: probe завершился с кодом ${status}. Вопрос про цвет не задаётся."
        echo "visual_${color_key}_observation: NOT_ASKED_SOFTWARE_FAIL" | tee -a "${REPORT}"
        return 1
    fi

    requested_seconds="$(printf '%s\n' "${case_output}" | awk -F': ' '/^  requested_seconds:/ {print $2; exit}')"
    target_frames="$(printf '%s\n' "${case_output}" | awk -F': ' '/^  target_frames:/ {print $2; exit}')"
    frames_sent="$(printf '%s\n' "${case_output}" | awk -F= '/^  frames_sent=/ {print $2; exit}')"
    missed_deadlines="$(printf '%s\n' "${case_output}" | awk -F= '/^  missed_deadlines=/ {print $2; exit}')"
    restore_status="$(printf '%s\n' "${case_output}" | awk -F= '/^  rs485_restore=/ {print $2; exit}')"

    if [[ "${requested_seconds}" != "${DURATION_SECONDS}" || \
          "${target_frames}" != "${EXPECTED_FRAMES}" || \
          "${frames_sent}" != "${EXPECTED_FRAMES}" || \
          "${missed_deadlines}" != "0" || \
          "${restore_status}" != "PASS" ]]; then
        echo "ПРОГРАММНЫЙ FAIL: параметры/счётчики не соответствуют acceptance." | tee -a "${REPORT}"
        echo "expected_seconds=${DURATION_SECONDS} observed_seconds=${requested_seconds:-MISSING}" | tee -a "${REPORT}"
        echo "expected_frames=${EXPECTED_FRAMES} target_frames=${target_frames:-MISSING} frames_sent=${frames_sent:-MISSING}" | tee -a "${REPORT}"
        echo "missed_deadlines=${missed_deadlines:-MISSING} rs485_restore=${restore_status:-MISSING}" | tee -a "${REPORT}"
        echo "visual_${color_key}_observation: NOT_ASKED_SOFTWARE_FAIL" | tee -a "${REPORT}"
        return 1
    fi

    echo "Программная часть PASS: ${EXPECTED_FRAMES}/${EXPECTED_FRAMES} кадров, missed_deadlines=0, RS-485 восстановлен."
    if ask_yes_no "Все ${DURATION_SECONDS} секунд светильник был ${color_ru}, без заметного мерцания?"; then
        echo "Наблюдение ${color_ru}: PASS"
        echo "visual_${color_key}_observation: PASS" | tee -a "${REPORT}"
        return 0
    fi

    echo "Наблюдение ${color_ru}: FAIL"
    echo "visual_${color_key}_observation: FAIL" | tee -a "${REPORT}"
    return 1
}

OVERALL=PASS
run_visual_case 1 1 "КРАСНЫЙ" red || OVERALL=FAIL
run_visual_case 2 2 "ЗЕЛЁНЫЙ" green || OVERALL=FAIL
run_visual_case 3 3 "СИНИЙ" blue || OVERALL=FAIL
run_visual_case 4 4 "БЕЛЫЙ" white || OVERALL=FAIL

echo
if [[ -f "${DMXWB_BIN}" ]]; then
    echo "Финальная безопасная команда: выключаем свет проверенным старым transport (all-off)."
    set +e
    ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" \
        "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel 1 --frames 3" \
        >> "${REPORT}" 2>&1
    ALL_OFF_STATUS=$?
    set -e
    echo "final_all_off_exit_code: ${ALL_OFF_STATUS}" | tee -a "${REPORT}"
    if [[ ${ALL_OFF_STATUS} -ne 0 ]]; then
        OVERALL=FAIL
    fi
else
    echo "final_all_off_exit_code: NOT_RUN_NO_DMXWB_ARTIFACT" | tee -a "${REPORT}"
    OVERALL=FAIL
fi

if [[ ${SERVICE_STOPPED} -eq 1 ]]; then
    if ssh -o ControlPath="${CONTROL_PATH}" "${TARGET}" 'systemctl start wb-mqtt-serial'; then
        SERVICE_STOPPED=0
        echo "wb_mqtt_serial_restore: PASS" | tee -a "${REPORT}"
    else
        echo "wb_mqtt_serial_restore: FAIL" | tee -a "${REPORT}"
        OVERALL=FAIL
    fi
else
    echo "wb_mqtt_serial_restore: PASS" | tee -a "${REPORT}"
fi

echo "overall_visual_result: ${OVERALL}" | tee -a "${REPORT}"
echo "=== DEV-004 MANUAL-DE VISUAL 512-SLOT TEST ${OVERALL} ===" | tee -a "${REPORT}"
echo
echo "Отчёт сохранён: ${REPORT}"
if [[ "${OVERALL}" == "PASS" ]]; then
    echo "PASS: все 4 цвета подтверждены на 512 slots / 30 Hz / 10 s / 300 frames."
else
    echo "FAIL: пришлите полный вывод. Git пока не выполняйте."
    exit 1
fi
