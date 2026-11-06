#!/bin/bash
# 用法: ./build.sh [clean|build|incremental|test|ota|flash|flash_full|flash_softdevice|erase|rebuild|size|info]
# 不带参数默认执行 build：编译并生成可直接烧录的 full HEX，同时生成全部外置 Flash ERS 包。
# test：只跑主机侧策略/布局单测。build/ota/flash 会先跑同一组测试。

# 确保脚本在遇到错误时立即停止
set -e

# 获取脚本所在目录，保证在任意目录下执行此脚本都能正确找到路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 检查第一个参数是否是特定的 Makefile 便捷目标
TARGET="build"
if [[ "$1" =~ ^(clean|build|incremental|test|erase|flash|flash_full|flash_softdevice|rebuild|ota|size|info)$ ]]; then
    TARGET="$1"
    shift
fi

if [[ "$TARGET" == "test" ]]; then
    TARGET="host_test"
fi

# 固件编译或烧录成功后，生成全部外置 Flash ERS 包。
GENERATE_RESOURCES=0
if [[ "$TARGET" =~ ^(build|incremental|rebuild|ota|flash|flash_full)$ ]]; then
    GENERATE_RESOURCES=1
fi

# incremental 只重编译应用，不清理、不合并 full HEX。
if [[ "$TARGET" == "incremental" ]]; then
    TARGET="nrf52811_xxaa"
fi

# 如果是编译或烧录相关的目标，先执行 clean 以防止不同编译配置（如 RTT/BLE 日志）的缓存干扰
if [[ "$TARGET" =~ ^(flash_full|flash|ota|rebuild|build)$ ]]; then
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

if [[ "$GENERATE_RESOURCES" == 1 ]]; then
    echo "========================================="
    echo " 正在生成外置 Flash 资源包..."
    echo "========================================="
    ./build-external-resources.sh
fi

echo "========================================="
echo " 执行 $TARGET 成功！"
echo "========================================="
