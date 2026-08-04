#!/bin/bash
# nRF52 调试烧录脚本 (84KB APP, 启用 RTT 日志，不含 SystemView)
# 用途: 通过 J-Link RTT Viewer 查看完整 NRF_LOG。
# 注意: 与 flash_sysview.sh 互斥，不可同时使用两种调试模式。

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

export PATH="$SCRIPT_DIR/tools/gcc/bin:$SCRIPT_DIR/tools/nrf-tools/bin:$HOME/.local/bin:$PATH"

echo "========================================="
echo " RTT 日志调试模式 (DEBUG_LAYOUT=1, USE_SYSVIEW=0, USE_BLE_LOG=0)"
echo "========================================="

echo "1. 重新编译 APP (84KB layout, RTT log enabled)..."
# USE_BLE_LOG=1 会将所有 NRF_LOG_* 重定义为 BLE 通知，RTT 只能看到直接
# SEGGER_RTT 输出；调试时必须关闭该重定向，才能将标准日志送到 RTT 后端。
make -f Makefile.nRF52_linux DEBUG_LAYOUT=1 USE_SYSVIEW=0 USE_BLE_LOG=0 rebuild

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
echo " 请先连接 J-Link RTT Viewer 的 Terminal 0，再执行一次 reset 以查看启动日志"
echo "========================================="
