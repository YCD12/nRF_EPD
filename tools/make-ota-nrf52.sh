#!/bin/bash

# nRF52 OTA包生成脚本 (Linux版本)

# 检查参数
if [ $# -ne 1 ]; then
    echo "Usage: $0 <hex_file_prefix>"
    echo "Example: $0 _build/nrf52811_xxaa"
    exit 1
fi

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 尝试多个可能的工具路径
NRF_TOOLS_PATHS=(
    "$PROJECT_ROOT/gcc/nrf-command-line-tools-10.15.4_linux-amd64/nrf-command-line-tools/bin"
    "$PROJECT_ROOT/tools/bin"
    "$SCRIPT_DIR/bin"
    "/usr/local/bin"
)

# 查找工具路径
NRF_TOOLS_PATH=""
for path in "${NRF_TOOLS_PATHS[@]}"; do
    if [ -f "$path/mergehex" ]; then
        NRF_TOOLS_PATH="$path"
        break
    fi
done

if [ -n "$NRF_TOOLS_PATH" ]; then
    export PATH="$NRF_TOOLS_PATH:$PATH"
    echo "Using nRF tools from: $NRF_TOOLS_PATH"
else
    echo "Warning: nRF tools not found in expected locations"
fi


# 变量设置
FW_VER="0x19"
FW_HEX="${1}.hex"
P_KEY="$SCRIPT_DIR/priv.pem"
BL_HEX="$SCRIPT_DIR/bootloader/bl_nrf52811_xxaa_s112.hex"
SD_HEX="$PROJECT_ROOT/SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex"
SETTINGS="${1}-settings.hex"
FW_FULL_HEX="${1}-full.hex"
OTA_ZIP="${1}-ota.zip"

echo "=== nRF52 OTA Package Generation ==="
echo "Firmware Version: $FW_VER"
echo "Input HEX: $FW_HEX"
echo "Output OTA: $OTA_ZIP"
echo "Output Full: $FW_FULL_HEX"

# 检查必要文件是否存在
if [ ! -f "$FW_HEX" ]; then
    echo "Error: Application hex file not found: $FW_HEX"
    exit 1
fi

if [ ! -f "$P_KEY" ]; then
    echo "Error: Private key file not found: $P_KEY"
    exit 1
fi

if [ ! -f "$BL_HEX" ]; then
    echo "Error: Bootloader hex file not found: $BL_HEX"
    exit 1
fi

if [ ! -f "$SD_HEX" ]; then
    echo "Error: SoftDevice hex file not found: $SD_HEX"
    exit 1
fi

# 检查工具是否可用
if ! command -v nrfutil >/dev/null 2>&1; then
    echo "Error: nrfutil not found. Please install nrfutil."
    exit 1
fi

if ! command -v mergehex >/dev/null 2>&1; then
    echo "Error: mergehex not found. Please install mergehex."
    exit 1
fi

echo ""
echo "Step 1: Generating OTA package..."
nrfutil pkg generate \
    --application "$FW_HEX" \
    --key-file "$P_KEY" \
    --hw-version 52 \
    --sd-req 0x126 \
    --sd-id 0x126 \
    --application-version "$FW_VER" \
    "$OTA_ZIP"

if [ $? -eq 0 ]; then
    echo "✓ OTA package generated: $OTA_ZIP"
else
    echo "✗ Failed to generate OTA package"
    exit 1
fi

echo ""
echo "Step 2: Generating bootloader settings..."
nrfutil settings generate \
    --family NRF52810 \
    --application "$FW_HEX" \
    --softdevice "$SD_HEX" \
    --application-version "$FW_VER" \
    --bootloader-version 1 \
    --bl-settings-version 1 \
    --key-file "$P_KEY" \
    --no-backup \
    "$SETTINGS"

if [ $? -eq 0 ]; then
    echo "✓ Settings generated: $SETTINGS"
else
    echo "✗ Failed to generate settings"
    exit 1
fi

echo ""
echo "Step 3: Merging hex files..."
mergehex \
    -m "$SD_HEX" "$BL_HEX" "$FW_HEX" "$SETTINGS" \
    -o "$FW_FULL_HEX"

if [ $? -eq 0 ]; then
    echo "✓ Full hex generated: $FW_FULL_HEX"
else
    echo "✗ Failed to merge hex files"
    exit 1
fi

echo ""
echo "=== OTA Package Generation Completed ==="
echo "Files generated:"
echo "  - OTA Package: $OTA_ZIP"
echo "  - Settings: $SETTINGS"  
echo "  - Full HEX: $FW_FULL_HEX"
echo "========================================"
