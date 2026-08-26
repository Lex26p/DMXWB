#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

ROOTFS="${DMXWB_WB8_ROOTFS:-/opt/dmxwb/wb8-bullseye-cross-arm64}"
MARKER="${ROOTFS}/.dmxwb-bullseye-cross-arm64-ready"
MOSQUITTO_PC="${ROOTFS}/usr/lib/aarch64-linux-gnu/pkgconfig/libmosquitto.pc"

if [[ ${EUID} -ne 0 ]]; then
    exec sudo env DMXWB_WB8_ROOTFS="${ROOTFS}" bash "$0" "$@"
fi

if [[ ! -f "${MARKER}" ]]; then
    echo "WB8 Bullseye cross-build rootfs is not ready: ${ROOTFS}" >&2
    echo "Run tools/wb8/setup_bullseye_arm64_rootfs.sh first." >&2
    exit 1
fi

if [[ -f "${MOSQUITTO_PC}" ]]; then
    echo "Bullseye ARM64 libmosquitto dependency is already present."
else
    echo "Enabling arm64 packages inside Bullseye build rootfs..."
    chroot "${ROOTFS}" /bin/bash -lc '
        set -euo pipefail
        dpkg --add-architecture arm64
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            pkg-config \
            libmosquitto-dev:arm64
        apt-get clean
    '
fi

if [[ ! -f "${MOSQUITTO_PC}" ]]; then
    echo "libmosquitto.pc for ARM64 was not installed at expected path: ${MOSQUITTO_PC}" >&2
    exit 1
fi

chroot "${ROOTFS}" /bin/bash -lc '
    set -euo pipefail
    export PKG_CONFIG_PATH=
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    printf "arm64 libmosquitto pkg-config version: "
    pkg-config --modversion libmosquitto
    printf "arm64 libmosquitto linker flags: "
    pkg-config --libs libmosquitto
    dpkg-query -W -f="package: \${Package}:\${Architecture} \${Version}\n" libmosquitto-dev:arm64 libmosquitto1:arm64
'

echo "Bullseye ARM64 libmosquitto dependency is ready."
