#!/bin/bash
# Nordic Secure DFU release wrapper.
# Example:
#   ./tools/make-ota-nrf52.sh output/release/nrf52811_xxaa

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_prefix>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$1"
OUT_DIR="$(dirname "$PREFIX")"
NAME="$(basename "$PREFIX")"

args=(
    --application "${PREFIX}.hex"
    --output "$OUT_DIR"
    --name "$NAME"
    --nrfutil "${NRFUTIL:-nrfutil}"
    --mergehex "${MERGEHEX:-mergehex}"
)
if [[ -n "${DFU_RESOURCES_DIR:-}" ]]; then
    args+=(--resources-dir "$DFU_RESOURCES_DIR")
fi
exec python3 "$SCRIPT_DIR/dfu_release.py" "${args[@]}"
