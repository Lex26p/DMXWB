#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ROOTFS="${DMXWB_WB8_ROOTFS:-/opt/dmxwb/wb8-bullseye-cross-arm64}"
CHROOT_SOURCE="${ROOTFS}/work/dmxwb"
OUTPUT_DIR="${REPO_ROOT}/artifacts/wb8-bullseye-arm64"
OUTPUT_BIN="${OUTPUT_DIR}/dmxwb-artnet-acceptance"

for command_name in readelf file sha256sum; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        echo "Missing host command: ${command_name}" >&2
        exit 1
    }
done

# Reuse the established build/test/cross-build gate first. This refreshes the
# chroot source tree from the current worktree and proves existing targets.
bash "${SCRIPT_DIR}/build_bullseye_arm64.sh"

echo
echo "=== DEV-010A Art-Net acceptance ARM64 target ==="
sudo chroot "${ROOTFS}" /bin/bash -lc '
    set -euo pipefail
    cd /work/dmxwb
    cmake --build build-wb8 -j2 --target dmxwb_artnet_acceptance
'

mkdir -p "${OUTPUT_DIR}"
sudo cp "${CHROOT_SOURCE}/build-wb8/dmxwb-artnet-acceptance" "${OUTPUT_BIN}"
sudo chown "$(id -u):$(id -g)" "${OUTPUT_BIN}"
chmod 0755 "${OUTPUT_BIN}"

file "${OUTPUT_BIN}"
if ! readelf -h "${OUTPUT_BIN}" | grep -q 'Machine:.*AArch64'; then
    echo "Unexpected target architecture; expected ELF AArch64." >&2
    exit 1
fi
if readelf -d "${OUTPUT_BIN}" | grep -Eq 'libstdc\+\+|libgcc_s'; then
    echo "GNU C++ runtime is still dynamically required." >&2
    readelf -d "${OUTPUT_BIN}" | grep NEEDED || true
    exit 1
fi
max_glibc="$(readelf --version-info "${OUTPUT_BIN}" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -V | tail -n1 || true)"
if [[ -z "${max_glibc}" ]]; then
    echo "Could not determine GLIBC requirement." >&2
    exit 1
fi
if [[ "$(printf '%s\n%s\n' "${max_glibc}" 'GLIBC_2.31' | sort -V | tail -n1)" != 'GLIBC_2.31' ]]; then
    echo "Binary requires ${max_glibc}, newer than Bullseye GLIBC_2.31." >&2
    exit 1
fi

echo "Maximum required glibc symbol version: ${max_glibc}"
echo "Dynamic dependencies:"
readelf -d "${OUTPUT_BIN}" | grep NEEDED || true
sha256sum "${OUTPUT_BIN}"
echo "DEV-010A Art-Net acceptance artifact completed: ${OUTPUT_BIN}"
