#!/bin/bash
# Nordic Secure DFU release wrapper.
# Example:
#   DFU_APPLICATION_VERSION=0x1A DFU_PREVIOUS_APPLICATION_VERSION=0x19 \
#     ./tools/make-ota-nrf52.sh output/release/nrf52811_xxaa

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: DFU_APPLICATION_VERSION=<new> DFU_PREVIOUS_APPLICATION_VERSION=<old> $0 <output_prefix>"
    exit 1
fi

: "${DFU_APPLICATION_VERSION:?set a new monotonic Nordic DFU application version}"
: "${DFU_PREVIOUS_APPLICATION_VERSION:?set the currently released Nordic DFU application version}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$1"
OUT_DIR="$(dirname "$PREFIX")"
NAME="$(basename "$PREFIX")"

args=(
    --application "${PREFIX}.hex"
    --application-version "$DFU_APPLICATION_VERSION"
    --previous-version "$DFU_PREVIOUS_APPLICATION_VERSION"
    --output "$OUT_DIR"
    --name "$NAME"
    --nrfutil "${NRFUTIL:-nrfutil}"
    --mergehex "${MERGEHEX:-mergehex}"
)
if [[ -n "${DFU_RESOURCES_DIR:-}" ]]; then
    args+=(--resources-dir "$DFU_RESOURCES_DIR")
fi
exec python3 "$SCRIPT_DIR/dfu_release.py" "${args[@]}"
