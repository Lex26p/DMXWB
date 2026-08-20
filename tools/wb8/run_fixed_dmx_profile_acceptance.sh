#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_fixed_dmx_profile_acceptance.sh user@wb8-host [start-channel]

Post-change acceptance фиксированного physical profile DMXWB:
  - maximum physical slots: 300;
  - fixed output cadence: 44 Hz;
  - duration: 60 seconds;
  - expected: about 2640 frames;
  - visual pattern: BLUE;
  - final all-off/reopen check.

Для текущего стенда /dev/ttyRS485-1 считается постоянно отключённым
в WB Serial Device Driver Configuration. wb-mqtt-serial не останавливается.
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
SLOTS=300
REFRESH=44
DURATION=60
EXPECTED_FRAMES=$((REFRESH * DURATION))
PERIOD_US=22727

if [[ ! "${START_CHANNEL}" =~ ^[0-9]+$ ]] || (( START_CHANNEL < 1 || START_CHANNEL > 297 )); then
    echo "start-channel должен быть целым числом 1..297 для RGBW fixture внутри physical limit 300." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DMX_FIXED_PROFILE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-fixed-profile"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-fixed-profile-ssh-${USER:-user}"
CONTROL_PATH="${CONTROL_DIR}/control-%C"

for command_name in ssh scp sha256sum awk grep git; do
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
    "${REPO_ROOT}/include/dmxwb/dmx_snapshot.hpp" \
    "${REPO_ROOT}/include/dmxwb/dmx_output.hpp" \
    "${REPO_ROOT}/src/dmx_snapshot.cpp" \
    "${REPO_ROOT}/src/dmx_output.cpp" \
    "${REPO_ROOT}/src/main.cpp"; do
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
REMOTE_READY=0
TEST_COMPLETE=0

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

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if (( SSH_OPENED == 1 )); then
        if (( REMOTE_READY == 1 && TEST_COMPLETE == 0 )); then
            echo
            echo "Best-effort: выключаем свет после прерванного теста..."
            remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 300 --frames 8" >/dev/null 2>&1 || true
        fi
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

is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

echo "DMXWB — acceptance фиксированного physical profile"
echo "Контроллер: ${TARGET}"
echo "Порт: ${PORT}"
echo "Physical limit: ${SLOTS} slots"
echo "Fixed refresh: ${REFRESH} Hz"
echo "Период: ~22.727 ms"
echo "Длительность: ${DURATION} секунд"
echo "Ожидается около ${EXPECTED_FRAMES} кадров"
echo
echo "На этом стенде ${PORT} считается уже отключённым в WB Serial Device Driver Configuration."
echo "wb-mqtt-serial останавливать и выбирать s/p/q не нужно."
echo

if ! ask_yes_no "Светильник подключён безопасно и готов к минутному тесту?"; then
    echo "Тест отменён."
    exit 2
fi

: > "${REPORT}"
record "=== DMXWB fixed physical profile acceptance ==="
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
record "port_release_mode: permanently_disabled_in_wb_serial_driver_config"
record "physical_slot_limit: ${SLOTS}"
record "fixed_refresh_hz: ${REFRESH}"
record "duration_seconds: ${DURATION}"
record "expected_frames: ${EXPECTED_FRAMES}"

echo
echo "Открываем одно SSH-соединение с ${TARGET}."
echo "Пароль контроллера потребуется ввести один раз."
ssh "${SSH_OPTS[@]}" -Nf "${TARGET}"
SSH_OPENED=1

remote "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}'"
scp "${SCP_OPTS[@]}" "${BINARY}" "${TARGET}:${REMOTE_DIR}/dmxwb"
remote "chmod 0755 '${REMOTE_DIR}/dmxwb'"
REMOTE_READY=1

record ""
record "--- target identity ---"
remote "printf 'model: '; tr -d '\0' </proc/device-tree/model 2>/dev/null || true; echo; uname -a; if [ -r /etc/wb-release ]; then cat /etc/wb-release; fi" | tee -a "${REPORT}"
record "wb_mqtt_serial_state: $(remote "systemctl is-active wb-mqtt-serial 2>/dev/null || true" | tr -d '\r')"

if ! remote "'${REMOTE_DIR}/dmxwb' --help" | grep -Fq "production DMX profile is fixed at 44 Hz"; then
    record "binary_profile_check: FAIL"
    echo "На контроллер попал binary без нового fixed-profile CLI marker." >&2
    exit 1
fi
record "binary_profile_check: PASS"

echo
echo "============================================================"
echo "МИНУТНЫЙ TEST — 300 slots / fixed 44 Hz"
echo "============================================================"
echo "Ожидаемый цвет: СИНИЙ"
echo "Длительность: 60 секунд"
echo "Ожидается около 2640 кадров"
echo "PASS: missed=0, failures=0, active=44, max_send_us<22727, без мерцания."
read -r -p "Нажмите Enter, когда готовы смотреть на светильник... " _
echo ">>> СМОТРИМ СИНИЙ: 60 СЕКУНД <<<"

record ""
record "--- production fixed-profile continuous test ---"
set +e
OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-continuous-test blue --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 300 --refresh 44 --seconds 60" 2>&1)"
RC=$?
set -e
printf '%s\n' "${OUTPUT}" | tee -a "${REPORT}"

frames="$(diag_value "${OUTPUT}" frames_sent)"
open_failures="$(diag_value "${OUTPUT}" open_failures)"
send_failures="$(diag_value "${OUTPUT}" send_failures)"
recoveries="$(diag_value "${OUTPUT}" recoveries)"
missed="$(diag_value "${OUTPUT}" missed_deadlines)"
active="$(diag_value "${OUTPUT}" active_refresh_hz)"
max_send="$(diag_value "${OUTPUT}" max_send_us)"

software_pass=0
if is_uint "${frames}" && is_uint "${open_failures}" && is_uint "${send_failures}" && \
   is_uint "${recoveries}" && is_uint "${missed}" && is_uint "${active}" && is_uint "${max_send}"; then
    if (( RC == 0 && frames >= 2638 && open_failures == 0 && send_failures == 0 && \
          recoveries == 0 && missed == 0 && active == 44 && max_send < PERIOD_US )); then
        software_pass=1
    fi
fi

if (( software_pass == 1 )); then
    record "software_result: PASS"
else
    record "software_result: FAIL"
fi
record "software_acceptance_expected: frames>=2638, active=44, missed=0, open/send/recoveries=0, max_send_us<22727"

visual_pass=0
if ask_yes_no "Все 60 секунд светильник был СИНИЙ без заметного мерцания?"; then
    visual_pass=1
    record "user_observation: PASS"
else
    record "user_observation: FAIL"
fi

record ""
record "--- final all-off / reopen check / 300 slots ---"
set +e
FINAL_OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --dmx-test all-off --port '${PORT}' --start-channel '${START_CHANNEL}' --slots 300 --frames 20" 2>&1)"
FINAL_RC=$?
set -e
printf '%s\n' "${FINAL_OUTPUT}" | tee -a "${REPORT}"

final_pass=0
if (( FINAL_RC == 0 )) && grep -Fq "DMX test completed; serial port closed cleanly." <<<"${FINAL_OUTPUT}"; then
    final_pass=1
    record "final_all_off_reopen_check: PASS"
else
    record "final_all_off_reopen_check: FAIL"
fi

TEST_COMPLETE=1
if (( software_pass == 1 && visual_pass == 1 && final_pass == 1 )); then
    record "fixed_profile_result: PASS"
    record "=== DMXWB FIXED 300-SLOT / 44 HZ PROFILE PASS ==="
    echo
    echo "PASS: новый production core подтверждён на 300 slots / fixed 44 Hz."
    echo "Отчёт: ${REPORT}"
    exit 0
fi

record "fixed_profile_result: FAIL"
record "=== DMXWB FIXED 300-SLOT / 44 HZ PROFILE FAIL ==="
echo
echo "FAIL: fixed profile пока нельзя commit-ить."
echo "Отчёт: ${REPORT}"
exit 1
