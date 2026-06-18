#!/bin/bash

# 确保脚本在遇到错误时立即停止
set -e

# 获取脚本所在目录，保证在任意目录下执行此脚本都能正确找到路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "========================================="
echo " 开始通过 J-Link 烧录完整固件..."
echo "========================================="

# 执行 Makefile 中的 flash_full 目标
make -f Makefile.nRF52_linux flash_full

echo "========================================="
echo " 固件烧录及复位成功！"
echo "========================================="
