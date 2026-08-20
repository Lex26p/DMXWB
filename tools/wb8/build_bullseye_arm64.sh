#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ROOTFS="${DMXWB_WB8_ROOTFS:-/opt/dmxwb/wb8-bullseye-cross-arm64}"
MARKER="${ROOTFS}/.dmxwb-bullseye-cross-arm64-ready"
CHROOT_SOURCE="${ROOTFS}/work/dmxwb"
OUTPUT_DIR="${REPO_ROOT}/artifacts/wb8-bullseye-arm64"
OUTPUT_BIN="${OUTPUT_DIR}/dmxwb"
HOST_BUILD="${REPO_ROOT}/build-linux-dev003a"

if [[ ! -f "${MARKER}" ]]; then
    echo "WB8 Bullseye cross-build rootfs is not ready: ${ROOTFS}" >&2
    echo "Run tools/wb8/setup_bullseye_arm64_rootfs.sh first." >&2
    exit 1
fi

for command_name in cmake tar readelf file sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

echo "=== Native Linux verification ==="
rm -rf "${HOST_BUILD}"
cmake -S "${REPO_ROOT}" -B "${HOST_BUILD}" \
    -DBUILD_TESTING=ON \
    -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build "${HOST_BUILD}" -j2
ctest --test-dir "${HOST_BUILD}" --output-on-failure
"${HOST_BUILD}/dmxwb" --version

echo
echo "=== Bullseye GCC 10 ARM64 cross build ==="
sudo rm -rf "${CHROOT_SOURCE}"
sudo mkdir -p "${CHROOT_SOURCE}"

tar \
    --exclude='./.git' \
    --exclude='./.vs' \
    --exclude='./build' \
    --exclude='./build-*' \
    --exclude='./cmake-build-*' \
    --exclude='./artifacts' \
    -C "${REPO_ROOT}" -cf - . \
    | sudo tar -C "${CHROOT_SOURCE}" -xf -

sudo chroot "${ROOTFS}" /bin/bash -lc '
    set -euo pipefail
    cd /work/dmxwb
    rm -rf build-wb8
    cmake -S . -B build-wb8 \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DDMXWB_WARNINGS_AS_ERRORS=ON \
        -DDMXWB_STATIC_GNU_RUNTIME=ON
    cmake --build build-wb8 -j2 --target dmxwb
'

mkdir -p "${OUTPUT_DIR}"
sudo cp "${CHROOT_SOURCE}/build-wb8/dmxwb" "${OUTPUT_BIN}"
sudo chown "$(id -u):$(id -g)" "${OUTPUT_BIN}"
chmod 0755 "${OUTPUT_BIN}"

echo
echo "Target artifact: ${OUTPUT_BIN}"
file "${OUTPUT_BIN}"

if ! readelf -h "${OUTPUT_BIN}" | grep -q 'Machine:.*AArch64'; then
    echo "Unexpected target architecture; expected ELF AArch64." >&2
    readelf -h "${OUTPUT_BIN}" >&2 || true
    exit 1
fi

if readelf -d "${OUTPUT_BIN}" | grep -Eq 'libstdc\+\+|libgcc_s'; then
    echo "GNU C++ runtime is still dynamically required; portable runtime link failed." >&2
    readelf -d "${OUTPUT_BIN}" | grep NEEDED || true
    exit 1
fi

MAX_GLIBC="$(readelf --version-info "${OUTPUT_BIN}" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -V | tail -n1 || true)"
if [[ -z "${MAX_GLIBC}" ]]; then
    echo "Could not determine GLIBC requirement." >&2
    exit 1
fi

if [[ "$(printf '%s\n%s\n' "${MAX_GLIBC}" 'GLIBC_2.31' | sort -V | tail -n1)" != 'GLIBC_2.31' ]]; then
    echo "Binary requires ${MAX_GLIBC}, newer than Bullseye GLIBC_2.31." >&2
    exit 1
fi

echo "Maximum required glibc symbol version: ${MAX_GLIBC}"
echo "Dynamic dependencies:"
readelf -d "${OUTPUT_BIN}" | grep NEEDED || true
sha256sum "${OUTPUT_BIN}"
echo "DEV-003A laptop cross build completed. Target execution on a real WB8 is still required for PASS."
