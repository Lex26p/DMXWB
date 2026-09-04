#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export DMXWB_REBOOT_ACCEPTANCE_VARIANT=dev013d
exec bash "${SCRIPT_DIR}/run_dev012d3_reboot_runtime_acceptance.sh" "$@"
