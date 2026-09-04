#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

for command_name in cmp cp mktemp python3 rm sed; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

SOURCE_ID="dev012c1-local"
APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
ARCHIVE="${REPO_ROOT}/artifacts/offline/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64-dev012c1-payload.tar.gz"
TEMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf -- "${TEMP_DIR}"
}
trap cleanup EXIT

echo "=== DMXWB DEV-012C1 OFFLINE PAYLOAD REGRESSION ==="
SOURCE_DATE_EPOCH=0 bash "${SCRIPT_DIR}/build_dev012c1_offline_payload.sh" "${SOURCE_ID}"
cp "${ARCHIVE}" "${TEMP_DIR}/first.tar.gz"
SOURCE_DATE_EPOCH=0 bash "${SCRIPT_DIR}/build_dev012c1_offline_payload.sh" "${SOURCE_ID}"

if ! cmp -s "${TEMP_DIR}/first.tar.gz" "${ARCHIVE}"; then
    echo "Repeated payload build is not byte-for-byte reproducible." >&2
    exit 1
fi
echo "dev012c1_reproducible_archive: PASS"

python3 "${SCRIPT_DIR}/check_dev012c1_bundle.py" "${ARCHIVE}" "${SOURCE_ID}"
echo "=== DMXWB DEV-012C1 OFFLINE PAYLOAD REGRESSION PASS ==="
echo "Archive: ${ARCHIVE}"
