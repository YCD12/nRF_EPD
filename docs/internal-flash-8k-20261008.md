# 保留内置 8 KiB 应用数据区域（2026-10-08）

**后续复核：** 本篇占用和哈希记录恢复 8 KiB 时的基线。10 月 9 日完成字库故障重试和构建/发布修复，当前应用为 55,716 B、余 1,628 B；8 KiB 分区及生产 Bootloader 不变。最新验证和配套固件见 [当前 8 KiB 修改复核](current-changes-review-8k-20261009.md)。

**SDK 目录已恢复：** 项目定制现位于 `Bootloader/` 及项目编译参数，SDK 相对仓库 HEAD 无改动。新路径、参数来源和构建验证见 [SDK 恢复说明](sdk-cleanup-20261009.md)，固件字节与迁移前一致。

按用户决定恢复原有 8 KiB 保留区，取消此前 4 KiB 扩容方案。待办功能、字库保存与故障恢复、重复全刷修复、时区校时、Bootloader 验签及增量构建修复全部保留。没有刷写硬件或提交 Git。

## 内置布局

地址范围为左闭右开。

| 区域 | 地址范围 | 容量 |
| --- | --- | ---: |
| MBR / S112 | `0x00000–0x19000` | 100 KiB |
| 应用程序 | `0x19000–0x27000` | 56 KiB |
| 应用数据保留区 | `0x27000–0x29000` | **8 KiB** |
| Bootloader | `0x29000–0x2E000` | 20 KiB |
| MBR 参数 / settings 备份 | `0x2E000–0x2F000` | 4 KiB |
| settings | `0x2F000–0x30000` | 4 KiB |

应用配置保留 `FDS_VIRTUAL_PAGES=2`、`FDS_VIRTUAL_PAGE_SIZE=1024`，对应 8 KiB 的 FDS 配置。业务数据已使用外置 Flash，`FDS_ENABLED=0`、`NRF_FSTORAGE_ENABLED=0` 继续保持；本次恢复区域容量，没有恢复内置 FDS 读写。

Bootloader 的 `NRF_DFU_APP_DATA_AREA_SIZE=8192`，实际编译的 `BLD1` 描述符也记录 8192；DFU 保留两个擦除页。GNU 应用链接脚本、Keil 两个有效应用区域、构建预算和网页 DFU 上限均恢复为 57,344 B。网页不再提供 60 KiB 容量和 4 KiB Bootloader 的扩容提示。

## 实测

| 项目 | 占用 | 可用预算 | 剩余 |
| --- | ---: | ---: | ---: |
| 应用程序 | 55,540 B | 57,344 B | **1,804 B** |
| 应用静态 RAM + 栈保留 | 7,456 + 2,048 B | 15,656 B | 6,152 B |
| Bootloader 程序 | 20,396 B | 20,480 B | 84 B |
| Bootloader 静态 RAM + 栈保留 | 9,704 + 2,048 B | 15,672 B | 3,920 B |

应用 BIN 与恢复前逐字节一致，SHA256 为 `B237895C90D85197C1F6C71523AE77F3799AA2C5FDE19C61C2FE832DF9427B7A`。8 KiB Bootloader HEX SHA256 为 `446541722100B923C25DC4B24B423058F3ACF9C88567345BAF459EEDD69E1C3F`。保留区容量改变不会改变应用代码字节；Bootloader 的保留边界及布局元数据会相应改变。

应用仅余 1,804 B，构建会按既有 4,096 B 余量目标给出警告；这不是越界错误。新增功能前继续检查应用和 Bootloader 实际占用。

## 构建和固件

Bootloader 在仓库根目录执行 `make -f Bootloader/Makefile`，默认输出 `output/bootloader-8k/bl_nrf52811_xxaa_s112_8k.hex`。配套镜像为 `tools/bootloader/bl_nrf52811_xxaa_s112_8k.hex`；旧无后缀镜像保留作历史对照，不作为默认发布镜像。

本轮文件位于 [`output/fds-8k-20261008/release`](../output/fds-8k-20261008/release/)：

- `nrf52811-fds8k-0x3D-full.hex`：SoftDevice、修复后的 8 KiB Bootloader、应用及两份 settings。
- `nrf52811-fds8k-0x3D-ota.zip`：签名应用 OTA 包。
- `nrf52811-fds8k-0x3D-release.json`：布局、签名密钥、资源和产物哈希审计清单。

8 KiB 地址与原布局一致。需要安装修复后的 Bootloader 时使用 SWD full HEX；应用 OTA 不会替换 Bootloader。内置全量刷写前仍应备份旧设备中仅存于 FDS 的配置。当前外置配置、标注、待办、字体和资源 ABI 不变，无需重装默认资源。

## 验证

17 项布局/预算检查、6 项应用 ARM 构建回归、5 项 Bootloader ARM 构建回归、19 个 SDK 验签子用例、26 项发布测试和 270 项网页测试全部通过。协议与资源工具检查也通过。

签名 ZIP 中的应用与 BIN 一致；full HEX 内应用、Bootloader、SoftDevice 和两份 settings 均逐字节匹配输入，且与普通开发合并结果一致。整个 8 KiB 保留区无写入记录，默认 Bootloader 与配套正式镜像一致。

full HEX SHA256：`B212967CF48E63B4D8343D7AFE22C474A53E6C0C0CCB244CF53B07DF79A54659`。

测量、测试和发布字节核对证据位于 [`output/fds-8k-20261008`](../output/fds-8k-20261008/)。此前 4 KiB 输出保留为历史记录，当前构建和发布选择 8 KiB 镜像，并拒绝混用 4 KiB 描述符。硬件 DFU、掉电恢复和栈使用峰值仍需实机验证。
