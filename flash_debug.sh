#!/bin/bash
# nRF52 本地免空间限制调试烧录脚本 (84KB APP)
# 作用: 擦除全芯片 (含 UICR)，编译 84KB 调试 APP 并烧录，不烧录 Bootloader。

set -e

# 获取脚本所在目录，保证在任意目录下执行此脚本都能正确找到路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 导出工具链路径，防止依赖系统全局变量
export PATH="$SCRIPT_DIR/tools/gcc/bin:$SCRIPT_DIR/tools/nrf-tools/bin:$HOME/.local/bin:$PATH"

# 默认开启 SystemView，允许通过参数覆盖
USE_SYSVIEW=1
for arg in "$@"; do
    if [[ "$arg" == "USE_SYSVIEW=0" ]]; then
        USE_SYSVIEW=0
    fi
done

echo "========================================="
# 1. 编译 84KB Debug 布局固件
echo "1. 重新编译 APP (DEBUG_LAYOUT=1, USE_SYSVIEW=${USE_SYSVIEW})..."
make -f Makefile.nRF52_linux DEBUG_LAYOUT=1 USE_SYSVIEW=${USE_SYSVIEW} rebuild

# 2. 全擦除芯片（清除 UICR 中的 Bootloader 地址）
echo "2. 擦除整颗芯片（清除 UICR / 0xFF8 指针）..."
nrfjprog -f nrf52 --eraseall

# 3. 烧录协议栈 S112
echo "3. 烧录 S112 SoftDevice..."
nrfjprog -f nrf52 --program SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex --sectorerase

# 4. 烧录 Application
echo "4. 烧录 84KB 调试 Application..."
nrfjprog -f nrf52 --program _build/nrf52811_xxaa.hex --sectorerase

# 5. 复位启动
echo "5. 重启设备..."
nrfjprog -f nrf52 --reset

echo "========================================="
echo " 调试烧录成功！(APP 最大可用 84KB)"
echo " FDS 已自动向后平移至 0x2E000 - 0x30000"
echo "========================================="
