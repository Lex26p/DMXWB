#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

if [[ $# -gt 1 ]]; then
    echo "Usage: bash tools/wb8/run_dev012c4_final_bundle_regression.sh [SOURCE_ID]" >&2
    exit 2
fi

for command_name in bash cmp cp mktemp python3 rm sed sha256sum tar; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

SOURCE_ID="${1:-dev012c4-local}"
APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
ARCHIVE="${REPO_ROOT}/artifacts/offline/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64.tar.gz"
TEMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf -- "${TEMP_DIR}"
}
trap cleanup EXIT

echo "=== DMXWB DEV-012C4 FINAL OFFLINE BUNDLE REGRESSION ==="
echo "=== Focused production ARM64 build ==="
bash "${SCRIPT_DIR}/build_bullseye_arm64.sh" --production-only

echo "=== Final bundle reproducibility and contract ==="
SOURCE_DATE_EPOCH=0 \
    bash "${SCRIPT_DIR}/build_dev012c4_offline_bundle.sh" "${SOURCE_ID}"
cp "${ARCHIVE}" "${TEMP_DIR}/first.tar.gz"
SOURCE_DATE_EPOCH=0 \
    bash "${SCRIPT_DIR}/build_dev012c4_offline_bundle.sh" "${SOURCE_ID}"
if ! cmp -s "${TEMP_DIR}/first.tar.gz" "${ARCHIVE}"; then
    echo "Repeated final bundle build is not byte-for-byte reproducible." >&2
    exit 1
fi
echo "dev012c4_reproducible_final_archive: PASS"

python3 \
    "${SCRIPT_DIR}/check_dev012c4_final_bundle.py" \
    "${ARCHIVE}" \
    "${SOURCE_ID}" \
    "${APPLICATION_VERSION}" \
    "${REPO_ROOT}/deploy/install_wirenboard.sh" \
    "${REPO_ROOT}/deploy/uninstall_wirenboard.sh"

tar -C "${TEMP_DIR}" -xzf "${ARCHIVE}"
BUNDLE_ROOT="${TEMP_DIR}/dmxwb-wb8-bullseye-arm64"

echo "=== Accepted install/update behavior on final bundle ==="
python3 \
    "${SCRIPT_DIR}/check_dev012c2_install_update.py" \
    "${ARCHIVE}" \
    "${BUNDLE_ROOT}/install.sh"

echo "=== Accepted remove/purge behavior on final bundle ==="
python3 \
    "${SCRIPT_DIR}/check_dev012c3_remove_purge.py" \
    "${ARCHIVE}" \
    "${BUNDLE_ROOT}/install.sh" \
    "${BUNDLE_ROOT}/uninstall.sh"

echo "=== DMXWB DEV-012C4 FINAL OFFLINE BUNDLE REGRESSION PASS ==="
echo "Archive: ${ARCHIVE}"
sha256sum "${ARCHIVE}"
