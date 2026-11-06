# 撤回 SDK 目录修改（2026-10-09）

按用户要求，`SDK/` 的全部修改已撤回到仓库 HEAD，两个新增文件已移出。`git status --short -- SDK` 无输出，SDK 索引也未改动。恢复的是本仓库已有版本，不声称它等同于未经项目修改的 Nordic 官方发布包。

## 项目定制的位置

| 内容 | 现在的位置 |
| --- | --- |
| Bootloader 主入口及 BLD1 编译布局描述符 | `Bootloader/main.c` |
| 本项目的 InitCommand/ECDSA 验签修复 | `Bootloader/nrf_dfu_validation.c` |
| GCC Bootloader 构建和链接脚本 | `Bootloader/Makefile`、`Bootloader/gcc_bootloader.ld` |
| 应用禁用内部 FDS/fstorage 的配置 | `Makefile.nRF52_linux` 的编译参数；`Keil/EPD-nRF52.uvprojx` 的 C/汇编/预处理参数 |

Keil Bootloader 工程已改为编译项目目录中的主入口和验签实现。原 SDK 中的对应文件保留 HEAD 内容，不参与这个 Bootloader 的编译；其他 SDK 库、Bootloader 配置及现有公钥继续复用。项目源文件保留原许可声明，维护与 SDK 升级注意事项见 [Bootloader 说明](../Bootloader/README.md)。

应用继续使用 `FDS_ENABLED=0`、`NRF_FSTORAGE_ENABLED=0`，项目参数覆盖 SDK 默认值；Bootloader 自己的 fstorage 继续启用，用于 DFU/settings。`FDS_VIRTUAL_PAGES=2`、`FDS_VIRTUAL_PAGE_SIZE=1024`、`NRF_DFU_APP_DATA_AREA_SIZE=8192` 不变，8 KiB 保留区仍为 `0x27000–0x29000`。待办、外置字库、失败退避与资源保存功能保留。

构建入口：

```sh
make -f Bootloader/Makefile
make -f Makefile.nRF52_linux nrf52811_xxaa
```

GCC Bootloader 默认输出到 `output/bootloader-8k/`，配套镜像仍为 `tools/bootloader/bl_nrf52811_xxaa_s112_8k.hex`。现有应用链接脚本继续使用 SDK 内未修改的原文件，56 KiB 上限不变。

## 验证

- 应用 8 项实际 ARM 构建回归通过，包括真实预处理结果和 Keil 各参数来源的 FDS/fstorage 覆盖检查。
- Bootloader 5 项实际 ARM 构建回归通过，包括源文件、参数和新链接脚本的增量依赖。
- 项目验签实现与真实 micro-ecc 后端的 19 个签名/篡改/错误子用例通过（2 个 unittest 方法）。
- 发布工具 28 项测试通过；实际签名 OTA 与 full HEX 生成成功。
- SDK 清洁状态、编译后 BLD1/公钥、默认与配套 Bootloader 镜像一致、ZIP 应用内容、full HEX 输入并集和 8 KiB 保留区均已核对。

应用 BIN、Bootloader HEX 及 full HEX 均与迁移前逐字节一致。

| 项目 | 占用 | 剩余 |
| --- | ---: | ---: |
| 应用 Flash | 55,716 B / 57,344 B | 1,628 B |
| 应用静态 RAM + 栈 | 7,464 + 2,048 B / 15,656 B | 6,144 B |
| Bootloader Flash | 20,396 B / 20,480 B | 84 B |
| Bootloader 静态 RAM + 栈 | 9,704 + 2,048 B / 15,672 B | 3,920 B |

日志与字节核对结果位于 [`output/sdk-cleanup-20261009`](../output/sdk-cleanup-20261009/)；签名包位于该目录的 [`release`](../output/sdk-cleanup-20261009/release/)，前缀为 `nrf52811-sdk-clean-8k-0x3D`。旧输出保留为历史记录。没有刷写硬件或提交 Git；Keil 仅核对项目配置，未运行其编译器。

应用 BIN SHA256：`75D535096A6AFF73D49EA4B95A126701154F09F54DA9A81FBFA354B31FB27574`。

Bootloader HEX SHA256：`446541722100B923C25DC4B24B423058F3ACF9C88567345BAF459EEDD69E1C3F`。

full HEX SHA256：`EE5B7AB7E4E1851337BE35ECE522261F82F028A5266888A47AFBD2E4C4564D30`。
