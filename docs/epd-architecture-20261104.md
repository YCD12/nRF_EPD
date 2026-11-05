# EPD 代码分层重构与扩展设计（2026-11-04）

本轮把 `EPD/` 从"平铺 + 单体服务"整理为按嵌入式分层组织的模块树，目标：

1. **按职责分层**：HAL/板级 → 面板驱动 → 存储 → 应用策略 → BLE 服务；
2. **拆分单体**：`EPD_service.c`（约 2,300 行）按功能拆为 service 层多个模块；
3. **屏幕扩展**：面板能力通过 `epd_driver_t` 虚函数表暴露，服务层不再直接依赖
   `UC8179_*` 符号；新增屏幕只需新增 driver 实现 + 型号表项；
4. **兼顾体积**：保持 `-Os -flto -flto-partition=one` 与静态分页缓冲，重构后
   用真实 ARM 构建对比 BIN / 静态 RAM。

## 1. 重构前问题

| 问题 | 表现 | 影响 |
| --- | --- | --- |
| 目录平铺 | 45 个文件混在 `EPD/` 一个目录 | 看不出依赖方向，新增文件容易乱放 |
| 单体服务 | `EPD_service.c` 2,166 行，含 BLE 事件、命令处理、显示刷新、通知、传图、配置校验、电源策略 | 修改任一功能都要读大文件；合并冲突高；难做单元测试 |
| 面板耦合 | `EPD_service.c` / `epd_clock_refresh.c` 直接 include `UC8179.h`，调用 `UC8179_ClearBusyTimeout/HadBusyTimeout/Refresh_Partial` 等 | 换屏要改服务层；无法并行支持第二种面板 |
| 板级配置散落 | 默认引脚/型号写死为 `EPD_CFG_52811`（service 内宏） | 新板子要改服务代码 |
| 型号表内嵌 | `epd_models[]` 与 HAL/SPI 代码在 `EPD_driver.c` 同一文件 | 面板注册与 HAL 耦合 |
| 依赖方向不显式 | HAL 头包含 `EPD_config.h`（板级持久化类型） | 层级倒置，但属于历史兼容点，保留并在文档标注 |

## 2. 目标目录树

```
EPD/
├── hal/                  # MCU 外设与板级硬件访问（不含面板协议）
│   ├── EPD_driver.c/.h   # SPI/GPIO/ADC/LED/复位/BUSY/引脚映射/半双工读
│   ├── epd_panel.h       # [新增] 面板型号/驱动虚表/可选能力包装（服务只依赖此头）
│   └── fm25q04b.c/.h     # 外置 SPI Flash 芯片驱动
├── driver/               # 墨水屏面板/驱动 IC 实现（纯面板逻辑）
│   ├── UC8179.c/.h       # UC8179 全刷 + 真 OLD/NEW 局刷
│   ├── epd_models.c      # [新增] 编译进固件的型号表 + epd_get_model/epd_init/查询
│   └── (新增屏幕/驱动 IC 放这里)
├── board/                # 板级配置与持久化格式
│   ├── EPD_config.c/.h   # 引脚/型号/显示模式/周起始 的持久化记录
│   └── epd_board.h       # [新增] 默认引脚表、默认型号、板级能力宏
├── storage/              # 外置 Flash 资源与记录存储
│   ├── resource_store.c/.h, resource_crc*.c, resource_layout.h
│   ├── persistent_record.c/.h
│   └── font_store.c/.h
├── app/                  # 显示应用/策略层（依赖 driver + storage + GUI）
│   ├── epd_refresh_policy.c/.h, epd_battery_policy.h
│   ├── epd_clock_refresh.c/.h, epd_clock_window.h
│   ├── epd_resources.c/.h
│   └── calendar_annotations_store.c/.h, calendar_annotations_tx.c/.h
└── service/              # BLE 服务/协议层（依赖 app + driver + storage）
    ├── EPD_service.c/.h            # 公开 API、BLE 事件路由、生命周期、主循环调度
    ├── epd_service_internal.h      # [新增] service 内部共享声明
    ├── epd_service_display.c       # [新增] GUI 刷新/全刷/局刷/省电/重试
    ├── epd_service_cmd.c           # [新增] 命令处理、配置校验/应用、命令队列
    ├── epd_service_control.c       # [新增] 通知/ACK/回执/MTU/时间/电压字符串
    ├── epd_service_transfer.c      # [新增] WRITE_BLOCK/RLE/块应答/状态查询
    ├── epd_advertising.c/.h
    ├── epd_protocol.h, epd_cmd_policy.h, epd_command_queue.h
    ├── epd_display_queue.h, epd_work_queue.h, epd_offline.h
    ├── resource_protocol.c/.h, resource_transfer.c/.h
    └── epd_rle.c/.h
```

依赖方向（箭头表示"可依赖"，禁止反向）：

```
hal ──► board(仅 epd_config_t 类型)          （历史兼容点，见 §6）
driver ──► hal
storage ──► hal
app ──► driver, storage, GUI
service ──► app, driver, storage, hal
main.c ──► service (+ app 的少量初始化接口)
GUI ──► storage 的资源读取接口（保持现状）
```

- `hal` 不包含任何面板协议命令；`EPD_driver.h` 中的 `UC81xx_*` 命令枚举应下沉到
  `driver/`（`uc81xx_commands.h` 或 driver 私有头），仅面板驱动使用。
- `service` 不允许直接 `#include "UC8179.h"`，只能通过 `epd_panel.h` 的虚表/包装。
- `driver` 不依赖 `service/GUI/app`。
- `storage` 不依赖面板/服务。

## 3. EPD_service.c 拆分边界

| 模块 | 职责 | 主要函数/数据 |
| --- | --- | --- |
| `EPD_service.c` | 公开 API、CCCD/连接/断开、事件路由、`ble_epd_process` 主循环、SLEEP/复位准备、离线请求、资源事件回调、INIT 工作机、连接代次清理 | `ble_epd_init/on_ble_evt/process/on_timer/sleep_prepare`、`epd_init_process`、`epd_connection_process`、`on_write`、`epd_service_on_write`、`epd_resource_event` |
| `epd_service_display.c` | 显示请求/完成/失败重试、全刷、时钟局刷、低电量守卫、面板省电、GUI 数据填充 | `epd_gui_update`、`epd_perform_full_refresh`、`finish_refresh`、`epd_display_*`、`epd_battery_guard`、`epd_fill_device_name` |
| `epd_service_cmd.c` | 命令处理：INIT/SET_PINS/SLEEP/SET_TIME/SYNC/SET_WEEK/SET_ANNOTATIONS/SET_MODE/GET_CONFIG/SET_CONFIG/SYS_*/CFG_ERASE、显示命令、配置校验/默认值/应用 | `cmd_*`、`epd_config_validate/apply/defaults/pins_valid`、`epd_execute_command`、`epd_process_next_command` |
| `epd_service_control.c` | 通知与回执：initial notify（config/mtu/voltage/time）、模式/显示 ACK、记录提交 ACK、配置回读、字符串格式化 | `epd_send_*`、`epd_command_ack`、`epd_control_notifications_process`、`epd_record_ack_process`、`epd_config_readback_process` |
| `epd_service_transfer.c` | 图片分块传输：CRC16、块 ACK/状态、WRITE_BLOCK（含 RLE 解压）、QUERY/RESET、bulk reply 重发 | `crc16_compute`、`send_block_response/status`、`epd_bulk_reply_*`、`cmd_write_block/query/reset` |
| `epd_service_internal.h` | 上述模块间共享函数声明/常量；不对外公开（外部只用 `EPD_service.h`） | — |

约束：
- 跨模块函数由 `static` 改为普通函数并在 internal header 声明；仅模块内使用
  的继续 `static`。GCC LTO 单分区会重新内联/裁剪，预期体积近中性。
- 文件级静态量归各自模块（`s_display_retry_*`→display、`s_rle_out`→transfer、
  `s_anno_epd`→cmd/annotations）。
- 不改变 BLE 协议、GATT 时序、刷新顺序、低电量/OTA 行为；只搬代码。

## 4. 屏幕扩展设计

### 4.1 面板虚表（driver/epd_panel.h）

在现有 `epd_driver_t` 基础上增加"可选能力"：

```c
typedef struct epd_driver {
    epd_driver_ic_t ic;
    void (*init)(const epd_model_t *epd);
    void (*clear)(const epd_model_t *epd, bool refresh);
    void (*write_image)(...);
    void (*write_ram)(const epd_model_t *epd, uint8_t cfg, uint8_t *data, uint8_t len);
    void (*refresh)(const epd_model_t *epd);
    void (*sleep)(const epd_model_t *epd);
    int8_t (*read_temp)(const epd_model_t *epd);
    /* 可选：真 OLD/NEW 局刷 */
    void (*init_partial)(const epd_model_t *epd);
    bool (*refresh_partial)(const epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    bool (*diff_push)(const epd_model_t *epd, ...);
    /* 新增：驱动级 BUSY 错误状态（不再由服务层直接调 UC8179_*） */
    void (*clear_busy_error)(const epd_model_t *epd);
    bool (*busy_error)(const epd_model_t *epd);
} epd_driver_t;
```

`epd_panel.h` 提供 `static inline` 包装（空指针安全）：

```c
static inline void epd_panel_clear_busy_error(const epd_model_t *epd);
static inline bool epd_panel_busy_error(const epd_model_t *epd);
static inline bool epd_panel_can_partial(const epd_model_t *epd);
```

服务层/时钟局刷只调用包装；新增面板实现自己的 BUSY 语义，其它面板可留空。

> **兼容层（2026-11-04 实施备注）**：HAL 仍保留一个"面板 BUSY 错误标志"全局 API
> （`EPD_SetBusyError/EPD_ClearBusyError/EPD_BusyError`），用于保持既有语义：
> 面板睡眠后 `p_epd->epd == NULL` 时服务仍能清除/查询上一次刷新留下的 BUSY 超时
> 标志。服务层通过 `epd_service_busy_error_clear()/epd_service_busy_error()`
> 访问：实例存在时走 `epd_panel_*` 虚表，`epd == NULL` 时回退 HAL 全局标志。
> 后续可改为"驱动实例级状态 + 注册层"，让 HAL 不再保存面板语义状态。

### 4.2 型号注册（driver/epd_models.c）

```c
extern const epd_model_t epd_uc8179_750_Partial_bwr;
static const epd_model_t * const epd_models[] = { &epd_uc8179_750_Partial_bwr };
const epd_model_t *epd_get_model(epd_model_id_t id);   // 未命中回退 epd_models[0]
const epd_model_t *epd_init(epd_model_id_t id);
bool epd_model_supported(uint8_t id);                  // 配置校验/网页回填使用
```

新增屏幕步骤：
1. 在 `driver/` 添加 `<ic>.c/.h` 实现 `epd_driver_t`（至少 init/clear/write_image/
   write_ram/refresh/sleep；如需局刷加 init_partial/refresh_partial/diff_push）；
2. 定义 `const epd_model_t`（含 id/color/width/height）；
3. 在 `epd_models.c` 表里登记；
4. `board/epd_board.h` 给默认型号/引脚；
5. 网页侧 canvas/驱动列表、UIR2 布局尺寸、资源分区按新分辨率适配（独立于固件）；
6. 主机回归：`tests/uc8179_command_sequence_test.c` 模式新增该驱动的命令序列测试；
   真机验证全刷/局刷/睡眠电流。

### 4.3 板级配置（board/epd_board.h）

```c
#define EPD_BOARD_DEFAULT_CONFIG {0x14,0x13,0x06,0x05,0x04,0x03,0x02, \
                                  EPD_UC8179_750_BWR, 0xFF,0x12,0x07, \
                                  0x00,0x00,0x00}
#define EPD_BOARD_DEFAULT_MODEL  EPD_UC8179_750_BWR
#define EPD_BOARD_HAS_RLE        1   /* 编译期功能开关（可选） */
```

服务层默认配置、`epd_config_defaults()` 使用板级宏；换板只改 board 头 + 编译。
如未来多板同仓库，可加 `-DEPD_BOARD_<name>` 选择不同 board 头。

### 4.4 功能扩展

- 编译期功能开关（可选，不引入运行期开销）：`EPD_FEATURE_RLE`、
  `EPD_FEATURE_RLE` 关闭时网页能力串不发 `rle=1`，固件 RLE 分支可被 LTO 删除。
- 协议命令扩展：`service/epd_protocol.h` 增加 opcode，`epd_service_cmd.c`
  加 handler，`epd_cmd_policy.h` 标注 urgent/queued/display 属性；网页同步解析。
- 资源类型扩展：`storage/resource_layout.h` 分区表 + `resource_pack.py` 生成器 +
  `app/epd_resources.c` 重载回调三处同步；新增分区必须更新容量/DFU 审计。

## 5. 体积/RAM 预算规则

- 新增面板驱动：仅当型号表登记才链接；未使用驱动代码由 `--gc-sections` 丢弃。
  单个 UC8179 驱动约 1.5 KB；新增第二种面板预计 +1~2 KB（若同时保留）。
- 服务拆分后若 LTO 内联变化导致 BIN 增长，优先：
  1) 确认所有跨模块函数都声明在 internal header（避免隐式声明/重复生成）；
  2) 对只在一个模块内使用的 helper 保持 `static`；
  3) 用 `arm-none-eabi-nm --size-sort` 找异常膨胀函数；
  4) 必要时减少模块间调用层次（合并过小模块）。
- **跨 TU 公共调用点要做统一 noinline 入口**：拆分后 `app_sched_event_put` /
  `epd_work_post` 在两个以上 TU 被调用时，GCC LTO 会按调用点克隆多份
  （本次实测首版拆分 +224 B）。把队列调度收敛为两个带
  `__attribute__((noinline))` 的小函数（ARMCC/Keil 下宏为空，行为不变）后
  恢复；最终整体 BIN 变化在 ±200 B 内。
- 静态缓冲（如 `s_rle_out[236]`）必须唯一且归单一模块，避免拆分后重复分配。
- **实测结果（2026-11-04）**：重构前 55,772 B → Stage B 拆分 55,756 B（−16 B）
  → Stage C 面板接口 55,932 B（+176 B，主要来自独立型号表 TU 与面板包装的
  LTO 克隆）；静态 RAM 9,444 B 全程不变。应用区余量 1,412 B（BIN 55,932 /
  57,344），+2 KiB 栈后 RAM 余 4,164 B。新增功能需继续按预算评估。
- **后续清理实测（2026-11-04 晚）**：
  - service 五个模块的 include 收窄（26/26/27/26/26 → 22/15/20/9/11 条，按
    “头文件导出的标识符在 .c 中确有使用” + `arm-none-eabi-gcc -fsyntax-only
    -Werror` 逐条验证）：BIN 仍 55,932 B（text 55,712 / data 220），无行为变化。
  - `epd_panel_clear_busy_error/busy_error/can_partial` 改 out-of-line + noinline
    的实验：BIN 55,948 B（**+16 B**），并非预期的 −80~100 B；LTO 对原
    `static inline` 版本已做过调用点合并，故**保留 inline**（epd_panel.h 注释）。
    该负结果说明体积优化必须以真实 ARM 构建为准。

## 6. 已知折中与后续

1. `hal/EPD_driver.h` 仍包含 `EPD_config.h`（板级类型），因为 `EPD_GPIO_Load()`
   直接接收配置结构。彻底解耦需 HAL 自有的引脚结构 + board 转换层，收益有限，
   本轮保留并在代码注释标注。
2. `EPD_config_t` 是持久化/网页协议共享的 14 B 结构，拆分时**不得改字段顺序/大小**。
3. GUI/资源层（`GUI/`、`resources/`）不在本轮范围；多分辨率扩展需同步 UIR2
   布局、固定面板宏 `GUI_FIXED_PANEL_WIDTH/HEIGHT`、GFX 页缓冲。
4. 多面板同时编译会增加 Flash；若产品只需单屏，建议按构建目标选择
   `EPD_PANEL_<id>` 宏，只登记对应型号（表驱动 + 编译期裁剪）。
5. HAL 的全局面板 BUSY 标志是为保持 `epd == NULL` 语义的过渡兼容层（见 §4.1），
   后续可改为驱动实例级状态 + 注册层，进一步去掉 HAL 中的面板语义。
6. ~~service 模块目前沿用原整块 include 列表（层内依赖可见性较粗）~~：已收窄
   （2026-11-04，26/26/27/26/26 → 22/15/20/9/11 条，逐条用真实 ARM
   `-fsyntax-only -Werror` 验证；BIN 不变）。后续新增依赖时按“谁用谁 include”
   保持，不要复制整块列表。
7. 本文档之前的历史优化文档（`docs/flash-ram-optimization-20261104.md`、
   `docs/upstream-reference-optimization-20261104.md` 等）中的 `EPD/EPD_service.c`、
   `EPD/EPD_driver.c` 等是当时平铺路径，仅作历史记录；当前路径以本文档 §2 为准。

## 7. 迁移与验证（已完成）

- **阶段 A：目录分层**：45 个文件迁入 `hal/driver/board/storage/app/service`；
  Makefile `SRC_FILES`/`INC_FOLDERS`、Keil 两个 Target 的 FilePath/IncludePath、
  host test runner、Python 抽取脚本、`tools/resource_pack.py` 路径同步；全量
  `build.bat build` 通过，BIN 55,772 B 不变（纯移动）。
- **阶段 B：拆分 EPD_service.c**：2,373 → 916 行；新增
  `epd_service_display.c`(518)/`cmd.c`(542)/`control.c`(261)/`transfer.c`(313)/
  `epd_service_internal.h`(75 行, 35 个声明)；静态量归位（`s_display_retry_*`/
  `s_rle_out`/`s_anno_epd`）；3 个抽取型 Python 测试改为 5 文件依赖序拼接；
  BIN 55,756 B（−16 B），静态 RAM 9,444 B 不变。
- **阶段 C：面板扩展接口**：新增 `hal/epd_panel.h`、`driver/epd_models.c`、
  `driver/uc81xx.h`、`board/epd_board.h`；HAL 收窄为外设原语；虚表新增
  `clear_busy_error/busy_error`；`service/app` 不再 `#include "UC8179.h"`
  （grep 为 NONE）；配置校验用 `epd_model_supported`；默认板级配置改用
  `EPD_BOARD_DEFAULT_CONFIG`；BIN 55,932 B（+176 B，±200 B 内），RAM 不变。
- **验证**：`build.bat build` exit 0（HOST_OK + ARM + full HEX/DFU 合并）；
  `tests/test_application_build.py` 8/8；IRQ 强符号检查通过；GUI 金图/像素、
  TODO/时钟/资源/协议/传图主机测试全绿；`tests/test_build_config_parity.py`
  通过；协议、刷新时序、低电量/OTA 行为未改。
- **构建入口同步**（2026-11-04 追加核查）：
  - Linux：`Makefile.nRF52_linux` 的 `SRC_FILES` 与 `INC_FOLDERS` 已同步
    （24 个 EPD .c、6 个 EPD 子目录）；`build.sh`、`flash.sh`、
    `flash_debug.sh`、`flash_sysview.sh`、`Makefile.nRF52` 只按 target 调用
    该 Makefile，无需改路径；`bash -n` 全部通过。仓库 `tools/gcc` 为 Linux
    ELF 工具链，在 Linux 主机上按默认 `GNU_INSTALL_ROOT` 使用；本机是 Windows，
    由 `build.bat` 改用 `tools/windows_tools/gcc`。
  - Windows：`build.bat` 复用同一 `Makefile.nRF52_linux`（无硬编码 EPD 路径），
    已实测全量构建通过；模拟器 `Makefile`/`Makefile.win32` 的 `-IEPD` 已同步改为
    6 个 EPD 子目录。模拟器链接失败是既有问题（`HEAD` 的 Makefile 未链接
    `GUI/todo_items.c`/`GUI/calendar_annotations.c`，也没有 todo/ui/calendar 的
    host 资源桩），与本次重构无关。后续已单独修复（2026-11-04，见下条）。
  - **模拟器 host 修复**（2026-11-04 追加）：`Makefile` 与 `Makefile.win32`
    统一为同一 host 源集合（生产 GUI + `tests/stubs/` 的 todo/calendar/ui 资源桩 +
    `epd_refresh_policy.c`），并以 `GUI_USE_EXTERNAL_FONTS=0` +
    `GUI_INCLUDE_INTERNAL_FONTS=1` 使用内置字库；`emulator.c` 修正
    `BITMAPINFO` 调色板越界写（16 项颜色写入 1 项数组）并改用局部
    `BITMAPINFOHEADER+RGBQUAD[16]`；`GUI.c` 的字体占位符按编译配置收敛，
    消除 host 构建告警。已实测 `make`/`make -f Makefile.win32` 零告警生成
    `emulator.exe`，窗口标题/800x480 日历与时钟分屏渲染正常（离屏回放验证）。
    模拟器使用默认资源数据，不等价于真机外置资源。
  - Keil：`Keil/EPD-nRF52.uvprojx` 的 `nRF52811_xxAA` 与 `flash_softdevice`
    两个 Target 各新增 5 个 .c（4 个 service 模块 + `epd_models.c`），
    `IncludePath` 4 处均加入 6 个 EPD 子目录并移除失效的裸 `..\EPD`；
    EPD 源文件/包含目录与 Makefile 集合一致（脚本对比 OK），XML 可解析，
    `test_build_config_parity.py` 通过；`Keil/DFU-nRF52.uvprojx` 不引用 EPD
    源文件，无需改。
  - 其他：`tests/run_host_tests.sh`、Python 抽取脚本、`tools/resource_pack.py`
    已同步（HOST_OK）；`run_cppcheck.sh` 的 include/source 列表已更新到分层
    路径并移除过时 `DEVELOP_IN_NRF52840` 宏，`bash -n` 通过。
- **未验证**：Keil/ARMCC 未实际编译（工程路径/include/源文件已同步，noinline
  宏在 ARMCC 下为空）；真机未复测（纯重构，无行为改动，建议烧录后做一次
  全刷/时钟局刷/传图冒烟）；`tests/test_application_build.py` 只覆盖 GCC 构建。
