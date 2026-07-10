#!/bin/bash
# nRF52 SystemView 性能分析烧录脚本 (56KB APP, 标准 layout)
# 用途: 用 SEGGER SystemView 分析任务调度与 CPU 占用
# 注意: 使用标准 56KB linker script，RAM 占用更低，可容纳 SysView 数据结构。
#       与 flash_debug.sh 互斥，不可同时使用两种调试模式。

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

export PATH="$SCRIPT_DIR/tools/gcc/bin:$SCRIPT_DIR/tools/nrf-tools/bin:$HOME/.local/bin:$PATH"

echo "========================================="
echo " SystemView 分析模式 (DEBUG_LAYOUT=0, USE_SYSVIEW=1)"
echo "========================================="

echo "1. 重新编译 APP (56KB layout, SystemView)..."
make -f Makefile.nRF52_linux DEBUG_LAYOUT=0 USE_SYSVIEW=1 rebuild

echo "2. 擦除整颗芯片（清除 UICR / 0xFF8 指针）..."
nrfjprog -f nrf52 --eraseall

echo "3. 烧录 S112 SoftDevice..."
nrfjprog -f nrf52 --program SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex --sectorerase

echo "4. 烧录 56KB SystemView Application..."
nrfjprog -f nrf52 --program _build/nrf52811_xxaa.hex --sectorerase

echo "5. 重启设备..."
nrfjprog -f nrf52 --reset

echo "========================================="
echo " SystemView 烧录成功！(APP 最大可用 56KB)"
echo " 用 SEGGER SystemView 连接查看性能数据"
echo "========================================="
