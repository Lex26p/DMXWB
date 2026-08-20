#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
echo "DEV-004B historical acceptance superseded by the fixed 300-slot / 44 Hz production profile."
echo "Запускаем актуальный acceptance helper."
exec bash "${SCRIPT_DIR}/run_fixed_dmx_profile_acceptance.sh" "$@"
