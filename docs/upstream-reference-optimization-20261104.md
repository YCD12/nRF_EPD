# 参考上游 EPD-nRF5 的对比分析与优化（2026-11-04）

本轮以 `D:\Desk\hema\EPD-nRF5`（[tsl0922/EPD-nRF5](https://github.com/tsl0922/EPD-nRF5)
上游 `main`，提交 `7e31961`，含工作树未提交改动）为参照，对当前工程
`he-ma_n-rf_-epd` 的固件驱动层、系统/BLE/电源层、网页/GUI 层做逐项对比，并实施
了风险可控的改进。三份逐层分析报告位于：

- `output/work-20261104/ref-compare/driver.md`（驱动层，206 行）
- `output/work-20261104/ref-compare/system.md`（系统/BLE/电源层，101 行）
- `output/work-20261104/ref-compare/web.md`（网页/GUI 层，168 行）

本文件是结论汇总、实施记录与后续路线，不重复上述报告的全部证据表格。

## 1. 基线

| 项目 | 值 |
| --- | --- |
| 工程 | nRF52811 + S112 7.3.0，UC8179 800×480 BWR，应用区 `0x19000–0x27000` |
| 构建 | GCC 9.3.1 `-Os -g3 -flto -flto-partition=one`，固定面板，外置字体，日志关闭 |
| 基线 BIN / 余量 | 55,004 B / 57,344 B（余 2,340 B） |
| 基线静态 RAM + 栈 | 9,204 B + 2,048 B（15,656 B 预算，余 4,404 B） |
| 基线提交 | `ce07cd6`（`dev` 分支） |

## 2. 结论摘要

1. **当前工程在三个层面整体强于上游 `main`**，不是"落后于上游"的形态：
   - 驱动层：真 OLD/NEW 硬件局刷（寄存器 LUT + DTM1/DTM2 条带）、BUSY 超时/打断、
     ADC 有界等待、GPIO 引用计数保护、pin 重映射、局刷窗口严格校验、外置记录式
     配置持久化、刷新策略（夜间静默/周期全刷/低电保持）等 10 项均强于上游
     （`driver.md` §1.3）。
   - 系统层：命令队列 + 主循环状态机、断连清理移出 BLE 中断、RTC tick + ppm
     校准、通知/配置写回执、GCC/Keil 双构建与 99 项主机测试等
     （`system.md` 一、18 项逐条对比）。
   - 网页/GUI 层：现代网页是上游的超集，GUI 的上游可见特性已全部覆盖且节日/休班
     已外置到可编辑资源（`web.md` §2.2/§2.3）。
2. **上游真正值得借鉴的只有少数几项**：
   - **BWR 三色抖动分类**：上游 `a563890` 用"调色板最近色"替换红色阈值判定；
     当前现代页仍是阈值判定，legacy 页还缺"红优先互斥"（会出现黑+红双平面
     同时激活）。→ 本轮已修（§3.3）。
   - **图片刷新完成后 SLEEP**：上游工作树刚加 `REFRESH` 后发 `SLEEP`；当前网页
     定义了 `SLEEP` 但从不发送，图片模式保持连接时面板不睡眠。→ 本轮已修（§3.2）。
   - **RLE 压缩传图**（上游 `29e511a`）：唯一有产品意义的上游缺功能；当前
     `WRITE_BLOCK` 协议与上游 `WRITE_IMAGE` 不同，不能照搬。本轮只做设计评估，
     未实施（§5.3）。
   - **电池图标/百分比**：上游 GUI 有 `DrawBattery` + `batt_cal`；当前只有
     `{voltage}` 文本 token。属于展示增强，未实施（§5.5）。
   - 多面板/在线切屏、原始命令透传、内置 FDS、`feature/ble_ack` 等明确不建议
     回退或移植（各报告"明确不建议"节）。
3. **本轮同时发现当前工程自身的实时性/发布风险**（与上游无关，但对比过程暴露）：
   - `WRITE_BLOCK` 首块在 SoftDevice 事件中断里做图片模式切换/面板 init，
     可能占中断 1–2 s（`system.md` S-1，`docs/clock-drift-20261023.md` 已记录）；
   - Bootloader 独立源码缺失，`Keil/DFU-nRF52.uvprojx` 引用不存在的
     `..\Bootloader\`（`system.md` S-2，已知 P1）；
   - 0xA0/0xA1 状态通知单次投递，HVX 满即丢（`system.md` S-3）；
   - 看门狗实际超时 60 s，但 `main.c` 注释写 32 s（`system.md` S-5，本轮已改注释）；
   - `sdk_config.h` 的 FDS/fstorage/strerror 仍为 1，仅靠 Makefile/Keil 宏覆盖
     （`system.md` S-6）。因项目政策是 **SDK 目录保持仓库原样**
     （`docs/sdk-cleanup-20261009.md`），本轮不改 SDK；需要由构建脚本/测试锁定。

## 3. 本轮已实施

### 3.1 删除 `EPD_SPI_Write` 无调用者的 Flash→RAM 拷贝分支（C-01）

- `EPD/EPD_driver.c`：原实现按指针地址 `< 0x20000000` 判断"可能在 Flash"，
  再 `memcpy` 到 128 B 栈缓冲分段发送；全仓复核所有调用点（`EPD_WriteCmd` 的
  `&cmd`、`EPD_Write` 宏栈数组、`EPD_FillRAM` 栈缓冲、UC8179 LUT/栈/GFX 页、
  BLE payload）都在 RAM，分支不可达。现改为单次 `nrf_drv_spi_transfer`，并注释
  约定"所有 SPI 发送 buffer 必须在 RAM（EasyDMA 限制）"。
- 实测：`EPD_SPI_Write` 168 B → **96 B**（−72 B），函数入口不再无条件
  `sub sp, #148`，栈帧同步缩小（不需要 2 KiB 栈预算变更，仅降低峰值占用风险）。

### 3.2 `UC8179_Sleep` 去掉冗余 PowerOff（C-02）

- `EPD/UC8179.c`：所有会开高压（PON）的路径（`UC8179_Refresh`、
  `UC8179_Refresh_Partial`，含 PON 失败、DRF 超时、传图打断）返回前都已 POF；
  `Init/Init_Partial/Clear/Write_Image` 从不开高压。因此 `UC8179_Sleep` 不再重复
  `PowerOff`，只保留 `delay(100) + DSLP(0xA5)`，并注释该不变式。
- 收益：每次时钟/待办刷新（走 `drv->sleep` 的主路径）少一次 POF 命令 +
  一次 `delay(100)`（约 100 ms/次）；实测 Flash 36 B → 32 B（−4 B）。
- 防回归：`tests/uc8179_command_sequence_test.c` 新增 "Refresh→Sleep 命令序列
  中 POF 恰好 1 次、Sleep 只发 DSLP 0xA5" 断言；负向验证（把 PowerOff 塞回）
  该用例会失败。
- 风险：该结论依赖 POF 不变式；将来新增"上电后不 POF 直接 sleep"的路径必须
  同步恢复 PowerOff。

### 3.3 BWR 三色抖动分类修正（参考上游 `a563890`）

- `html/js/dithering.js`：三色不再使用独立的 `红阈值(160)/亮度阈值(140)` 判定，
  改为与四色一致的 Lab 调色板最近色（黑/白/红）；`findClosestColor()` 与
  `processImageData()` 使用同一分类函数，**预览像素与实际发送平面同源**；
  红像素强制黑/白平面写白（bit=1）、红平面写 0，两平面永不同时激活。
- `html/legacy/js/dithering.js`：保留旧页面原分类阈值（服务旧固件），但
  `processImageData` 复用 `findClosestColor` 并补"红优先互斥"，修复旧实现里
  亮红 `(200,0,0)` 同时写黑平面和红平面的问题。
- 新测试 `tests/test_review_bwr_dither.cjs`（6 项）：暗红 `(150,20,20)` 判红、
  灰/亮灰/暗灰落黑白、纯色精确匹配、modern/legacy 全像素双平面互斥、
  `decodeProcessedData` 预览与 `processImageData` 平面逐字节一致（modern 覆盖
  none/Floyd/Atkinson/Bayer 四种算法）。
- 已接入 `tests/run_host_tests.sh` 的 Node 测试列表。

### 3.4 四色发送拦截（网页真 bug）

- `html/js/main.js`：`sendImageOperation` 原来只拦截 `sixColor`，`fourColor`
  会把 2bpp 四色数据当作单色层发给只有 UC8179 BWR 的固件，必然错色。
  现在四色与六色同样在设备协商/任何传输前拦截并提示，预览/数组导出保留。
- `tests/test_image_workspace.cjs`：six/fourColor 参数化用例，断言不进入
  `waitForNegotiatedMtu`、输出对应提示。
- `html/index.html`：四色/六色颜色模式下拉标注“仅预览”，与实际可发送能力一致。

### 3.5 图片刷新完成后自动 SLEEP（参考上游工作树改动）

- `html/js/main.js`：新固件（有 `0xAB` 刷新确认）在 `REFRESH` 确认 `result=0`、
  仍连接且 owner 有效时，完成日志/计时后发送 `EpdCmd.SLEEP`（`[0x04]`，无响应
  等待）；失败只记日志，不改变"图片刷新完成"结果；旧固件/断连/刷新失败不发。
- `tests/test_ble_lifecycle.cjs`：新增 5 条用例——成功后恰好 1 条 0x04、刷新失败
  不发、确认期间断连不发、SLEEP 写入抛错仍保留完成结果、旧固件只发 0x05 REFRESH。
- 固件侧 `cmd_sleep` 已实现（`EPD_service.c:1083-1091`），`p_epd->epd = NULL`
  后下一次写图会经 `epd_ensure_display → epd_init` 重新复位/配置面板，因此
  "睡后再次传图"路径仍可用。
- 与"删 `cmd_sleep` 瘦身"的取舍：本轮选择**保留**该命令（网页已在用），
  `docs/flash-ram-optimization-20261104.md` §5.4 中"删 SLEEP"建议随之作废。

### 3.6 文档与注释修正

- `main.c` 看门狗注释：实际超时 **60 s**（SDK `sdk_config.h` 的 legacy
  `WDT_CONFIG_RELOAD_VALUE=60000` 经 `apply_old_config.h` 覆盖
  `NRFX_WDT_CONFIG_RELOAD_VALUE=10000`），原注释"约 32 秒"错误；已改为说明
  生效值与唯一修改点。
- `docs/ble-protocol.md` 低电量行为：原文写"进入低电量页/显示电池图标和电压"，
  与实现（`epd_battery_replaces_gui()` 固定 false，保留原布局）不符；已按
  实际行为改写（暂停周期刷新、LED 每 30 s 闪、每 10 s 轮询、临界关机）。

## 4. 体积与 RAM 结果

| 指标 | 基线 | 本轮后 | 变化 |
| --- | ---: | ---: | ---: |
| 应用 BIN | 55,004 B | **54,924 B** | **−80 B** |
| 应用区余量 | 2,340 B | **2,420 B** | +80 B |
| `.text` | 54,680 B | 54,704 B | +24 B |
| 静态 RAM（.data+.bss） | 9,204 B | **9,204 B** | 0 |
| 静态 RAM + 2 KiB 栈余量 | 4,404 B | **4,404 B** | 0 |

> 说明：C-06（`EPD_GPIO_Uninit` 对可选 pin 加 `0xFF` 判断）实现后实测 **+12 B**
> （pin 是运行期变量，且 `pinMode()` 内部本就有 `nrf_gpio_pin_present_check()`），
> 无行为收益，已回退，故最终净收益由 C-01（`EPD_SPI_Write` 168 → 96 B，−72 B）、
> C-02（`UC8179_Sleep` 36 → 32 B，−4 B）以及 LTO/对齐的少量变化构成。
> 最终 BIN SHA-256：`BDD933BE2A3FCFF8718F33B04FF918B609DC30716E941444977A0322B04F68DB`；
> 最终产物副本见 `output/work-20261104/ref-compare/build-final/`。

## 5. 后续建议（未实施）

> **2026-11-04 晚更新（第二轮）**：除 S-2 按用户要求暂缓外，本节原列的
> S-1（§5.1）、RLE（§5.3）、S-3（§5.4）、`{battery}`/`interleavedcount`/
> debug 驱动纠正（§5.5）均已在第二轮实施，结果见 §8；本节保留为第一轮评估
> 记录，未实施项以 §8.4 为准。

### 5.1 S-1（高）：`WRITE_BLOCK` 首块面板初始化移出 SoftDevice 事件中断

`on_write` 在 BLE 事件中断里同步执行 `epd_ensure_display` / `epd_enter_picture_mode`
（含 `drv->init`：复位 + 最长 1000 ms BUSY），可能造成 1–2 s 中断延迟
（`docs/clock-drift-20261023.md:100-113`）。建议首块只登记
`picture_entry_pending` 并回 `0x03 RESTART`，由主循环完成 GPIO/init/模式切换后
网页重发块 0（`html/js/ble_transfer.js` 已有 restart 兜底）。预计 +60~200 B Flash，
需扩展 `tests/service_lifecycle_fixture.c` 并真机验证传图耗时/时钟回调延迟。

### 5.2 S-2（高/发布）：恢复 Bootloader 独立源码

`Keil/DFU-nRF52.uvprojx` 引用 `..\Bootloader\main.c` 等文件，但仓库与历史中不存在
`Bootloader/`，只能依赖预编译 `tools/bootloader/bl_nrf52811_xxaa_s112_8k.hex`
（SHA256 `4465417221…`）。这是 OTA 可重建性的 P1 缺口，应恢复源码并用
`tests/test_dfu_release.py` 等校验重建 HEX 与现网逐字节一致；应用体积不变。

### 5.3 RLE 压缩传图（P2，唯一有产品价值的上游缺功能）

上游按"压缩后更小才用"策略对图片层做 RLE，并在 `mtu=<n> rle=1` 能力广播后启用。
当前 `WRITE_BLOCK` 有 8 B 头 + CRC16 + 层/首块标志，不能照搬上游
`WRITE_IMAGE`。若要实施：在 cfg 字节新增"本块为自包含 RLE 码流"标志（需兼容
现有 `0x00/0x0F/0xF0/0xFF` 编码），固件解压到 ≤255 B 栈缓冲后按 begin/continue
调 `write_ram`，网页端按"完整 RLE code"切块、CRC 仍按压缩流计算；预计固件
+200~400 B、网页 +~2 KB，需新增编解码对拍与传输回归。照片类抖动图收益接近 0
（会被"更小才用"跳过），线稿/课表类平色图可省 50%–80% 空口时间。

### 5.4 S-3（中）：状态/最终 ACK 通知可靠重发

0xA0/0xA1 目前单次发送，HVX 满时 `NRF_ERROR_RESOURCES` 被忽略，网页只能整层
重传。可复用现有 `mode_ack`/`record_ack` + `HVN_TX_COMPLETE` 机制，只保留最近
一条待发副本，预计 +60~150 B Flash、+7~8 B RAM。

### 5.5 其余低优先级项

- 电池图标/百分比（上游 `batt_cal`）→ 新增 `{battery}` token 约 +100~200 B；
  或只改文档（本轮已完成文档修正）。
- `interleavedcount` 死控件接线或删除；debug 驱动下拉与固件单型号不符；
  `fourColor` 下拉未加"仅预览"标注（本轮未改 `index.html`）。
- 多面板/在线切屏、原始命令透传、内置 FDS、`feature/ble_ack` 的"刷新后不
  sleep"、BUSY 期间 LED 翻转等：各报告已给出量化理由，**明确不建议**。

## 6. 验证

- **主机测试**：`build.bat build` 内置 `tests/run_host_tests.sh` 输出
  `HOST_OK`（C 测试 + Node 26 个文件 + Python 全部通过）；新增
  `tests/test_review_bwr_dither.cjs` 6 项、`test_ble_lifecycle.cjs` 37 项
  （原 32）、`test_image_workspace.cjs` 14 项（原 13）、`test_uc8179`
  新增 refresh→sleep 用例均通过。
- **ARM 构建**：`Flash: 54,924 / 57,344`、`RAM reserved: 11,252 / 15,656`
  （含 2 KiB 栈）、`IRQ handlers: 6 forced handlers are strong T`、
  `check_flash_budget.py` 通过、`nrf52811_xxaa-full.hex` 生成成功；
  BIN SHA-256 `BDD933BE…B04F68DB`。
- **网页**：`node --test tests/test_image_workspace.cjs tests/test_ble_lifecycle.cjs
  tests/test_review_bwr_dither.cjs` → 57 pass / 0 fail；`tests/test_legacy_*.cjs`
  → 106 pass / 0 fail；改动 JS `node --check` 通过。
- **未验证**（无硬件/无 Keil）：真机 SLEEP 电流与"睡后再次传图"、C-02 刷新
  耗时/电流、温度读取时序（C-03 仍待真机确认）、Keil 编译、实机传图回归。

## 7. 产物与复现

- 逐层分析：`output/work-20261104/ref-compare/{driver,system,web}.md`
- 本轮构建/测试完整日志：`output/work-20261104/ref-compare/build-and-host-tests.log`
- 构建产物：`_build/nrf52811_xxaa.{out,bin,hex,map}` 与
  `_build/nrf52811_xxaa-full.hex`
- 复现：
  ```powershell
  .\build.bat build
  node --test tests/test_image_workspace.cjs tests/test_ble_lifecycle.cjs tests/test_review_bwr_dither.cjs
  ```

## 8. 第二轮实施（S-1/S-3、界面与 RLE，2026-11-04 晚）

按用户要求"除 S-2 外继续按分析优化"，第二轮实施以下项目；S-2（Bootloader
独立源码）按用户指示暂缓，未做。

### 8.1 实施内容

1. **S-1 首块 `WRITE_BLOCK` 面板初始化移出 SoftDevice 事件中断**（高优先级）
   - `ble_epd_t` 增加 `picture_entry_pending`；`cmd_write_block` 在中断内只登记
     并回 `0xA0 status=0x03 (RESTART)`，不再执行 `epd_ensure_display/epd_init/
     drv->init`；新增 `epd_picture_entry_process()` 在主循环完成 GPIO 获取、
     面板 init 与切图片模式（受连接/刷新/INIT/资源会话门控）；断连清除 pending。
   - 网页已有的 RESTART→从块 0 重传语义直接复用，无新命令。
   - 主机回归：块 0 后 init/driver-init/RAM 写调用数不变、主循环 tick 后各一次、
     重发块 0 收到 ACK 且 `write_ram` 收到数据；负向对照可失败。
2. **S-3 0xA0/0xA1 通知 HVX 满时可靠重发**：中断里把最新应答（≤7 B）存入单槽，
   主循环（含 `HVN_TX_COMPLETE` 后下一轮）补发；其它错误丢弃、断连/CCCD 关闭
   清槽。主机测试注入 `NRF_ERROR_RESOURCES`/`BUSY`/`INVALID_STATE` 验证保留/
   丢弃/重发。
3. **`{battery}` UI token（电量百分比）**：`resources/ui-schema.json` 追加
   `battery`（21 号，既有 1–20 不变）并重新生成 `GUI/ui_schema.h` /
   `html/js/ui_tools/ui_data.js`；固件 `ui_battery_percent()` 与网页预览使用
   同一整数曲线（3000 mV→100、2800→32、2500→10、2000→0）；`font_store_tool.js`
   的缺字生成列表同步纳入 `battery` 数字与 `%`。默认布局未改，金图不变。
4. **`interleavedcount` 死控件接线**：传图时读取并 clamp 1..100（默认 20），
   作为 GATT response fence 间隔；非法/缺失回退 `BATCH_SIZE`。
5. **debug 驱动下拉纠正**：收到 14 B 配置通知/0xA9 回读后按固件 `config[7]`
   选中实际型号（当前仅 `07`），未知型号保留选择并警告。
6. **RLE 压缩传图（上游唯一可借鉴的功能）**
   - 新模块 `EPD/epd_rle.c/.h`：每块自包含 RLE 解码（repeat `0x80|(count-3)`
     count 3–130 +1B；literal `count-1` count 1–128 +原文），输出上限 236 B；
     host 单测 `tests/test_rle.c`。
   - `WRITE_BLOCK` 的 total 字段 **bit15=RLE 标志**，低 15 位为真实块数；固件
     用 236 B 静态缓冲解压后按原 cfg `write_ram`，非法码流回 `0xA0 CRC` 且不写
     RAM/不计块，`0xA1` 状态回真实 total；CRC 仍按压缩 payload 计算。
   - 能力通告：`epd_send_mtu()` 与 initial-notify stage2 均发
     `mtu=<n> rle=1`；网页 `html/js/rle.js` 编码器按块仅在更小时使用，能力由
     `main.js` 从字符串通知解析（并修正 `BleTransfer.init()` 必须在订阅通知前
     执行，否则先到的能力会被清掉）。旧网页/旧固件互不影响。
   - 网页单测：`tests/test_rle.cjs`（14 项）+ `tests/test_rle_transfer.cjs`
     （5 项：能力开关、全平色整层 RLE、混合 raw/RLE、reset 清能力、CRC/包长）。
7. **协议文档**：`docs/ble-protocol.md` 增补 `0x30/0x31/0x32` 包格式、
   `0xA0/0xA1` 通知格式、RLE 能力与格式、首块 RESTART 行为；低电量行为按
   实现修正（已在第一轮完成）。

### 8.2 体积与 RAM 结果（第二轮最终构建）

| 指标 | 第一轮后 | 第二轮后 | 变化 |
| --- | ---: | ---: | ---: |
| 应用 BIN | 54,924 B | **55,772 B** | **+848 B** |
| 应用区余量 | 2,420 B | **1,572 B** | −848 B |
| `.text` | 54,704 B | 55,552 B | +848 B |
| 静态 RAM（.data+.bss） | 9,204 B | **9,444 B** | **+240 B** |
| RAM + 2 KiB 栈余量 | 4,404 B | **4,164 B** | −240 B |

> RAM 的 +240 B 中 **236 B 是 RLE 静态解压缓冲 `s_rle_out`**（放在静态区而非
> 中断栈），其余约 4 B 为 S-1/S-3 新增状态字段（含对齐）。BIN SHA-256：
> `E46C29984FE5275034D11179025681FB2534934FE7F0BA588D64C31E1CB38B06`。
> 应用区余量 1,572 B 仍高于 0，但低于 `check_flash_budget.py` 的 4 KiB 提示线；
> 若继续加功能，建议先评估裁剪（例如不启用 `{battery}` 默认模板、RLE 只在
> 需要时保留，或按 §5.1 的 8 KiB 数据区方案重排）。

### 8.3 验证

- **主机测试**：`build.bat build` 输出 `HOST_OK`（C 测试 + Node 28 个文件 +
  Python 全部通过）。新增/扩展用例：`test_rle`（C）、`test_rle.cjs` 14 项、
  `test_rle_transfer.cjs` 5 项、`test_service_lifecycle.py` 13 行 PASS（含
  S-1/S-3 注入用例）、UI 渲染 29 例（含 4 个电池百分比电压点）、
  `test_ble_lifecycle.cjs` 39 项（含 7 组 fence/驱动纠正/SLEEP 用例）。
- **ARM 构建**：`Flash: 55,772 / 57,344`、`RAM static: 9,444`、
  `RAM reserved: 11,492 / 15,656`（含 2 KiB 栈）、6 个强制 IRQ 强符号、
  `check_flash_budget.py` 通过、`nrf52811_xxaa-full.hex` 生成成功；
  `tests/test_application_build.py` 8/8 通过（含 `epd_rle.c` 加入 GNU/Keil
  源文件列表后的真实构建与预处理检查）。
- **网页**：`node --test tests/test_ble_lifecycle.cjs tests/test_rle_transfer.cjs
  tests/test_rle.cjs tests/test_image_workspace.cjs` → 72 pass / 0 fail；
  `tests/test_review_bwr_dither.cjs` 6 pass；legacy 三文件 106 pass。
- **未验证**（无硬件/无 Keil）：真机首次传图的 RESTART 重试预算（正常初始化
  ~40 ms，最坏 ~1 s）、真实 HVX 满时 latest-wins 的观感、RLE 实际省时/耗电、
  `{battery}` 真机布局、Keil 编译。

### 8.4 仍未实施

- **S-2（高/发布）**：Bootloader 独立源码缺失（用户明确暂缓）。
- **S-7（可选）**：RC ppm 的在线估计/售后可调；当前固定 -298 ppm 仅单板标定。
- 低电量页/电池图标组件、v1.5 轻量页、多面板等仍按第一轮结论**不建议**。
- 后续 Flash 余量若不足：优先评估 8 KiB 数据区重排（需 Bootloader 同步），
  其次再考虑裁剪 RLE/电池 token 等新增功能。

## 9. 后续（分层重构、清理与模拟器修复，2026-11-04）

第二轮之后完成了 EPD 分层重构（hal/driver/board/storage/app/service）及两项
清理/修复，协议、刷新时序与 BLE 行为不变；详见
[docs/epd-architecture-20261104.md](epd-architecture-20261104.md)：

- **模拟器 host 构建修复**：`Makefile`/`Makefile.win32` 统一 host 源集合
  （生产 GUI + `tests/stubs/` 资源桩 + 内置字库），`emulator.c` 修正
  `BITMAPINFO` 调色板越界写；`make`/`make -f Makefile.win32` 可零告警生成
  `emulator.exe`，窗口与离屏回放均为 800×480 BWR。
- **service include 收窄**：26/26/27/26/26 → 22/15/20/9/11 条（逐条真实 ARM
  `-fsyntax-only -Werror` 验证）；BIN 仍 55,932 B（与重构后逐字节一致）。
- **`epd_panel_*` out-of-line 试验否决**：实测 +16 B（55,948 vs 55,932），
  保留 `static inline`；该负结果再次说明体积优化必须以真实 ARM 构建为准。
- 最终 `build.bat build` exit 0（HOST_OK），BIN SHA-256 仍为
  `4504DA0F…E75394DA`，`tests/test_application_build.py` 与
  `test_build_config_parity.py` 通过。

