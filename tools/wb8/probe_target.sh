#!/bin/sh
set -eu

echo "=== DMXWB WB8 target probe ==="
printf 'date: '
date -Iseconds 2>/dev/null || date

printf 'device_model: '
if [ -r /proc/device-tree/model ]; then
    tr -d '\000' < /proc/device-tree/model
    echo
else
    echo "unknown"
fi

printf 'uname_machine: '
uname -m
printf 'kernel: '
uname -r

printf 'dpkg_architecture: '
if command -v dpkg >/dev/null 2>&1; then
    dpkg --print-architecture
else
    echo "dpkg-not-found"
fi

printf 'glibc: '
if command -v getconf >/dev/null 2>&1; then
    getconf GNU_LIBC_VERSION 2>/dev/null || true
else
    echo "getconf-not-found"
fi

echo "--- os-release ---"
cat /etc/os-release 2>/dev/null || true

echo "--- wb-release ---"
if command -v wb-release >/dev/null 2>&1; then
    wb-release 2>&1 || true
else
    echo "wb-release-not-found"
fi

echo "--- RS-485 ports ---"
ls -l /dev/ttyRS485-* 2>/dev/null || echo "no /dev/ttyRS485-* devices found"

echo "=== end probe ==="
