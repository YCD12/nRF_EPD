#!/bin/bash
# Build every external-Flash ERS package without writing to a device.
# Usage:
#   ./build-external-resources.sh [revision] [output_dir] [EPD-MIG1.json] [calendar-data.json]
# Examples:
#   ./build-external-resources.sh
#   ./build-external-resources.sh 2 output/resources-r2 device-migration.json
#   ./build-external-resources.sh 3 output/resources-r3 "" calendar-2027.json

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

REVISION="${1:-1}"
BUILD_STAMP="$(date +%Y%m%d-%H%M%S)"
OUTPUT_DIR="${2:-output/external-resources-r${REVISION}-${BUILD_STAMP}}"
MIGRATION_BUNDLE="${3:-}"
CALENDAR_DATA="${4:-}"

if [[ -n "$CALENDAR_DATA" && ! -f "$CALENDAR_DATA" ]]; then
    echo "Error: calendar data file was not found: ${CALENDAR_DATA}"
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: Python was not found on PATH."
    exit 1
fi

echo "========================================="
echo "Building external Flash resources"
echo "  Revision : ${REVISION}"
echo "  Output   : ${OUTPUT_DIR}"
if [[ -z "$MIGRATION_BUNDLE" ]]; then
    echo "  Records  : factory-default EPD config and empty annotations"
else
    echo "  Records  : migration bundle \"${MIGRATION_BUNDLE}\""
fi
if [[ -n "$CALENDAR_DATA" ]]; then
    echo "  Calendar : \"${CALENDAR_DATA}\""
fi
echo "========================================="

args=(--check --fonts --calendar)
if [[ -n "$CALENDAR_DATA" ]]; then
    args+=(--calendar-data "$CALENDAR_DATA")
fi
args+=(--ui --todo)

if [[ -z "$MIGRATION_BUNDLE" ]]; then
    args+=(--records --revision "$REVISION" --output "$OUTPUT_DIR")
else
    if [[ ! -f "$MIGRATION_BUNDLE" ]]; then
        echo "Error: migration bundle was not found: ${MIGRATION_BUNDLE}"
        exit 1
    fi
    args+=(--migration-bundle "$MIGRATION_BUNDLE" --revision "$REVISION" --output "$OUTPUT_DIR")
fi

if ! python3 tools/resource_pack.py "${args[@]}"; then
    echo "External resource generation failed."
    exit 1
fi

echo
echo "Generated ERS packages:"
for package in "$OUTPUT_DIR"/*.ers; do
    echo "  $(basename "$package")"
done
echo
echo "Upload order: partition-table, calendar, ui, fonts, epd-config, calendar-annotations, todo."
echo "Generation completed. No device Flash was written."
if [[ "$OUTPUT_DIR" == output/* ]]; then
    printf '%s\n' "${OUTPUT_DIR#output/}" > output/external-resources-latest.txt
fi
