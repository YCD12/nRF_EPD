#!/bin/bash

# 确保脚本在遇到错误时立即停止
set -e

# 获取脚本所在目录，保证在任意目录下执行此脚本都能正确找到路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 检查第一个参数是否是特定的 Makefile 便捷目标
TARGET="flash_full"
if [[ "$1" =~ ^(clean|erase|flash_softdevice|rebuild|ota|size|info)$ ]]; then
    TARGET="$1"
    shift
fi

# 如果是编译或烧录相关的目标，先执行 clean 以防止不同编译配置（如 RTT/BLE 日志）的缓存干扰
if [[ "$TARGET" =~ ^(flash_full|flash|ota|rebuild)$ ]]; then
    echo "========================================="
    echo " 正在清理编译缓存..."
    echo "========================================="
    make -f Makefile.nRF52_linux clean
fi

echo "========================================="
echo " 开始执行 Make Target: $TARGET..."
echo "========================================="

# 执行 Makefile 中的对应目标，并传递剩余的所有参数
make -f Makefile.nRF52_linux "$TARGET" "$@"

echo "========================================="
echo " 执行 $TARGET 成功！"
echo "========================================="
