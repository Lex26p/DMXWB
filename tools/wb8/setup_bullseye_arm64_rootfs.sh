#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

ROOTFS="${DMXWB_WB8_ROOTFS:-/opt/dmxwb/wb8-bullseye-cross-arm64}"
DEBIAN_MIRROR="${DMXWB_DEBIAN_MIRROR:-http://deb.debian.org/debian}"
SECURITY_MIRROR="${DMXWB_DEBIAN_SECURITY_MIRROR:-http://security.debian.org/debian-security}"
MARKER="${ROOTFS}/.dmxwb-bullseye-cross-arm64-ready"

if [[ ${EUID} -ne 0 ]]; then
    exec sudo env \
        DMXWB_WB8_ROOTFS="${ROOTFS}" \
        DMXWB_DEBIAN_MIRROR="${DEBIAN_MIRROR}" \
        DMXWB_DEBIAN_SECURITY_MIRROR="${SECURITY_MIRROR}" \
        bash "$0" "$@"
fi

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing host command: $1" >&2
        exit 1
    fi
}

require_command debootstrap
require_command chroot
require_command dpkg

HOST_ARCH="$(dpkg --print-architecture)"
if [[ "${HOST_ARCH}" != "amd64" ]]; then
    echo "This DEV-003A setup currently expects an amd64/x86_64 local Linux host; got: ${HOST_ARCH}" >&2
    exit 1
fi

show_environment() {
    echo "WB8 Bullseye cross-build rootfs: ${ROOTFS}"
    chroot "${ROOTFS}" /bin/bash -lc '
        set -e
        printf "build_rootfs_arch: "; dpkg --print-architecture
        printf "cross_g++: "; aarch64-linux-gnu-g++ --version | head -n1
        printf "cmake: "; cmake --version | head -n1
        printf "arm64_cross_libc: "; dpkg-query -W -f="\${Version}\n" libc6-dev-arm64-cross
    '
}

if [[ -f "${MARKER}" ]]; then
    echo "WB8 Bullseye cross-build rootfs is already ready."
    show_environment
    exit 0
fi

if [[ -e "${ROOTFS}" ]] && [[ -n "$(find "${ROOTFS}" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
    echo "Rootfs exists but is not marked ready: ${ROOTFS}" >&2
    echo "If it is a failed/incomplete DEV-003A rootfs, remove it manually and rerun this script:" >&2
    echo "  sudo rm -rf '${ROOTFS}'" >&2
    exit 1
fi

mkdir -p "${ROOTFS}"

echo "Creating native Debian 11 Bullseye amd64 build rootfs in ${ROOTFS}"
echo "ARM64 output will be produced by Bullseye's aarch64-linux-gnu cross compiler; QEMU is not required."
debootstrap --arch=amd64 --variant=minbase bullseye "${ROOTFS}" "${DEBIAN_MIRROR}"

cat > "${ROOTFS}/etc/apt/sources.list" <<SOURCES
deb ${DEBIAN_MIRROR} bullseye main
deb ${DEBIAN_MIRROR} bullseye-updates main
deb ${SECURITY_MIRROR} bullseye-security main
SOURCES

cp -L /etc/resolv.conf "${ROOTFS}/etc/resolv.conf"

chroot "${ROOTFS}" /bin/bash -lc '
    set -e
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
        file \
        binutils \
        crossbuild-essential-arm64
    apt-get clean
'

touch "${MARKER}"

echo
show_environment
echo "WB8 Bullseye ARM64 cross-build rootfs is ready."
