#!/usr/bin/env bash
# Host-side policy/layout tests. These do not need the ARM toolchain.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${HOST_TEST_OUT:-_build/host_tests}"
CC="${HOST_CC:-gcc}"
CFLAGS="${HOST_CFLAGS:--Wall -Werror -std=c11}"
mkdir -p "$OUT"

run() {
    local name="$1"
    shift
    echo "=== ${name} ==="
    # shellcheck disable=SC2086
    ${CC} ${CFLAGS} "$@" -o "${OUT}/${name}"
    "${OUT}/${name}"
}

run test_cmd_policy -I. -IEPD -IGUI tests/test_cmd_policy.c
run test_clock_layout -IGUI tests/test_clock_layout.c
run test_calendar_grid -IGUI tests/test_calendar_grid.c
run test_clock_refresh_window -IGUI tests/test_clock_refresh_window.c
run test_clock_text -IGUI tests/test_clock_text.c
run test_clock_window_page -IEPD tests/test_clock_window_page.c
run test_refresh_policy -IEPD -IGUI EPD/epd_refresh_policy.c tests/test_refresh_policy.c
run test_clock_refresh -IEPD -IGUI -Itests/stubs EPD/epd_clock_refresh.c tests/test_clock_refresh.c
run test_uc8179 -IEPD -Itests/stubs -Itools/test/host/shim EPD/UC8179.c tests/uc8179_command_sequence_test.c

echo "HOST_OK"
