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
run test_calendar_resources -DCALENDAR_RESOURCES_HOST_TEST -IEPD -IGUI \
    GUI/calendar_resources.c GUI/Lunar.c tests/test_calendar_resources.c
run test_ui_resources -DUI_RESOURCES_HOST_TEST -IEPD -IGUI \
    GUI/ui_resources.c tests/test_ui_resources.c
for maintenance in 0 1; do
    run "test_resource_store_${maintenance}" -DRESOURCE_STORE_MAINTENANCE=$maintenance \
        -Itests/font_transfer_stubs -IEPD \
        -ISDK/17.1.0_ddde560/components/libraries/util \
        -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
        EPD/resource_store.c EPD/persistent_record.c EPD/resource_protocol.c EPD/resource_transfer.c tests/test_resource_store.c
done
run test_advertising -DSVCALL_AS_NORMAL_FUNCTION -DS112 \
    -Itests/advertising_stubs -Itests/storage_stubs -IEPD \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/ble/common \
    -ISDK/17.1.0_ddde560/components/ble/ble_advertising \
    SDK/17.1.0_ddde560/components/ble/common/ble_advdata.c \
    SDK/17.1.0_ddde560/components/ble/ble_advertising/ble_advertising.c \
    EPD/epd_advertising.c tests/test_advertising.c
run test_command_queue -Itests/font_transfer_stubs -IEPD tests/test_command_queue.c
run test_async_queues -Itests/font_transfer_stubs -IEPD -IGUI \
    -DCALENDAR_ANNOTATIONS_USE_NRF_CRITICAL=1 \
    EPD/calendar_annotations_tx.c GUI/calendar_annotations.c tests/test_async_queues.c
run test_storage -Itests/storage_stubs -Itests/font_transfer_stubs -Itests/stubs -I. -IEPD -IGUI \
    EPD/EPD_config.c EPD/calendar_annotations_store.c \
    GUI/calendar_annotations.c tests/test_storage.c
run test_calendar_annotations -IGUI GUI/calendar_annotations.c tests/test_calendar_annotations.c
run test_wqy_ascii -IGUI -DGUI_USE_EXTERNAL_FONTS=0 -DGUI_INCLUDE_INTERNAL_FONTS=1 GUI/fonts.c GUI/u8g2_font.c tests/test_wqy_ascii.c
run test_font_files -IGUI GUI/u8g2_font.c tests/test_font_files.c
run test_external_font_text -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_external_font_text.c
run test_gfx_primitives -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_gfx_primitives.c
run test_gfx_primitives_fixed -DGFX_FIXED_BWR_PANEL=1 -DGFX_FIXED_FONT_DIRECTION_0=1 \
    -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_gfx_primitives.c
run test_u8g2_external_cache -IGUI GUI/u8g2_font.c tests/test_u8g2_external_cache.c
run test_u8g2_placeholder -IGUI GUI/u8g2_font.c tests/test_u8g2_placeholder.c
run test_u8g2_bounds -IGUI GUI/u8g2_font.c tests/test_u8g2_bounds.c
run test_font_store -I. -IEPD -IGUI \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DFONT_STORE_HOST_TEST EPD/font_store.c tests/test_font_store.c
run test_font_store_atomic -I. -IEPD -IGUI \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DFONT_STORE_HOST_TEST EPD/font_store.c tests/test_font_store_atomic.c
run test_font_transfer -Itests/font_transfer_stubs -I. -IEPD -IGUI \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DFONT_TRANSFER_HOST_TEST -DFONT_STORE_MUTABLE=1 EPD/font_transfer.c tests/test_font_transfer.c
run test_fm25q04b_read -Itests/spi_stubs -I. -IEPD \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    EPD/fm25q04b.c tests/test_fm25q04b_read.c
run test_external_font_sources -I. -IEPD -IGUI -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DGUI_USE_EXTERNAL_FONTS=1 -DGUI_INCLUDE_INTERNAL_FONTS=0 GUI/fonts.c tests/test_external_font_sources.c
run test_clock_layout -IGUI tests/test_clock_layout.c
run test_calendar_grid -IGUI tests/test_calendar_grid.c
run test_clock_refresh_window -IGUI tests/test_clock_refresh_window.c
run test_clock_text -IGUI tests/test_clock_text.c
run test_lunar -IGUI GUI/Lunar.c tests/stubs/calendar_resources_stub.c tests/test_lunar.c
run test_clock_window_page -IEPD tests/test_clock_window_page.c
run test_refresh_policy -IEPD -IGUI EPD/epd_refresh_policy.c tests/test_refresh_policy.c
run test_clock_refresh -IEPD -IGUI -Itests/stubs EPD/epd_clock_refresh.c \
    tests/stubs/ui_resources_stub.c tests/test_clock_refresh.c
run test_gui_render -IGUI -IEPD -Itests/stubs -Itools/test/host/shim \
    GUI/GUI.c GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/Lunar.c GUI/calendar_annotations.c tests/stubs/calendar_resources_stub.c tests/stubs/ui_resources_stub.c \
    EPD/epd_clock_refresh.c tests/test_gui_render.c
run test_gui_render_fixed -DGFX_FIXED_BWR_PANEL=1 -DGFX_FIXED_FONT_DIRECTION_0=1 \
    -DGUI_FIXED_PANEL_WIDTH=800 -DGUI_FIXED_PANEL_HEIGHT=480 \
    -IGUI -IEPD -Itests/stubs -Itools/test/host/shim \
    GUI/GUI.c GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/Lunar.c GUI/calendar_annotations.c tests/stubs/calendar_resources_stub.c tests/stubs/ui_resources_stub.c \
    EPD/epd_clock_refresh.c tests/test_gui_render.c
run test_uc8179 -IEPD -Itests/stubs -Itools/test/host/shim EPD/UC8179.c tests/uc8179_command_sequence_test.c

if command -v node >/dev/null 2>&1; then
    node --test tests/test_ble_lifecycle.cjs
    node --test tests/test_resource_web.cjs
    node --test tests/test_calendar_annotations_web.cjs
else
    echo "Node.js unavailable; run node --test tests/test_ble_lifecycle.cjs separately."
fi

python3 tests/test_font_store_tool.py
python3 tests/test_resource_pack.py
python3 tests/test_dfu_release.py
python3 tests/test_service_lifecycle.py

echo "HOST_OK"
