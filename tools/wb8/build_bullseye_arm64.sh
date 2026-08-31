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
OUTPUT_HARDWARE_DIAGNOSTICS="${OUTPUT_DIR}/dmxwb-hardware-diagnostics"
OUTPUT_MQTT_ACCEPTANCE="${OUTPUT_DIR}/dmxwb-mqtt-acceptance"
HOST_BUILD="${REPO_ROOT}/build-linux-wb8"

if [[ ! -f "${MARKER}" ]]; then
    echo "WB8 Bullseye cross-build rootfs is not ready: ${ROOTFS}" >&2
    echo "Run tools/wb8/setup_bullseye_arm64_rootfs.sh first." >&2
    exit 1
fi

for command_name in cmake tar readelf file sha256sum grep; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "Missing host command: ${command_name}" >&2
        exit 1
    fi
done

if ! sudo chroot "${ROOTFS}" /bin/bash -lc '
    set -e
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    pkg-config --exists libmosquitto
    printf "cross libmosquitto: "; pkg-config --modversion libmosquitto
' ; then
    echo "Bullseye ARM64 libmosquitto dependency is missing." >&2
    echo "Run tools/wb8/ensure_bullseye_mosquitto_arm64.sh first." >&2
    exit 1
fi

echo "=== Native Linux verification ==="
rm -rf "${HOST_BUILD}"
cmake -S "${REPO_ROOT}" -B "${HOST_BUILD}" \
    -DBUILD_TESTING=ON \
    -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build "${HOST_BUILD}" -j2
ctest --test-dir "${HOST_BUILD}" --output-on-failure
"${HOST_BUILD}/dmxwb" --version
"${HOST_BUILD}/dmxwb" --help | grep -Fq '/etc/dmxwb/config.json'
"${HOST_BUILD}/dmxwb" --help | grep -Fq '/var/lib/dmxwb/state.json'
"${HOST_BUILD}/dmxwb-hardware-diagnostics" --version
"${HOST_BUILD}/dmxwb-dev010-source-acceptance" --help >/dev/null

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
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
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
    cmake --build build-wb8 -j2 --target \
        dmxwb \
        dmxwb_hardware_diagnostics \
        dmxwb_mqtt_acceptance
'

mkdir -p "${OUTPUT_DIR}"
sudo cp "${CHROOT_SOURCE}/build-wb8/dmxwb" "${OUTPUT_BIN}"
sudo cp \
    "${CHROOT_SOURCE}/build-wb8/dmxwb-hardware-diagnostics" \
    "${OUTPUT_HARDWARE_DIAGNOSTICS}"
sudo cp \
    "${CHROOT_SOURCE}/build-wb8/dmxwb-mqtt-acceptance" \
    "${OUTPUT_MQTT_ACCEPTANCE}"
sudo chown "$(id -u):$(id -g)" \
    "${OUTPUT_BIN}" \
    "${OUTPUT_HARDWARE_DIAGNOSTICS}" \
    "${OUTPUT_MQTT_ACCEPTANCE}"
chmod 0755 \
    "${OUTPUT_BIN}" \
    "${OUTPUT_HARDWARE_DIAGNOSTICS}" \
    "${OUTPUT_MQTT_ACCEPTANCE}"

verify_artifact() {
    local artifact="$1"
    local require_mosquitto="$2"

    echo
    echo "Target artifact: ${artifact}"
    file "${artifact}"

    if ! readelf -h "${artifact}" | grep -q 'Machine:.*AArch64'; then
        echo "Unexpected target architecture; expected ELF AArch64." >&2
        readelf -h "${artifact}" >&2 || true
        exit 1
    fi

    if readelf -d "${artifact}" | grep -Eq 'libstdc\+\+|libgcc_s'; then
        echo "GNU C++ runtime is still dynamically required; portable runtime link failed." >&2
        readelf -d "${artifact}" | grep NEEDED || true
        exit 1
    fi

    local max_glibc
    max_glibc="$(
        readelf --version-info "${artifact}" \
            | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' \
            | sort -V \
            | tail -n1 \
            || true
    )"
    if [[ -z "${max_glibc}" ]]; then
        echo "Could not determine GLIBC requirement." >&2
        exit 1
    fi
    if [[ "$(
        printf '%s\n%s\n' "${max_glibc}" 'GLIBC_2.31' \
            | sort -V \
            | tail -n1
    )" != 'GLIBC_2.31' ]]; then
        echo "Binary requires ${max_glibc}, newer than Bullseye GLIBC_2.31." >&2
        exit 1
    fi

    if [[ "${require_mosquitto}" == "yes" ]] && \
       ! readelf -d "${artifact}" | grep -Fq 'libmosquitto.so.1'; then
        echo "Artifact does not dynamically require libmosquitto.so.1." >&2
        readelf -d "${artifact}" | grep NEEDED || true
        exit 1
    fi

    echo "Maximum required glibc symbol version: ${max_glibc}"
    echo "Dynamic dependencies:"
    readelf -d "${artifact}" | grep NEEDED || true
    sha256sum "${artifact}"
}

# DEV-012A production dmxwb is now the integrated MQTT runtime and therefore
# must carry the MQTT dynamic dependency. The separate historical hardware
# diagnostic executable remains MQTT-independent.
verify_artifact "${OUTPUT_BIN}" yes
verify_artifact "${OUTPUT_HARDWARE_DIAGNOSTICS}" no
verify_artifact "${OUTPUT_MQTT_ACCEPTANCE}" yes

echo "WB8 laptop cross build completed."
echo "Run tools/wb8/verify_on_target.sh for target CLI verification when required by the current gate."
