#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Usage:
  bash tools/wb8/build_dev012c4_offline_bundle.sh SOURCE_ID

Builds the final offline WB8 archive from the already-built production artifact.
SOURCE_ID is recorded verbatim in the bundle manifest. SOURCE_DATE_EPOCH may be
supplied for a reproducible timestamp; its default is 0.
USAGE
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi

SOURCE_ID="$1"
if [[ ! "${SOURCE_ID}" =~ ^[A-Za-z0-9._+-]{1,128}$ ]]; then
    echo "SOURCE_ID must use 1..128 safe identity characters." >&2
    exit 2
fi

BUILD_EPOCH="${SOURCE_DATE_EPOCH:-0}"
if [[ ! "${BUILD_EPOCH}" =~ ^[0-9]{1,12}$ ]]; then
    echo "SOURCE_DATE_EPOCH must be a non-negative integer." >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUNDLE_DIR_NAME="dmxwb-wb8-bullseye-arm64"
INSTALLER="${REPO_ROOT}/deploy/install_wirenboard.sh"
UNINSTALLER="${REPO_ROOT}/deploy/uninstall_wirenboard.sh"
OUTPUT_DIR="${REPO_ROOT}/artifacts/offline"

for command_name in awk bash chmod find grep gzip install mkdir mktemp rm sed sha256sum sort tar touch; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

for script_path in "${INSTALLER}" "${UNINSTALLER}"; do
    if [[ ! -f "${script_path}" ]]; then
        echo "Required deployment script is missing: ${script_path}" >&2
        exit 1
    fi
done

APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
if [[ -z "${APPLICATION_VERSION}" ]]; then
    echo "Cannot determine DMXWB application version." >&2
    exit 1
fi

SOURCE_DATE_EPOCH="${BUILD_EPOCH}" \
    bash "${SCRIPT_DIR}/build_dev012c1_offline_payload.sh" "${SOURCE_ID}"
PAYLOAD_ARCHIVE="${OUTPUT_DIR}/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64-dev012c1-payload.tar.gz"

STAGE_DIR="$(mktemp -d)"
cleanup() {
    rm -rf -- "${STAGE_DIR}"
}
trap cleanup EXIT

tar -C "${STAGE_DIR}" -xzf "${PAYLOAD_ARCHIVE}"
BUNDLE_ROOT="${STAGE_DIR}/${BUNDLE_DIR_NAME}"
if [[ ! -d "${BUNDLE_ROOT}" ]]; then
    echo "Intermediate payload has an unexpected root." >&2
    exit 1
fi

install -m 0755 "${INSTALLER}" "${BUNDLE_ROOT}/install.sh"
install -m 0755 "${UNINSTALLER}" "${BUNDLE_ROOT}/uninstall.sh"
sed -i 's/^bundle_kind=dev012c1-payload$/bundle_kind=production-offline/' \
    "${BUNDLE_ROOT}/MANIFEST.txt"
if ! grep -Fxq 'bundle_kind=production-offline' "${BUNDLE_ROOT}/MANIFEST.txt"; then
    echo "Cannot update final bundle kind." >&2
    exit 1
fi

(
    cd "${BUNDLE_ROOT}"
    find . -type f ! -name SHA256SUMS -print0 |
        sort -z |
        while IFS= read -r -d '' relative_path; do
            checksum="$(sha256sum "${relative_path}" | awk '{print $1}')"
            printf '%s  %s\n' "${checksum}" "${relative_path#./}"
        done > SHA256SUMS
)
chmod 0644 "${BUNDLE_ROOT}/MANIFEST.txt" "${BUNDLE_ROOT}/SHA256SUMS"
find "${BUNDLE_ROOT}" -exec touch -h -d "@${BUILD_EPOCH}" {} +

mkdir -p "${OUTPUT_DIR}"
OUTPUT_ARCHIVE="${OUTPUT_DIR}/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64.tar.gz"
TEMP_ARCHIVE="${STAGE_DIR}/final.tar.gz"
tar \
    --sort=name \
    --mtime="@${BUILD_EPOCH}" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -C "${STAGE_DIR}" \
    -cf - "${BUNDLE_DIR_NAME}" |
    gzip -n > "${TEMP_ARCHIVE}"
install -m 0644 "${TEMP_ARCHIVE}" "${OUTPUT_ARCHIVE}"

echo "DMXWB final offline bundle created."
echo "Archive: ${OUTPUT_ARCHIVE}"
echo "Archive SHA256: $(sha256sum "${OUTPUT_ARCHIVE}" | awk '{print $1}')"
