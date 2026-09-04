#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_ID="dev012c2-local"
APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
ARCHIVE="${REPO_ROOT}/artifacts/offline/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64-dev012c1-payload.tar.gz"

echo "=== DMXWB DEV-012C2 INSTALL/UPDATE REGRESSION ==="
SOURCE_DATE_EPOCH=0 bash "${SCRIPT_DIR}/build_dev012c1_offline_payload.sh" "${SOURCE_ID}"
python3 \
    "${SCRIPT_DIR}/check_dev012c2_install_update.py" \
    "${ARCHIVE}" \
    "${REPO_ROOT}/deploy/install_wirenboard.sh"
echo "=== DMXWB DEV-012C2 INSTALL/UPDATE REGRESSION PASS ==="
