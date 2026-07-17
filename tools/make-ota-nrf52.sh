#!/bin/bash
# nRF52 OTA 包生成脚本（Linux）
# 用法: ./tools/make-ota-nrf52.sh _build/nrf52811_xxaa
# 生成: <prefix>-ota.zip / <prefix>-settings.hex / <prefix>-full.hex

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_prefix>"
    echo "  e.g. $0 _build/nrf52811_xxaa"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 工具路径（tools/nrf-tools 为 symlink 指向打包的工具）
export PATH="$SCRIPT_DIR/nrf-tools/bin:$HOME/.local/bin:$PATH"

FW_VER="0x19"
FW_HEX="${1}.hex"
P_KEY="$SCRIPT_DIR/priv.pem"
BL_HEX="$SCRIPT_DIR/bootloader/bl_nrf52811_xxaa_s112.hex"
SD_HEX="$PROJECT_ROOT/SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex"
SETTINGS="${1}-settings.hex"
FW_FULL_HEX="${1}-full.hex"
OTA_ZIP="${1}-ota.zip"

echo "=== nRF52 OTA Package Generation ==="
echo "  Firmware HEX : $FW_HEX"
echo "  Bootloader   : $BL_HEX"
echo "  SoftDevice   : $SD_HEX"
echo "  Private key  : $P_KEY"
echo "  FW version   : $FW_VER"
echo ""

# 检查必要文件
for f in "$FW_HEX" "$P_KEY" "$BL_HEX" "$SD_HEX"; do
    [ -f "$f" ] || { echo "Error: file not found: $f"; exit 1; }
done
command -v nrfutil  >/dev/null 2>&1 || {
    echo "Error: nrfutil not found (looked in: $SCRIPT_DIR/nrf-tools/bin, \$HOME/.local/bin, \$PATH)"
    echo "  Install with: pip install nrfutil==6.1.7"
    exit 1
}
command -v mergehex >/dev/null 2>&1 || {
    echo "Error: mergehex not found (provided by nRF Command Line Tools)"
    echo "  Ensure tools/nrf-tools/bin is populated, or install nRF Command Line Tools."
    exit 1
}

# Step 1: 生成 OTA zip
echo "Step 1: Generating OTA package..."
nrfutil pkg generate \
    --application  "$FW_HEX" \
    --key-file     "$P_KEY" \
    --hw-version   52 \
    --sd-req       0x126 \
    --sd-id        0x126 \
    --application-version "$FW_VER" \
    "$OTA_ZIP"
echo "  -> $OTA_ZIP"

# Step 2: 生成 bootloader settings
echo "Step 2: Generating bootloader settings..."
nrfutil settings generate \
    --family              NRF52810 \
    --application         "$FW_HEX" \
    --softdevice          "$SD_HEX" \
    --application-version "$FW_VER" \
    --bootloader-version  1 \
    --bl-settings-version 1 \
    --key-file            "$P_KEY" \
    --no-backup \
    "$SETTINGS"
echo "  -> $SETTINGS"

# Step 3: 合并 SoftDevice + Bootloader + App + Settings → full hex
echo "Step 3: Merging hex files..."
mergehex -m "$SD_HEX" "$BL_HEX" "$FW_HEX" "$SETTINGS" -o "$FW_FULL_HEX"
echo "  -> $FW_FULL_HEX"

echo ""
echo "=== Done ==="
echo "  OTA zip  : $OTA_ZIP"
echo "  Settings : $SETTINGS"
echo "  Full HEX : $FW_FULL_HEX"
