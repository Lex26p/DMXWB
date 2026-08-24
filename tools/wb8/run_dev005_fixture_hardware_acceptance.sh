#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Использование:
  bash tools/wb8/run_dev005_fixture_hardware_acceptance.sh user@wb8-host [start-address]

DEV-005B hardware smoke новой Fixture model:
  - один RGBW Fixture;
  - FixtureCollection -> DmxSnapshot -> DmxOutput;
  - fixed physical output 44 Hz;
  - RED / GREEN / BLUE;
  - Temperature 0 / 50 / 100%;
  - Brightness 50%;
  - Power OFF / ON restore;
  - Reset;
  - final all-off.

Для текущего стенда /dev/ttyRS485-1 считается постоянно отключённым
в WB Serial Device Driver Configuration. wb-mqtt-serial не останавливается.
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
DURATION=3
REFRESH=44
EXPECTED_FRAMES=$((REFRESH * DURATION))
MIN_FRAMES=$((EXPECTED_FRAMES - 4))

if [[ ! "${START_ADDRESS}" =~ ^[0-9]+$ ]] || (( START_ADDRESS < 1 || START_ADDRESS > 297 )); then
    echo "start-address должен быть целым числом 1..297 для одного RGBW Fixture внутри physical limit 300." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
REPORT="${REPO_ROOT}/docs/DEV005_FIXTURE_HARDWARE_REPORT.txt"
REMOTE_DIR="/tmp/dmxwb-dev005-fixture"
CONTROL_DIR="${TMPDIR:-/tmp}/dmxwb-dev005-ssh-${USER:-user}"
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
    "${REPO_ROOT}/include/dmxwb/fixture.hpp" \
    "${REPO_ROOT}/include/dmxwb/dmx_output.hpp" \
    "${REPO_ROOT}/src/fixture.cpp" \
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
    -o "ControlPersist=1200"
    -o "StrictHostKeyChecking=accept-new"
)
SCP_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CONTROL_PATH}"
    -o "ControlPersist=1200"
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
        if (( REMOTE_READY == 1 && TEST_COMPLETE == 0 )); then
            echo
            echo "Best-effort: выключаем Fixture через новую модель после прерванного теста..."
            remote "'${REMOTE_DIR}/dmxwb' --fixture-hardware-test all-off --port '${PORT}' --start-address '${START_ADDRESS}' --seconds 1" >/dev/null 2>&1 || true
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

STATES=(
    red
    green
    blue
    temperature-0
    temperature-50
    temperature-100
    brightness-50
    power-off
    power-on-restore
    reset
)

EXPECTED_RGBW=(
    255/0/0/0
    0/255/0/0
    0/0/255/0
    255/255/255/0
    255/255/255/128
    255/255/255/255
    127/127/127/127
    0/0/0/0
    127/127/127/127
    255/255/255/255
)

DESCRIPTIONS=(
    "КРАСНЫЙ: только R"
    "ЗЕЛЁНЫЙ: только G"
    "СИНИЙ: только B"
    "Temperature 0%: холодный белый, W=0"
    "Temperature 50%: RGB full + W примерно 128"
    "Temperature 100%: RGBW все 255"
    "Brightness 50% после Temperature 100%: RGBW примерно половина"
    "Power OFF: свет полностью выключен"
    "Power ON restore: восстановлена половинная яркость RGBW"
    "Reset: Power ON, Brightness 100%, RGBW все 255"
)

echo "DMXWB — DEV-005B Fixture RGBW hardware acceptance"
echo "Контроллер: ${TARGET}"
echo "Порт: ${PORT}"
echo "Fixture Start Address: ${START_ADDRESS}"
echo "Fixed refresh: ${REFRESH} Hz"
echo "Каждый визуальный шаг: ${DURATION} s (~${EXPECTED_FRAMES} кадров)"
echo
echo "Этот тест использует новую цепочку FixtureCollection -> DmxSnapshot -> DmxOutput."
echo "Старый diagnostic pattern generator для проверяемых состояний не используется."
echo "${PORT} считается уже отключённым в WB Serial Device Driver Configuration."
echo "wb-mqtt-serial останавливать не нужно."
echo

if ! ask_yes_no "Светильник подключён безопасно и готов к DEV-005B?"; then
    echo "Тест отменён."
    exit 2
fi

: > "${REPORT}"
record "=== DMXWB DEV-005 Fixture RGBW hardware acceptance ==="
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
record "fixture_start_address: ${START_ADDRESS}"
record "fixed_refresh_hz: ${REFRESH}"
record "step_duration_seconds: ${DURATION}"
record "minimum_frames_per_step: ${MIN_FRAMES}"

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

if ! remote "'${REMOTE_DIR}/dmxwb' --help" | grep -Fq -- "--fixture-hardware-test STATE"; then
    record "binary_fixture_cli_check: FAIL"
    echo "На контроллер попал binary без DEV-005 Fixture hardware CLI." >&2
    exit 1
fi
record "binary_fixture_cli_check: PASS"

for index in "${!STATES[@]}"; do
    state="${STATES[$index]}"
    expected="${EXPECTED_RGBW[$index]}"
    description="${DESCRIPTIONS[$index]}"
    number=$((index + 1))

    echo
    echo "============================================================"
    echo "TEST ${number}/${#STATES[@]} — ${state}"
    echo "============================================================"
    echo "Ожидается: ${description}"
    echo "Ожидаемый actual RGBW: ${expected}"
    read -r -p "Нажмите Enter для запуска этого состояния... " _

    record ""
    record "--- fixture state ${number}/${#STATES[@]}: ${state} ---"
    set +e
    OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --fixture-hardware-test '${state}' --port '${PORT}' --start-address '${START_ADDRESS}' --seconds '${DURATION}'" 2>&1)"
    RC=$?
    set -e
    printf '%s\n' "${OUTPUT}" | tee -a "${REPORT}"

    frames="$(diag_value "${OUTPUT}" frames_sent)"
    open_failures="$(diag_value "${OUTPUT}" open_failures)"
    send_failures="$(diag_value "${OUTPUT}" send_failures)"
    recoveries="$(diag_value "${OUTPUT}" recoveries)"
    missed="$(diag_value "${OUTPUT}" missed_deadlines)"
    active="$(diag_value "${OUTPUT}" active_refresh_hz)"
    actual="$(diag_value "${OUTPUT}" actual_rgbw)"
    snapshot_check="$(diag_value "${OUTPUT}" snapshot_check)"
    software_result="$(diag_value "${OUTPUT}" software_result)"
    serial_open_after_stop="$(diag_value "${OUTPUT}" serial_open_after_stop)"

    software_pass=0
    if is_uint "${frames}" && is_uint "${open_failures}" && is_uint "${send_failures}" && \
       is_uint "${recoveries}" && is_uint "${missed}" && is_uint "${active}" && \
       is_uint "${serial_open_after_stop}"; then
        if (( RC == 0 && frames >= MIN_FRAMES && open_failures == 0 && send_failures == 0 && \
              recoveries == 0 && missed == 0 && active == 44 && serial_open_after_stop == 0 )) && \
           [[ "${actual}" == "${expected}" && "${snapshot_check}" == "PASS" && "${software_result}" == "PASS" ]]; then
            software_pass=1
        fi
    fi

    if (( software_pass == 1 )); then
        record "step_software_acceptance: PASS"
    else
        record "step_software_acceptance: FAIL"
        record "expected: frames>=${MIN_FRAMES}, active=44, failures/recoveries/missed=0, serial_closed=1, actual_rgbw=${expected}, snapshot_check=PASS"
        echo "FAIL: software acceptance шага ${state}. Останавливаем DEV-005B." >&2
        exit 1
    fi

    if ask_yes_no "Визуально состояние соответствует: ${description}?"; then
        record "step_user_observation: PASS"
    else
        record "step_user_observation: FAIL"
        echo "FAIL: визуальное состояние ${state} не подтверждено." >&2
        exit 1
    fi
done

record ""
record "--- final all-off through Fixture model ---"
echo
echo "============================================================"
echo "FINAL — ALL OFF через Fixture model"
echo "============================================================"
set +e
FINAL_OUTPUT="$(remote "'${REMOTE_DIR}/dmxwb' --fixture-hardware-test all-off --port '${PORT}' --start-address '${START_ADDRESS}' --seconds 1" 2>&1)"
FINAL_RC=$?
set -e
printf '%s\n' "${FINAL_OUTPUT}" | tee -a "${REPORT}"

final_actual="$(diag_value "${FINAL_OUTPUT}" actual_rgbw)"
final_snapshot_check="$(diag_value "${FINAL_OUTPUT}" snapshot_check)"
final_software_result="$(diag_value "${FINAL_OUTPUT}" software_result)"
final_serial="$(diag_value "${FINAL_OUTPUT}" serial_open_after_stop)"

if (( FINAL_RC != 0 )) || [[ "${final_actual}" != "0/0/0/0" || "${final_snapshot_check}" != "PASS" || \
    "${final_software_result}" != "PASS" || "${final_serial}" != "0" ]]; then
    record "final_all_off_software: FAIL"
    echo "FAIL: final all-off software check." >&2
    exit 1
fi
record "final_all_off_software: PASS"

if ask_yes_no "Светильник после final all-off полностью выключен?"; then
    record "final_all_off_user_observation: PASS"
else
    record "final_all_off_user_observation: FAIL"
    echo "FAIL: final all-off не подтверждён визуально." >&2
    exit 1
fi

TEST_COMPLETE=1
record "dev005_fixture_hardware_result: PASS"
record "=== DMXWB DEV-005 FIXTURE RGBW HARDWARE PASS ==="
echo
echo "PASS: новая Fixture model физически подтверждена через DmxOutput на WB8."
echo "Отчёт: ${REPORT}"
exit 0
