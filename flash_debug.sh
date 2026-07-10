#!/bin/bash
# nRF52 RTT 日志调试烧录脚本 (84KB APP, 仅 RTT，不含 SystemView)
# 用途: 查看完整 NRF_LOG 日志输出（SEGGER RTT Viewer）
# 注意: 与 flash_sysview.sh 互斥，不可同时使用两种调试模式。

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

export PATH="$SCRIPT_DIR/tools/gcc/bin:$SCRIPT_DIR/tools/nrf-tools/bin:$HOME/.local/bin:$PATH"

echo "========================================="
echo " RTT 日志调试模式 (DEBUG_LAYOUT=1, USE_SYSVIEW=0)"
echo "========================================="

echo "1. 重新编译 APP (84KB layout, RTT only)..."
make -f Makefile.nRF52_linux DEBUG_LAYOUT=1 USE_SYSVIEW=0 rebuild

echo "2. 擦除整颗芯片（清除 UICR / 0xFF8 指针）..."
nrfjprog -f nrf52 --eraseall

echo "3. 烧录 S112 SoftDevice..."
nrfjprog -f nrf52 --program SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex --sectorerase

echo "4. 烧录 84KB 调试 Application..."
nrfjprog -f nrf52 --program _build/nrf52811_xxaa.hex --sectorerase

echo "5. 重启设备..."
nrfjprog -f nrf52 --reset

echo "========================================="
echo " RTT 调试烧录成功！(APP 最大可用 84KB)"
echo " 用 SEGGER RTT Viewer 连接查看日志"
echo "========================================="
