#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CPPCHECK_ROOT="${CPPCHECK_ROOT:-$SCRIPT_DIR/../cppcheck}"
CPPCHECK_BUILD_DIR="${CPPCHECK_BUILD_DIR:-$CPPCHECK_ROOT/build}"
CHECK_LEVEL="${CPPCHECK_CHECK_LEVEL:-normal}"
REPORT_FILE="${1:-$SCRIPT_DIR/cppcheck_report.txt}"

if [[ -n "${CPPCHECK_BIN:-}" ]]; then
    BUILD_CPPCHECK="${BUILD_CPPCHECK:-0}"
else
    CPPCHECK_BIN="$CPPCHECK_BUILD_DIR/bin/cppcheck"
    BUILD_CPPCHECK="${BUILD_CPPCHECK:-auto}"
fi

if command -v nproc >/dev/null 2>&1; then
    BUILD_JOBS="${CPPCHECK_BUILD_JOBS:-$(nproc)}"
else
    BUILD_JOBS="${CPPCHECK_BUILD_JOBS:-1}"
fi

if [[ "$REPORT_FILE" != /* ]]; then
    REPORT_FILE="$SCRIPT_DIR/$REPORT_FILE"
fi

if [[ "$BUILD_CPPCHECK" == "auto" ]]; then
    BUILD_PARENT_DIR="$(dirname -- "$CPPCHECK_BUILD_DIR")"
    if [[ -x "$CPPCHECK_BIN" && (( -d "$CPPCHECK_BUILD_DIR" && ! -w "$CPPCHECK_BUILD_DIR" ) || ( ! -d "$CPPCHECK_BUILD_DIR" && ! -w "$BUILD_PARENT_DIR" )) ]]; then
        echo "[1/2] Cppcheck build directory is not writable; using existing binary"
        echo "[1/2] To rebuild in a writable directory, set CPPCHECK_BUILD_DIR"
        BUILD_CPPCHECK=0
    else
        BUILD_CPPCHECK=1
    fi
fi

if [[ "$BUILD_CPPCHECK" == "1" ]]; then
    if [[ ! -f "$CPPCHECK_ROOT/CMakeLists.txt" ]]; then
        echo "Cppcheck source directory not found: $CPPCHECK_ROOT" >&2
        echo "Set CPPCHECK_ROOT to the correct source directory if needed." >&2
        exit 2
    fi

    echo "[1/2] Configuring Cppcheck: $CPPCHECK_ROOT"
    # The dmake target regenerates Cppcheck's source-tree Makefile.  The
    # source tree may be read-only in managed environments, and dmake is not
    # needed to build the CMake CLI binary used by this script.
    cmake -S "$CPPCHECK_ROOT" -B "$CPPCHECK_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DDISABLE_DMAKE=ON

    echo "[1/2] Building Cppcheck with $BUILD_JOBS job(s)"
    cmake --build "$CPPCHECK_BUILD_DIR" --parallel "$BUILD_JOBS"
else
    echo "[1/2] Skipping Cppcheck build (BUILD_CPPCHECK=$BUILD_CPPCHECK)"
fi

if [[ ! -x "$CPPCHECK_BIN" ]]; then
    echo "Cppcheck executable not found or not executable: $CPPCHECK_BIN" >&2
    echo "Set CPPCHECK_BIN or CPPCHECK_ROOT to the correct path if needed." >&2
    exit 2
fi

SDK_ROOT="$SCRIPT_DIR/SDK/17.1.0_ddde560"
GCC_ROOT="$SCRIPT_DIR/tools/gcc"

INCLUDES=(
    "-I$GCC_ROOT/arm-none-eabi/include"
    "-I$GCC_ROOT/lib/gcc/arm-none-eabi/9.3.1/include"
    "-I$SDK_ROOT/components/toolchain/cmsis/include"
    "-I$SDK_ROOT/components/softdevice/common"
    "-I$SDK_ROOT/components/softdevice/s112/headers"
    "-I$SDK_ROOT/components/softdevice/s112/headers/nrf52"
    "-I$SDK_ROOT/components/libraries/atomic"
    "-I$SDK_ROOT/components/libraries/atomic_fifo"
    "-I$SDK_ROOT/components/libraries/atomic_flags"
    "-I$SDK_ROOT/components/libraries/balloc"
    "-I$SDK_ROOT/components/libraries/bootloader"
    "-I$SDK_ROOT/components/libraries/bootloader/ble_dfu"
    "-I$SDK_ROOT/components/libraries/bootloader/dfu"
    "-I$SDK_ROOT/components/libraries/delay"
    "-I$SDK_ROOT/components/libraries/fstorage"
    "-I$SDK_ROOT/components/libraries/fds"
    "-I$SDK_ROOT/components/libraries/experimental_section_vars"
    "-I$SDK_ROOT/components/libraries/log"
    "-I$SDK_ROOT/components/libraries/log/src"
    "-I$SDK_ROOT/components/libraries/memobj"
    "-I$SDK_ROOT/components/libraries/mutex"
    "-I$SDK_ROOT/components/libraries/pwr_mgmt"
    "-I$SDK_ROOT/components/libraries/ringbuf"
    "-I$SDK_ROOT/components/libraries/sortlist"
    "-I$SDK_ROOT/components/libraries/scheduler"
    "-I$SDK_ROOT/components/libraries/strerror"
    "-I$SDK_ROOT/components/libraries/svc"
    "-I$SDK_ROOT/components/libraries/timer"
    "-I$SDK_ROOT/components/libraries/util"
    "-I$SDK_ROOT/components/ble/common"
    "-I$SDK_ROOT/components/ble/ble_advertising"
    "-I$SDK_ROOT/components/ble/nrf_ble_gatt"
    "-I$SDK_ROOT/components/ble/ble_services/ble_dfu"
    "-I$SDK_ROOT/external/fprintf"
    "-I$SDK_ROOT/external/segger_rtt"
    "-I$SDK_ROOT/integration/nrfx"
    "-I$SDK_ROOT/integration/nrfx/legacy"
    "-I$SDK_ROOT/modules/nrfx"
    "-I$SDK_ROOT/modules/nrfx/mdk"
    "-I$SDK_ROOT/modules/nrfx/drivers/include"
    "-I$SDK_ROOT/modules/nrfx/hal"
    "-I$SDK_ROOT"
    "-I$SCRIPT_DIR/EPD/hal"
    "-I$SCRIPT_DIR/EPD/driver"
    "-I$SCRIPT_DIR/EPD/board"
    "-I$SCRIPT_DIR/EPD/storage"
    "-I$SCRIPT_DIR/EPD/app"
    "-I$SCRIPT_DIR/EPD/service"
    "-I$SCRIPT_DIR/GUI"
    "-I$SCRIPT_DIR"
)

DEFINES=(
    '-D__ARM_EABI__'
    '-D__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__'
    '-D__ORDER_BIG_ENDIAN__=4321'
    '-D__ORDER_LITTLE_ENDIAN__=1234'
    '-D__IEEE_LITTLE_ENDIAN'
    '-D__INTPTR_TYPE__=int'
    '-D__UINTPTR_TYPE__=unsigned int'
    '-D__INT8_TYPE__=signed char'
    '-D__UINT8_TYPE__=unsigned char'
    '-D__INT16_TYPE__=short int'
    '-D__UINT16_TYPE__=short unsigned int'
    '-D__INT32_TYPE__=long int'
    '-D__UINT32_TYPE__=long unsigned int'
    '-D__INT64_TYPE__=long long int'
    '-D__UINT64_TYPE__=long long unsigned int'
    -DAPP_TIMER_V2
    -DAPP_TIMER_V2_RTC1_ENABLED
    -DCONFIG_GPIO_AS_PINRESET
    -DFLOAT_ABI_SOFT
    -DNRF52811_XXAA
    -DNRFX_COREDEP_DELAY_US_LOOP_CYCLES=3
    -DNRF_DFU_SVCI_ENABLED
    -DNRF_DFU_TRANSPORT_BLE=1
    -DNRF_SD_BLE_API_VERSION=7
    -DS112
    -DSOFTDEVICE_PRESENT
    -DBLE_LOG_REDIRECT_ALL_NRF_LOG=0
    -DNRF_LOG_ENABLED=0
    -DNRF_STRERROR_ENABLED=0
    -D__HEAP_SIZE=0
    -D__STACK_SIZE=2048
    -D__GNUC__=9
    -D__GNUC_MINOR__=3
    -D__GNUC_PATCHLEVEL__=1
)

SOURCES=(
    "$SCRIPT_DIR/main.c"
    "$SCRIPT_DIR/gcc_sbrk.c"
    "$SCRIPT_DIR/EPD/board/EPD_config.c"
    "$SCRIPT_DIR/EPD/hal/EPD_driver.c"
    "$SCRIPT_DIR/EPD/hal/fm25q04b.c"
    "$SCRIPT_DIR/EPD/driver/UC8179.c"
    "$SCRIPT_DIR/EPD/driver/epd_models.c"
    "$SCRIPT_DIR/EPD/storage/resource_store.c"
    "$SCRIPT_DIR/EPD/storage/resource_crc.c"
    "$SCRIPT_DIR/EPD/storage/resource_crc_flash.c"
    "$SCRIPT_DIR/EPD/storage/persistent_record.c"
    "$SCRIPT_DIR/EPD/storage/font_store.c"
    "$SCRIPT_DIR/EPD/app/epd_refresh_policy.c"
    "$SCRIPT_DIR/EPD/app/epd_clock_refresh.c"
    "$SCRIPT_DIR/EPD/app/epd_resources.c"
    "$SCRIPT_DIR/EPD/app/calendar_annotations_store.c"
    "$SCRIPT_DIR/EPD/app/calendar_annotations_tx.c"
    "$SCRIPT_DIR/EPD/service/EPD_service.c"
    "$SCRIPT_DIR/EPD/service/epd_service_display.c"
    "$SCRIPT_DIR/EPD/service/epd_service_cmd.c"
    "$SCRIPT_DIR/EPD/service/epd_service_control.c"
    "$SCRIPT_DIR/EPD/service/epd_service_transfer.c"
    "$SCRIPT_DIR/EPD/service/epd_advertising.c"
    "$SCRIPT_DIR/EPD/service/resource_protocol.c"
    "$SCRIPT_DIR/EPD/service/resource_transfer.c"
    "$SCRIPT_DIR/EPD/service/epd_rle.c"
    "$SCRIPT_DIR/GUI/GUI.c"
    "$SCRIPT_DIR/GUI/calendar_annotations.c"
    "$SCRIPT_DIR/GUI/Lunar.c"
    "$SCRIPT_DIR/GUI/fonts.c"
    "$SCRIPT_DIR/GUI/Adafruit_GFX.c"
    "$SCRIPT_DIR/GUI/u8g2_font.c"
)

mkdir -p -- "$(dirname -- "$REPORT_FILE")"

echo "[2/2] Starting Cppcheck scan"
echo "[2/2] Check level: $CHECK_LEVEL"
echo "[2/2] Report file: $REPORT_FILE"
echo "[2/2] Sources: ${#SOURCES[@]} file(s), including EPD/service/EPD_service.c"

set +e
"$CPPCHECK_BIN" \
    --enable=all \
    --inconclusive \
    --std=c99 \
    --platform=arm32-wchar_t4 \
    --check-level="$CHECK_LEVEL" \
    --template=gcc \
    "${INCLUDES[@]}" \
    "${DEFINES[@]}" \
    "${SOURCES[@]}" \
    2>&1 | tee "$REPORT_FILE"
PIPE_STATUS=("${PIPESTATUS[@]}")
STATUS="${PIPE_STATUS[0]}"
TEE_STATUS="${PIPE_STATUS[1]}"
set -e

if [[ "$TEE_STATUS" -ne 0 ]]; then
    echo "Failed to write Cppcheck report: $REPORT_FILE" >&2
    exit "$TEE_STATUS"
fi

echo "Cppcheck report saved to: $REPORT_FILE"
echo "Report lines: $(wc -l < "$REPORT_FILE")"

if [[ "$STATUS" -ne 0 ]]; then
    echo "Cppcheck exited with status: $STATUS" >&2
fi

exit "$STATUS"
