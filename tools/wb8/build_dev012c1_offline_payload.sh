#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Usage:
  bash tools/wb8/build_dev012c1_offline_payload.sh SOURCE_ID

SOURCE_ID identifies the exact source used for the artifact. DEV-012C4 will
require the final committed source SHA. SOURCE_DATE_EPOCH may be supplied to
record and reproduce a specific build timestamp; its default is 0.
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
BINARY="${REPO_ROOT}/artifacts/wb8-bullseye-arm64/dmxwb"
SERVICE="${REPO_ROOT}/deploy/dmxwb.service"
EXAMPLE_CONFIG="${REPO_ROOT}/deploy/config.example.json"
WEB_ROOT="${REPO_ROOT}/www/dmxwb"
OUTPUT_DIR="${REPO_ROOT}/artifacts/offline"
BUNDLE_DIR_NAME="dmxwb-wb8-bullseye-arm64"

for command_name in awk chmod date find grep gzip install mapfile mkdir mktemp readelf rm sed sha256sum sort tail tar touch; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

for required_file in \
    "${BINARY}" \
    "${SERVICE}" \
    "${EXAMPLE_CONFIG}" \
    "${WEB_ROOT}/index.html" \
    "${WEB_ROOT}/app.js" \
    "${WEB_ROOT}/model.js" \
    "${WEB_ROOT}/mqtt-client.js" \
    "${WEB_ROOT}/styles.css"; do
    if [[ ! -f "${required_file}" ]]; then
        echo "Required payload source is missing: ${required_file}" >&2
        exit 1
    fi
done
if [[ ! -x "${BINARY}" ]]; then
    echo "Production artifact is not executable: ${BINARY}" >&2
    exit 1
fi

if [[ "${REPO_ROOT}/CMakeLists.txt" -nt "${BINARY}" ]] ||
   find "${REPO_ROOT}/src" "${REPO_ROOT}/include" -type f -newer "${BINARY}" -print -quit |
       grep -q .; then
    echo "Production artifact is older than C++ sources; rebuild it first." >&2
    exit 1
fi

APPLICATION_VERSION="$(
    sed -nE \
        's/^project\(DMXWB VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${REPO_ROOT}/CMakeLists.txt"
)"
if [[ -z "${APPLICATION_VERSION}" ]]; then
    echo "Cannot determine DMXWB application version." >&2
    exit 1
fi

if ! readelf -h "${BINARY}" | grep -q 'Machine:.*AArch64'; then
    echo "Production artifact is not ELF AArch64." >&2
    exit 1
fi
if readelf -d "${BINARY}" | grep -Eq 'libstdc\+\+|libgcc_s'; then
    echo "Production artifact unexpectedly requires dynamic GNU C++ runtime." >&2
    exit 1
fi

MAX_GLIBC="$(
    readelf --version-info "${BINARY}" |
        grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' |
        sort -V |
        tail -n1
)"
if [[ -z "${MAX_GLIBC}" ]]; then
    echo "Cannot determine maximum GLIBC requirement." >&2
    exit 1
fi
if [[ "$(printf '%s\n%s\n' "${MAX_GLIBC}" GLIBC_2.31 | sort -V | tail -n1)" != "GLIBC_2.31" ]]; then
    echo "Production artifact requires ${MAX_GLIBC}, newer than Bullseye GLIBC_2.31." >&2
    exit 1
fi

mapfile -t DYNAMIC_DEPENDENCIES < <(
    readelf -d "${BINARY}" |
        sed -nE 's/.*Shared library: \[([^]]+)\].*/\1/p' |
        sort -u
)
if [[ ! " ${DYNAMIC_DEPENDENCIES[*]} " =~ " libmosquitto.so.1 " ]]; then
    echo "Production artifact does not require libmosquitto.so.1." >&2
    exit 1
fi

ARTIFACT_SHA256="$(sha256sum "${BINARY}" | awk '{print $1}')"
GENERATED_UTC="$(date -u -d "@${BUILD_EPOCH}" +%Y-%m-%dT%H:%M:%SZ)"
STAGE_DIR="$(mktemp -d)"
cleanup() {
    rm -rf -- "${STAGE_DIR}"
}
trap cleanup EXIT

BUNDLE_ROOT="${STAGE_DIR}/${BUNDLE_DIR_NAME}"
install -d -m 0755 \
    "${BUNDLE_ROOT}/payload/usr/local/bin" \
    "${BUNDLE_ROOT}/payload/etc/dmxwb" \
    "${BUNDLE_ROOT}/payload/etc/systemd/system" \
    "${BUNDLE_ROOT}/payload/var/www/dmxwb"

install -m 0755 "${BINARY}" "${BUNDLE_ROOT}/payload/usr/local/bin/dmxwb"
install -m 0644 "${SERVICE}" "${BUNDLE_ROOT}/payload/etc/systemd/system/dmxwb.service"
install -m 0644 "${EXAMPLE_CONFIG}" "${BUNDLE_ROOT}/payload/etc/dmxwb/config.example.json"
for web_file in index.html app.js model.js mqtt-client.js styles.css; do
    install -m 0644 \
        "${WEB_ROOT}/${web_file}" \
        "${BUNDLE_ROOT}/payload/var/www/dmxwb/${web_file}"
done

{
    printf 'bundle_format=1\n'
    printf 'bundle_kind=dev012c1-payload\n'
    printf 'product=DMXWB\n'
    printf 'application_version=%s\n' "${APPLICATION_VERSION}"
    printf 'source_id=%s\n' "${SOURCE_ID}"
    printf 'target=wb8-bullseye-arm64\n'
    printf 'build_epoch=%s\n' "${BUILD_EPOCH}"
    printf 'generated_utc=%s\n' "${GENERATED_UTC}"
    printf 'artifact_sha256=%s\n' "${ARTIFACT_SHA256}"
    printf 'architecture=AArch64\n'
    printf 'max_glibc=%s\n' "${MAX_GLIBC}"
    for dependency in "${DYNAMIC_DEPENDENCIES[@]}"; do
        printf 'dynamic_dependency=%s\n' "${dependency}"
    done
} > "${BUNDLE_ROOT}/MANIFEST.txt"
chmod 0644 "${BUNDLE_ROOT}/MANIFEST.txt"

(
    cd "${BUNDLE_ROOT}"
    find . -type f ! -name SHA256SUMS -print0 |
        sort -z |
        while IFS= read -r -d '' relative_path; do
            checksum="$(sha256sum "${relative_path}" | awk '{print $1}')"
            printf '%s  %s\n' "${checksum}" "${relative_path#./}"
        done > SHA256SUMS
)
chmod 0644 "${BUNDLE_ROOT}/SHA256SUMS"

find "${BUNDLE_ROOT}" -exec touch -h -d "@${BUILD_EPOCH}" {} +
mkdir -p "${OUTPUT_DIR}"
OUTPUT_ARCHIVE="${OUTPUT_DIR}/dmxwb-${APPLICATION_VERSION}-wb8-bullseye-arm64-dev012c1-payload.tar.gz"
TEMP_ARCHIVE="${STAGE_DIR}/payload.tar.gz"
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

echo "DEV-012C1 offline payload created."
echo "Archive: ${OUTPUT_ARCHIVE}"
echo "Archive SHA256: $(sha256sum "${OUTPUT_ARCHIVE}" | awk '{print $1}')"
