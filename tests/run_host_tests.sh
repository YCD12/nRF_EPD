#!/usr/bin/env bash
# Host-side policy/layout tests. These do not need the ARM toolchain.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${HOST_TEST_OUT:-_build/host_tests}"
CC="${HOST_CC:-gcc}"
CFLAGS="${HOST_CFLAGS:--Wall -Werror -std=c11}"
mkdir -p "$OUT"

# Some checks use tempfile.TemporaryDirectory(ignore_cleanup_errors=...), which
# needs Python >= 3.10. Under Cygwin, /usr/bin/python3 can be older than the
# Windows Python on PATH, so probe candidates instead of assuming python3.
python_ok() {
    "$@" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' \
        >/dev/null 2>&1
}
PYTHON="${HOST_PYTHON:-}"
if [ -n "$PYTHON" ] && ! python_ok $(printf '%s\n' "$PYTHON"); then
    echo "Error: HOST_PYTHON ($PYTHON) must be Python >= 3.10." >&2
    exit 1
fi
if [ -z "$PYTHON" ]; then
    for candidate in python3.14 python3.13 python3.12 python3.11 python3.10 python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 && python_ok "$candidate"; then
            PYTHON="$candidate"
            break
        fi
    done
fi
if [ -z "$PYTHON" ] && command -v py >/dev/null 2>&1 && python_ok py -3; then
    PYTHON="py -3"
fi
if [ -z "$PYTHON" ]; then
    echo "Error: host tests need Python >= 3.10 (python3 on PATH is older)." >&2
    echo "       Set HOST_PYTHON to a suitable interpreter, e.g." >&2
    echo "       HOST_PYTHON='py -3' build.bat test" >&2
    exit 1
fi
echo "Host Python: $($PYTHON --version 2>&1)"

run() {
    local name="$1"
    shift
    echo "=== ${name} ==="
    # shellcheck disable=SC2086
    ${CC} ${CFLAGS} "$@" -o "${OUT}/${name}"
    "${OUT}/${name}"
}

run test_cmd_policy -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI tests/test_cmd_policy.c
run test_calendar_resources -DCALENDAR_RESOURCES_HOST_TEST  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI \
    GUI/calendar_resources.c GUI/Lunar.c tests/test_calendar_resources.c
run test_ui_resources -DUI_RESOURCES_HOST_TEST  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI \
    GUI/ui_resources.c tests/test_ui_resources.c
run test_resource_store -Itests/font_transfer_stubs -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    EPD/storage/resource_store.c EPD/storage/resource_crc.c EPD/storage/resource_crc_flash.c EPD/storage/persistent_record.c EPD/service/resource_protocol.c EPD/service/resource_transfer.c tests/test_resource_store.c
run test_resource_erase -Itests/font_transfer_stubs -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    EPD/storage/resource_store.c EPD/storage/resource_crc.c EPD/storage/resource_crc_flash.c EPD/storage/persistent_record.c EPD/service/resource_protocol.c EPD/service/resource_transfer.c tests/test_resource_erase.c
run test_advertising -DSVCALL_AS_NORMAL_FUNCTION -DS112 \
    -Itests/advertising_stubs -Itests/storage_stubs  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/ble/common \
    -ISDK/17.1.0_ddde560/components/ble/ble_advertising \
    SDK/17.1.0_ddde560/components/ble/common/ble_advdata.c \
    SDK/17.1.0_ddde560/components/ble/ble_advertising/ble_advertising.c \
    EPD/service/epd_advertising.c tests/test_advertising.c
run test_command_queue -Itests/font_transfer_stubs  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service tests/test_command_queue.c
run test_offline  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service tests/test_offline.c
run test_async_queues -Itests/font_transfer_stubs  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI \
    -DCALENDAR_ANNOTATIONS_USE_NRF_CRITICAL=1 \
    EPD/app/calendar_annotations_tx.c GUI/calendar_annotations.c tests/test_async_queues.c
run test_storage -Itests/storage_stubs -Itests/font_transfer_stubs -Itests/stubs -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI \
    EPD/board/EPD_config.c EPD/app/calendar_annotations_store.c \
    GUI/calendar_annotations.c tests/test_storage.c
run test_calendar_annotations -IGUI GUI/calendar_annotations.c tests/test_calendar_annotations.c
run test_wqy_ascii -IGUI -DGUI_USE_EXTERNAL_FONTS=0 -DGUI_INCLUDE_INTERNAL_FONTS=1 GUI/fonts.c GUI/font_geometry.c GUI/u8g2_font.c tests/test_wqy_ascii.c
run test_font_files -IGUI GUI/u8g2_font.c tests/test_font_files.c
run test_external_font_text -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_external_font_text.c
run test_gfx_primitives -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_gfx_primitives.c
run test_gfx_primitives_fixed -DGFX_FIXED_BWR_PANEL=1 -DGFX_FIXED_FONT_DIRECTION_0=1 \
    -IGUI GUI/Adafruit_GFX.c GUI/u8g2_font.c tests/test_gfx_primitives.c
run test_u8g2_external_cache -IGUI GUI/u8g2_font.c tests/test_u8g2_external_cache.c
run test_u8g2_placeholder -IGUI GUI/u8g2_font.c tests/test_u8g2_placeholder.c
run test_u8g2_bounds -IGUI GUI/u8g2_font.c tests/test_u8g2_bounds.c
run test_font_store -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DFONT_STORE_HOST_TEST EPD/storage/font_store.c GUI/font_geometry.c EPD/storage/resource_crc.c EPD/storage/resource_crc_flash.c tests/test_font_store.c
run test_fm25q04b_read -Itests/spi_stubs -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    EPD/hal/fm25q04b.c tests/test_fm25q04b_read.c
run test_external_font_sources -I.  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    -DGUI_USE_EXTERNAL_FONTS=1 -DGUI_INCLUDE_INTERNAL_FONTS=0 GUI/fonts.c GUI/font_geometry.c tests/test_external_font_sources.c
run test_clock_layout -IGUI tests/test_clock_layout.c
run test_todo_items  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI GUI/todo_items.c GUI/font_geometry.c tests/stubs/ui_resources_stub.c tests/test_todo_items.c
run test_calendar_grid -IGUI tests/test_calendar_grid.c
run test_clock_refresh_window -IGUI tests/test_clock_refresh_window.c
run test_clock_text -IGUI tests/test_clock_text.c
run test_lunar -IGUI GUI/Lunar.c tests/stubs/calendar_resources_stub.c tests/test_lunar.c
run test_clock_window_page  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service tests/test_clock_window_page.c
run test_refresh_policy  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI EPD/app/epd_refresh_policy.c tests/test_refresh_policy.c
run test_clock_refresh  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI -Itests/stubs EPD/app/epd_clock_refresh.c \
    tests/stubs/ui_resources_stub.c tests/test_clock_refresh.c
run test_gui_render -IGUI  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -Itests/stubs -Itools/test/host/shim \
    GUI/GUI.c GUI/todo_items.c GUI/font_geometry.c tests/stubs/todo_resources_stub.c GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/Lunar.c GUI/calendar_annotations.c tests/stubs/calendar_resources_stub.c tests/stubs/ui_resources_stub.c \
    EPD/app/epd_clock_refresh.c tests/test_gui_render.c
run test_gui_render_fixed -DGFX_FIXED_BWR_PANEL=1 -DGFX_FIXED_FONT_DIRECTION_0=1 \
    -DGUI_FIXED_PANEL_WIDTH=800 -DGUI_FIXED_PANEL_HEIGHT=480 \
    -IGUI  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -Itests/stubs -Itools/test/host/shim \
    GUI/GUI.c GUI/todo_items.c GUI/font_geometry.c tests/stubs/todo_resources_stub.c GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/Lunar.c GUI/calendar_annotations.c tests/stubs/calendar_resources_stub.c tests/stubs/ui_resources_stub.c \
    EPD/app/epd_clock_refresh.c tests/test_gui_render.c
run test_uc8179  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -Itests/stubs -Itools/test/host/shim EPD/driver/UC8179.c tests/uc8179_command_sequence_test.c
run test_rle -I. EPD/service/epd_rle.c tests/test_rle.c
run test_review_resource_reply_wedge -Itests/font_transfer_stubs  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
    -ISDK/17.1.0_ddde560/components/libraries/util \
    -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
    EPD/service/resource_transfer.c EPD/storage/resource_crc.c tests/test_review_resource_reply_wedge.c
run test_review_clock_partial_eligibility -DUI_RESOURCES_HOST_TEST  -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI -Itests/stubs \
    EPD/app/epd_clock_refresh.c EPD/app/epd_refresh_policy.c \
    tests/stubs/ui_resources_stub.c tests/test_review_clock_partial_eligibility.c

if command -v node >/dev/null 2>&1; then
    node --test tests/test_dfu_layout.cjs
    node --test tests/test_dfu_resume.cjs
    node --test tests/test_dfu_transport.cjs
    node --test tests/test_ble_lifecycle.cjs
    node --test tests/test_resource_erase.cjs
    node --test tests/test_resource_web.cjs
    node --test tests/test_resource_timing.cjs
    node --test tests/test_site_statistics.cjs
    node --test tests/test_resource_builder.cjs
    node --test tests/test_resource_editor.cjs
    node --test tests/test_resource_draft.cjs
    node --test tests/test_resource_recovery.cjs
    node --test tests/test_ui_tools.cjs
    node --test tests/test_ui_model.cjs
    node --test tests/test_font_builder.cjs
    node --test tests/test_font_sources.cjs
    node --test tests/test_font_sync.cjs
    node --test tests/test_todo_calendar_web.cjs
    node --test tests/test_todo_custom_fonts.cjs
    node --test tests/test_todo_sync.cjs
    node --test tests/test_paint_tools.cjs
    node --test tests/test_paint_schedule.cjs
    node --test tests/test_image_workspace.cjs
    node --test tests/test_review_bwr_dither.cjs
    node --test tests/test_rle.cjs
    node --test tests/test_rle_transfer.cjs
    node --test tests/test_calendar_annotations_web.cjs
    node --test tests/test_review_font_sync_busy.cjs
else
    echo "Node.js unavailable; run node --test tests/test_ble_lifecycle.cjs separately."
fi

$PYTHON tests/test_font_store_tool.py
$PYTHON tests/test_protocol_sync.py
$PYTHON tests/test_build_config_parity.py
$PYTHON tests/test_irq_handler_check.py
$PYTHON tests/test_resource_pack.py
$PYTHON tools/export_web_template.py --check
$PYTHON tools/ui_resource_codec.py --check
$PYTHON tests/test_dfu_release.py
$PYTHON tests/test_service_lifecycle.py
$PYTHON tests/test_clock_timekeeping.py
$PYTHON tests/test_rtc_timer_lifecycle.py
$PYTHON tests/test_adc_voltage.py
$PYTHON tests/test_resources_reload.py
$PYTHON tests/test_dynamic_font.py
$PYTHON tests/test_gui_layout.py
$PYTHON tests/test_ui_render.py
$PYTHON tests/test_todo_integration.py
$PYTHON tests/test_todo_service_pipeline.py

echo "HOST_OK"
