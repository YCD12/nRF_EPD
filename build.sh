#!/bin/bash
# 用法: ./build.sh [clean|build|ota|flash|flash_full|flash_softdevice|erase]
# 不带参数默认执行 ota（编译 + 生成 OTA 包 + 合并 full hex）

CMD="${1:-ota}"
make -f Makefile.nRF52_linux "$CMD"
