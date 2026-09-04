#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export DMXWB_SYSTEMD_ACCEPTANCE_VARIANT=dev012b3
exec bash "${SCRIPT_DIR}/run_dev012b_systemd_acceptance.sh" "$@"
