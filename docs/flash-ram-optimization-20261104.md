# 内置 Flash / RAM 优化、构建配置与缺陷修复（2026-11-04）

本轮目标是降低 `nRF52811 + UC8179 7.5 寸三色` 主线的内置 Flash 与 RAM 占用，
同时复核固件/网页实现中的缺陷。所有体积数据均为实际 ARM 构建（仓库自带
GCC 9.3.1、`-Os -g3 -flto -flto-partition=one`、固定 800×480 三色面板、
外置字体/资源、日志关闭）的 `BIN` 与 ELF `size` 实测值，不是估算。

## 1. 结果

| 指标 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| 应用 BIN | 56,388 B | **55,004 B** | **−1,384 B（−2.45 %）** |
| 56 KiB 应用区余量 | 956 B | **2,340 B** | **+1,384 B** |
| ELF `.text` | 56,176 B | 54,784 B | −1,392 B |
| `.data` | 212 B | 220 B | +8 B |
| `.bss` | 9,300 B | 8,984 B | −316 B |
| 静态 RAM（.data+.bss） | 9,512 B | **9,204 B** | **−308 B** |
| 静态 RAM + 2 KiB 栈 | 11,560 B | **11,252 B** | −308 B |
| 15,656 B RAM 预算余量（含栈） | 4,096 B | **4,404 B** | +308 B |

应用区仍为 `[0x19000, 0x27000)`，栈仍为 2,048 B，堆为 0；分区、Bootloader、
SoftDevice、外置资源格式与协议版本均未改变。

最终 BIN SHA-256：`c90f8fb6d08d9bb9bcbb93423332895ba11f4d661886a563a949d52db64c8753`。

## 2. 已实施的优化

### 2.1 唤醒脚直接使用 GPIOTE，删除 nrfx_gpiote 驱动（−1,112 B Flash / −92 B RAM）

整个固件只有 `main.c` 的唤醒脚使用 GPIOTE。原实现通过通用 `nrfx_gpiote`
驱动完成「低功耗 SENSE 输入 + PORT 事件中断」，链接了约 1.1 KiB 通用驱动代码
和 92 B 控制块。现改为直接寄存器/ HAL 调用（`main.c`）：

- 布防：`nrf_gpio_cfg_input` + `nrf_gpio_cfg_sense_set(HIGH)`，清
  `EVENTS_PORT`，置 `INTENSET.PORT` 并以优先级 6（原 `GPIOTE_CONFIG_IRQ_PRIORITY`）
  使能 `GPIOTE_IRQn`；
- `GPIOTE_IRQHandler`：确认/清除 `EVENTS_PORT`，撤防（SENSE、INTENCLR、NVIC），
  然后调用原有 `advertising_start()`。

行为与原来的低精度 PORT 事件路径等价；`Makefile.nRF52_linux` 与
`Keil/EPD-nRF52.uvprojx` 已移除 `nrfx_gpiote.c`。主机侧
`tests/offline_radio_fixture.c`、`tests/test_service_lifecycle.py` 已同步改为
校验新的布防/撤防/广播顺序。

### 2.2 移除 `DEVELOP_IN_NRF52840`（−272 B Flash，并修正硬件配置）

该宏是 Nordic 例程中「在 nRF52840 开发板上运行 nRF52811 固件」的兼容开关，
会为 nRF52811 镜像启用 nRF52840 的 errata 集合、把 UICR `PSELRESET` 写成
P0.18（nRF52840 的复位脚）并写入 nRF52840 专用的 NFC 引脚重映射。本项目硬件
是自定义 nRF52811 价签，因此应从 GNU `CFLAGS`/`ASMFLAGS` 与 Keil 两个 Target
的编译/汇编宏中删除。删除后 SystemInit 使用 nRF52811 的 errata 与
`PSELRESET = P0.21`，同时少 272 B Flash。

如果确实要在 nRF52840 DK 上调试 nRF52811 镜像，请只在该调试构建中把
`-DDEVELOP_IN_NRF52840` 加回。

### 2.3 仅对 DFU 三个文件启用 LTO（−192 B Flash）

删除 `ble_dfu.c`、`ble_dfu_unbonded.c`、`nrf_dfu_svci.c` 的 `-fno-lto` 例外；
`drv_rtc.c`、`nrf_sdh.c`、`nrfx_wdt.c`、`nrfx_spi.c`、`nrfx_spim.c` 的例外**必须保留**：

> GCC 9.3.1 LTO 会把 `-Wl,-u` 强制保留的中断处理函数内部化，链接后向量表回落到
> startup 的弱 `Default_Handler`（`b .` 死循环），第一个中断即挂死。本轮实验中
> 「全部启用 LTO」看起来能省 1.3–1.5 KiB，但 6 个强制中断全部变成弱符号，属于
> **不可发布**构建。

为防止回归，新增 `tools/check_irq_handlers.py`，并在 `make` 生产构建
（`nrf52811_xxaa` 目标）链接后自动执行：要求 `GPIOTE/RTC1/WDT/SPIM0/TWIM0…/
SWI2` 六个处理函数在 ELF 中为强符号 `T`、大小 ≥4 B、且不指向
`Default_Handler`。对应的单元测试为 `tests/test_irq_handler_check.py`。
`tests/test_build_config_parity.py` 同时锁定 GNU/Keil 的发布宏一致性与
`-fno-lto` 例外清单。

### 2.4 资源应答去重缓存改为 CRC32（−240 B RAM，Flash 净 +16 B）

`EPD/resource_transfer.c` 原来用 244 B 的 `previous[]` 与上一请求逐字节比较，
以识别 BLE 重传。改为保存长度 + CRC32（`resource_crc32`），语义等价（碰撞时
只会重发上一条应答而不是重新执行，概率 2⁻³²）。RAM 收益 240 B，CRC 计算带来
约 16 B Flash 净增。

### 2.5 条件时钟局刷资格提前判断（+144 B Flash，省掉每周期一次无用的 init_partial）

`epd_clock_refresh_try()` 原本在 `EPD_service.c` 先执行 `init_partial()`
（面板复位 + 局部刷新配置，约 30–60 ms 且耗电）之后才检查 UI 几何/条件时钟
限制；对使用 `SYNC_ONLY` / `JIEQI_AHEAD` / `JIEQI_TODAY` 条件组件的非默认布局，
检查必然失败并回退全刷，于是每个刷新周期都白做一次面板局部初始化。

本轮给 `epd_clock_refresh_try()` 增加 `prepare_only` 参数：为 true 时只执行与
真正局刷完全相同的资格检查（旧帧基准、UI 资源几何、`partial_allowed`、
Jan-2025 覆盖层窗口），不绘制、不访问面板。`EPD_service.c` 在
`init_partial()` 之前先做该检查，不通过就直接走全刷路径；无条件时钟布局的
正常 OLD/NEW 局刷不受影响。

- 收益：条件时钟布局每个刷新周期少一次 `init_partial()`（面板复位/配置）；
  像素与刷新结果不变。
- 成本：实测 **+144 B Flash**（`epd_gui_update` 与 `try()` 的内联/拆分方式变化），
  RAM 不变。
- 回归测试：`tests/test_review_clock_partial_eligibility.c`（已接入默认主机测试
  脚本），覆盖三种条件标志、Jan-2025 覆盖层以及无条件时钟仍走局刷的对照。

### 2.6 Keil 应用 Target 与 GCC 发布配置对齐

`Keil/EPD-nRF52.uvprojx` 的 `nRF52811_xxAA` Target 此前仍带
`USE_SYSVIEW` + `NRF_LOG_ENABLED=1`，并参与编译 `nRF_Log`、`nRF_Segger_RTT`、
`SystemView` 源文件。这有两个问题：

1. **Flash/RAM**：GCC 等价配置下仅打开日志就使镜像超出 56 KiB 约 13 KiB；
   Keil 侧虽无法在本环境实测（无 ARMCC），但日志/RTT/SystemView 代码与缓冲同样
   显著增加占用。
2. **低功耗回归**：`main.c` 的 `idle_state_handle()` 在 `USE_SYSVIEW` 下跳过
   `nrf_pwr_mgmt_run()` 休眠，相当于把电池设备的空闲功耗拉满。

本轮已把该 Target 改为发布配置：移除 `USE_SYSVIEW`、`NRF_LOG_ENABLED=1`、
`DEVELOP_IN_NRF52840`，增加 `NRF_STRERROR_ENABLED=0`，并从该 Target 移除
`nRF_Log` / `nRF_Segger_RTT` / `SystemView` 三个源文件组与 `nrfx_gpiote.c`。
`flash_softdevice` Target 同步移除 `DEVELOP_IN_NRF52840` 并禁用 strerror。

> 需要 RTT/SystemView 调试时，可临时恢复该 Target 的宏与源文件组，或改用
> GCC `DEBUG_LAYOUT=1` 调试布局。
>
> **2026-11-05 补充（AC6 迁移后实测）**：该 Target 已改用 ARM Compiler 6.11 +
> LTO（`Keil/EPD-nRF52-ac6.sct` + `Keil/ac6_gnu_symbols.c`），uVision 5.38
> 实测 **0 errors / 0 warnings**，Code 52,704 B、RO-data 1,400 B、RW-data 112 B
> （Flash 合计 54,216 B）。原因：ARMCC5 无法 LTO，同源镜像约 58.9 KiB，超出
> 56 KiB 应用区 1,528 B；SoftDevice 中转/烧录已拆到独立单 Target 工程
> `Keil/flash_softdevice.uvprojx`（ARMCC5，只链接占位镜像并复制官方 HEX）。
> GCC 构建保持 55,916 B 不变。

## 3. 已修复的缺陷

### 3.1 BUSY 超时与耗时统计的整数截断

`EPD/EPD_driver.c` 原实现先算 `ticks_per_ms = (32768/2)/1000 = 16`，再乘
timeout，导致 1000 ms → 976.5625 ms、30 s → 29.296875 s（短 2.34 %），看门狗
喂狗阈值 500 ms → 488.28 ms；`EPD/EPD_service.c` 的日志耗时则相反地被放大
2.4 %。现改为先乘后除：
`timeout_ticks = timeout × 16384 / 1000`（uint16 timeout 乘积 < 2³²，任意
uint16 都安全），耗时统计用 64 位精确比例（仅日志构建编译）。修正后
1000 ms = 16,384 ticks、30 s = 491,520 ticks、喂狗 8,192 ticks，均精确。

### 3.2 资源应答投递失败导致显示刷新停摆

`EPD/resource_transfer.c` 原实现只有在应答发送成功时才清除
`reply_pending`，其它错误（如未使能通知时的 `NRF_ERROR_INVALID_STATE`）会
永久保留待发状态；`epd_gui_update()` 在该标志为真时持续放弃刷新，设备保持连接
时只能靠断开/重连恢复。现约定：

- `NRF_SUCCESS` → 完成；
- `NRF_ERROR_RESOURCES` / `NRF_ERROR_BUSY` → 保持待发，下轮重试；
- 其它错误 → 视为不可投递并丢弃，显示刷新继续；网页重试请求时会重新执行/复用
  应答。

回归测试 `tests/test_review_resource_reply_wedge.c` 已接入默认主机测试脚本。

### 3.3 其它

- `tests/test_clock_timekeeping.py` 在非 UTF-8 控制台（如中文 Windows）会因
  GCC 本地化诊断信息解码失败而中断，已改为容错解码并强制 `LC_ALL=C`。
- 新增主机测试：`tests/test_review_clock_partial_eligibility.c` 固定「条件
  时钟组件不得进入硬件 OLD/NEW 局刷」以及「资格检查在 init_partial 之前完成」
  的行为（见 §2.5）。

## 4. 验证

- **主机 C 回归**：`tests/run_host_tests.sh` 在最终树上输出 `HOST_OK` 全部通过
  （含新增的资源应答、条件时钟、配置一致性、IRQ 检查用例）。
- **网页**：`run_host_tests.sh` 当前 Node 列表 25 个文件 **482 项通过 / 0 失败**；
  另补跑未列入脚本的 legacy/archive 4 个文件 **111 项通过 / 0 失败**，合计
  **593 项**。本轮修复帧同步/资源安装的 RS_BUSY 重试问题并新增
  `tests/test_review_font_sync_busy.cjs`（详见 §6）。
- **Python 工具/跨层检查**：字体/协议/资源包/发布工具/动态字体/GUI/待办等
  全部通过；新增 `tests/test_build_config_parity.py`、`tests/test_irq_handler_check.py`。
- **ARM 构建**：`Flash: 55,004 / 57,344`、`IRQ handlers: 6 forced handlers are
  strong T`、`check_flash_budget.py` 通过；`tests/test_application_build.py`
  的 8 项真实 ARM 构建/增量依赖检查通过。
- **像素/刷新回归**：GUI 金图（含 OLD/NEW 条带）与资源加载测试全部通过，
  本轮未改动绘制路径，画面哈希不变。
- **未验证**（本环境无硬件/无 Keil）：唤醒脚实际唤醒→重新广播、System OFF、
  DFU/OTA 全流程、Keil 编译、实机刷新耗时与栈高水位。

## 5. 仍未采用 / 后续可评估

1. **回收 8 KiB 保留区（容量方案，不是代码压缩）**：应用区可由 56 KiB 扩为
   64 KiB，余量可达约 10 KiB；但需要同步修改 Bootloader 的
   `NRF_DFU_APP_DATA_AREA_SIZE`、链接脚本、DFU 打包审计与回退兼容性，属于
   分区/Bootloader 项目，本轮未做。
2. **`UI_MAX_COMPONENTS 64 → 21`**：实测 **−1,376 B RAM / +64 B Flash**。
   UIR2 实际只有 20 个组件 ID、网页固定生成 19 个；若后续需要 RAM 再考虑，
   同时应让 `ui_resource_codec` 上限与固件常量一致。
3. **`s_gfx_buffer 1400 → 800`**：约 −600 B RAM，但三色页行数 7→4、页数
   69→120，刷新耗时需实机测量，暂不采用。
4. **网页侧可简化项**（需产品确认）：`0x20 SET_TIME`、
   `0x21 SET_WEEK_START`、`0x90 SET_CONFIG`、`0x92 SYS_SLEEP`、
   `0x99 CFG_ERASE`；`t=` 时间字符串通知只被网页当日志显示；TODO v4 字体名称
   仅供回读显示；A8 提交确认的 revision 网页未使用。删除这些可继续减少数百
   字节固件代码，但会改变开发模式命令集/跨浏览器回读体验，属于产品取舍。
   > 2026-11-04 更新：`0x04 SLEEP` **已不在可删列表**——参考上游实现后，
   > 网页在图片刷新确认成功后主动发送 SLEEP 省电（见
   > [参考上游 EPD-nRF5 的对比分析与优化](upstream-reference-optimization-20261104.md)
   > §3.5），固件必须保留 `cmd_sleep`。
5. **自定义启动代码**（绕过 crt0/libc init/exit，约 −236 B + −96 B RAM）风险
   高（初始化顺序、启动验证），不建议为几百字节引入。
6. **已实测无效/不可用的方向**：`-g3` 不影响 BIN；`-falign-*`、
   `-fno-unwind-tables`、`-fno-common`、`-fshrink-wrap` 收益为 0；
   `-fno-jump-tables` 反而 +144 B；LDFLAGS 加 `-ffunction-sections` 会 +552 B；
   默认 LTO 分区 +176 B；对带中断处理函数的 SDK 文件启用 LTO 会得到
   **不可运行镜像**（见 §2.3）。

## 6. 网页侧同期修复（W-01/W-02/W-03）

详见 `output/work-20261104/web-bugs/report.md` 与实施记录。摘要：

- **W-01（中）**：`html/js/font_sync.js` 在真正写入前就把维护会话标记为已进入，
  设备在面板刷新窗口返回 `RS_BUSY` 时会被误判为写失败，触发整包旧字库回滚重写，
  甚至误报「旧字库恢复未完成」。改为与 `todo_sync.js` 一致的 ENTER/RS_BUSY
  重试（250 ms 间隔、约 90 s 预算），真实写失败后的回滚逻辑保持不变。
- **W-02（低-中）**：`html/js/resource_installer.js` 的资源安装同样对瞬时
  `RS_BUSY` 增加了等待/重试，不再让一次长安装因首个 ENTER 失败而中止。
- **W-03（低）**：`docs/ble-protocol.md` 补齐能力位 bit6（ERASE，掩码 0x7F）、
  op 11 ERASE 与 `0xA6` INIT 完成通知格式。

## 7. 产物与复现

- 最终固件：`output/work-20261104/build/final1/nrf52811_xxaa.{out,bin,hex,map}`。
- 体积/RAM 复核：
  ```powershell
  make -f Makefile.nRF52_linux memory OUTPUT_DIRECTORY=output/work-20261104/build/final1 ...
  ```
- 中断向量安全检查：
  ```powershell
  python tools/check_irq_handlers.py --nm <arm-none-eabi-nm> <...>.out
  ```
- 分析明细：`output/work-20261104/size/report.md`（实验矩阵、模块/函数归属、
  RAM 明细）、`output/work-20261104/bugs/report.md`（缺陷复现与证据）、
  `output/work-20261104/web-bugs/report.md`（网页缺陷与协议一致性）。
- 构建脚本容错：`build.bat` 的 clean 步骤在 `_build` 被其他工具写入受限 ACL、
  目录本身无法删除时会自动清空并复用该目录继续构建，并打印管理员清理命令；
  相对路径的批处理调用改为绝对路径，避免 `NoDefaultCurrentDirectoryInExePath`
  环境下列表/调用失败。主机测试需要 Python ≥3.10，`tests/run_host_tests.sh`
  会自动选择可用的高版本 Python（含 Windows `py -3`），也支持 `HOST_PYTHON`
  覆盖。详见 README FAQ 第 8 条与“命令行 GCC 编译”说明。
