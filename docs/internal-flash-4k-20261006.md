# 内置应用数据保留区调整为 4 KiB

**历史方案，已停用：** 用户随后决定继续保留 8 KiB。当前布局、构建和配套固件见 [8 KiB 保留区说明](internal-flash-8k-20261008.md)，本文仅保留此前调整与验证记录。

本文记录 10 月 6 日的布局调整与测量。后续待办、字库、构建和启动校验修正及最新发布文件见 [10 月 8 日复核](current-changes-review-20261008.md)；首次升级要求和内置地址不变。

当前应用的配置、日期标注、待办和字库均使用外置 Flash，已不再初始化 Nordic FDS。因此可将 Bootloader 保护的内置应用数据区域从 8 KiB 缩至 4 KiB，给应用增加一个擦除页。真正运行的 Nordic FDS 至少需要两个虚拟页，不能通过将 `FDS_VIRTUAL_PAGES` 改为 1 来实现本次调整。

## 分区与实测

地址范围为左闭右开，nRF52811 总容量 192 KiB。

| 区域 | 调整前 | 调整后 |
| --- | --- | --- |
| MBR / S112 SoftDevice | `0x00000–0x19000` | 不变，100 KiB |
| 应用 | `0x19000–0x27000`，56 KiB | `0x19000–0x28000`，60 KiB |
| 内置应用数据保留区 | `0x27000–0x29000`，8 KiB | `0x28000–0x29000`，4 KiB |
| Bootloader | `0x29000–0x2E000`，20 KiB | 不变 |
| MBR 参数 / settings 备份 | `0x2E000–0x2F000` | 不变 |
| settings | `0x2F000–0x30000` | 不变 |

发布应用实测 **55,508 B**，可用容量 **61,440 B**，余量 **5,932 B**（原为 1,836 B）。静态 RAM 为 7,456 B，另预留 2,048 B 栈，均未增加。BIN SHA256 与上次优化版本一致：`23D2BE4B5EC65B3CED7D2CEB248E06712D7B202B9C9A9322E90F79EEA92AE471`。本次增加容量，没有减少应用程序字节。

配套 GNU Bootloader 占 **20,380 B / 20,480 B**，剩余 **100 B**，没有移动 Bootloader 或 settings 地址。签名验证恢复后通过 LTO、已使用曲线的源码编译、禁用跳转表以及 C 程序启动路径减少体积；SDK 启动代码仍初始化 `.data`、清零 BSS 并调用 `SystemInit`，再进入 `main`。

## 同步修改的入口

- GNU 生产应用链接脚本和 Keil 两个目标的有效代码区域上限均为 `0x28000`；预算、构建占用显示和网页 DFU 容量检查同步为 60 KiB。GNU `flash_debug.sh` 仍为擦片后单独运行、不带 Bootloader 的历史 84 KiB 调试布局，不适用于本发布布局。
- 应用 `FDS_ENABLED`、`NRF_FSTORAGE_ENABLED` 关闭；未启用的 FDS 页数仍保持合法的 2，最终 ELF 无 `fds_*`、`nrf_fstorage_*` 或 `sd_flash_*` 函数。
- Bootloader 的 `NRF_DFU_APP_DATA_AREA_SIZE` 改为 4096。SDK 的 `DFU_REGION_END` 因此由 `0x27000` 变为 `0x28000`。
- Bootloader 编译时嵌入 32 B `BLD1` 描述符，记录应用、保留区、settings 边界及 DFU 配置。发布工具读取实际 HEX 并核对，避免只改配置却继续使用旧 Bootloader。
- 普通 `build` 的 full HEX 合并和签名 OTA 发布均执行布局及 UICR 地址审计。正式发布还检查签名私钥对应的公钥是否在实际 Bootloader HEX 中。默认优先采用 Keil 构建输出，其次 GCC 的 `output/bootloader-4k` 输出，最后配套的 `tools/bootloader/bl_nrf52811_xxaa_s112_4k.hex`。旧构建输出若缺少正确描述符会报错，需要重编译或显式指定新镜像。
- 网页允许 60 KiB 应用，超过旧 56 KiB 上限时提示需要新 Bootloader；设备端最终检查仍由 Bootloader 执行。

原来 `tools/bootloader/bl_nrf52811_xxaa_s112.hex` 保留用于旧版本对照，当前布局不再将它作为默认镜像。

配套 Bootloader 的检查还发现历史源码注释了签名校验和哈希路径的 crypto 初始化；本轮恢复这两处调用，使行为与 `NRF_DFU_REQUIRE_SIGNED_APP_UPDATE=1` 一致。沿用 `tools/priv.pem` 对应的现有公钥，不更换签名密钥。GNU 构建仅保留已使用的 P-256 曲线，沿用 SDK 的 micro-ecc 实现。

## 构建与首次升级

已有配套 Bootloader HEX 时，应用仍用现有 `build.bat build` / `build.bat ota` 或 GNU 入口构建。重编译 Bootloader 可在仓库根目录执行：

```bash
make -f SDK/17.1.0_ddde560/dfu/Makefile
```

输出位于 `output/bootloader-4k`。Windows 可使用仓库配置的 Cygwin make 和 ARM GCC；工具链路径也可通过 `GNU_INSTALL_ROOT` 显式指定。Keil Bootloader 工程沿用同一份配置和 `main.c`，但本轮实编验证使用 GNU GCC。

首次切换时，通过 SWD 安装配套的 **full HEX**，其中包含 SoftDevice、新 Bootloader、应用和两份 settings。生成文件不等于已经安装；本轮没有连接或刷写硬件。安装成功后，后续应用可按现有网页 Secure DFU 流程 OTA 更新。

**应用 OTA 包只更新应用，不会替换旧 Bootloader。** 当前应用 55,508 B 仍能装进旧 56 KiB 范围，但超过 57,344 B 的后续固件必须先安装新 Bootloader。不能以本次小镜像 OTA 成功，认定设备已经支持 60 KiB。

此布局适用于已经完成外置持久化的当前固件。旧设备若仍有仅存于内置 FDS 的配置或日期标注，先按 [P4 迁移说明](external-resources-p4-20260916.md) 导出并保存到外置 Flash，再进行内部全量刷写。当前外置分区和资源 ABI 不变，无需重装默认资源覆盖个人事项。

## 验证记录

- 应用在 60 KiB 生产区域构建成功，程序 BIN 与上次版本完全一致。
- 预算及真实 GNU 链接边界检查共 17 项通过，包含恰好 61,440 B、超限 1 B、`.data` 加入 Flash 后的边界和 Keil XML 一致性。
- 16 项发布测试通过，覆盖编译后的 4/8 KiB 描述符差异、缺失/重复/不完整描述符、冲突 HEX 记录、错误或不完整 UICR、签名公钥匹配、保留页保护及普通构建入口审计。
- 网页 11 项边界测试通过，覆盖 56 KiB、56 KiB + 1、60 KiB、超 60 KiB、组合包缓存提示及重新选包后的提示清除。
- Keil 调试脚本在编译失败时停止，防止继续擦片并使用上次 HEX；5 项模拟返回码检查通过，未调用真实烧录工具。
- 正式签名 OTA ZIP、settings 和 full HEX 已生成。ZIP 内应用与 BIN 一致，full HEX 内应用也一致；普通开发合并与正式发布得到相同 full HEX，保留页无写入记录，Bootloader 包含配置的公钥。
- Bootloader 默认 GCC 构建入口通过，输出 HEX 与配套镜像一致。使用实际 OTA InitCommand 和公钥的宿主 micro-ecc 验签通过；篡改签名、指令、公钥及全零签名/公钥共 5 个负例均被拒绝。反汇编确认签名失败返回 `INVALID_OBJECT`，验签路径实际保留。Bootloader 静态 RAM 9,704 B，加 2,048 B 栈后还剩 3,920 B。

首次 SWD 安装文件：`output/fds-4k-20261006/release/nrf52811-fds4k-0x3D-full.hex`；配套应用 OTA：同目录 `nrf52811-fds4k-0x3D-ota.zip`；审计清单：同目录 `nrf52811-fds4k-0x3D-release.json`。本轮 full HEX SHA256 为 `27CF3C832842EF2EEC448F15309339F9CC91EA390747CF71123E928C2A26B956`。

构建及测量证据位于 `output/fds-4k-20261006`。此前的 8 KiB 清单保留为历史记录，当前分区以本文和实际链接配置为准。
