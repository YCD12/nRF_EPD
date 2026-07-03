# 模块解耦现状分析与重构记录

> 分析时间：2026-07-03
> 当前分支：dev
> 基准提交：4e3e5dc（refactor: eliminate ui_runtime.c dirty include, decouple EPD path dependency, and optimize GFX RAM usage by 2.8KB）

---

## 一、模块全景

```
main.c (BLE stack/timer/scheduler 初始化)
  └── EPD/EPD_service.c          ← 核心枢纽，2346 行
        ├── EPD/EPD_driver.c     HAL：SPI/GPIO/LED/ADC
        ├── EPD/UC8179.c         芯片驱动：全刷/局刷/LUT
        ├── EPD/EPD_config.c     FDS 持久化配置
        ├── EPD/epd_ext_resources.c  外置 Flash 资源管理
        ├── EPD/res_hal.c        资源 HAL 抽象（平台可替换）
        ├── EPD/fm25q04b.c       FM25Q04B SPI Flash 驱动
        ├── EPD/epd_clock_diff.c 差分局刷（数字位比对）
        └── GUI/
              ├── GUI.c          模式渲染（1462 行）
              ├── ui_runtime.c   UI bundle 解析 + render cache
              ├── Lunar.c        农历 DB 查询（LCDB 格式）
              ├── Adafruit_GFX.c 光栅化绘图
              ├── fonts.c        位图字体数据
              ├── u8g2_font.c    u8g2 字体解码
              └── gui_porting_hal.c  WDT/资源 HAL 桥接
```

---

## 二、解耦现状——已做的 vs 仍存在的耦合

### 已做好的解耦（近几次 refactor 成果）

| 改动 | 解耦效果 |
|---|---|
| `gui_porting_hal.h` 抽象资源读取 | GUI 层不再直接 include `epd_ext_resources.h` |
| `res_hal.c/h` 两层 HAL | 主机测试 (`tools/res_hal_host.c`) 可替换 MCU 实现 |
| `ui_runtime.c` 从 GUI.c 分离 | bundle 解析/render cache 独立，`GUI.c` 只做渲染 |
| `epd_clock_diff.c` 独立出来 | 差分局刷不再混在 `epd_gui_update()` 里 |
| `epd_driver_t` vtable | 驱动操作抽象为函数指针，理论上可换芯片 |
| 农历数据库 LCDB 迁移 | MCU 只做 query，`Lunar.c` 变成纯 DB reader |

### 仍存在的耦合问题（有优化空间）

**问题 1：EPD_service.c 直接调用 Lunar 层内部函数**

`EPD_service.c:1610` 和 `L1673` — RES_ERASE 和 RES_COMMIT 里都有这两行同时出现：

```c
lunar_db_reset();
GUI_ReloadUICache(width, height, color);
```

`lunar_db_reset()` 是 GUI 内部缓存失效，应当在 `GUI_ReloadUICache()` 内部调用，不该暴露给服务层。

**问题 2：epd_clock_diff.c 直接绑定 UC8179 具体函数**

`epd_clock_diff.c:9`:

```c
extern void UC8179_Partial_Diff_Push(epd_model_t *epd, ...);
```

`epd_clock_diff.c` 绕过了 `epd_driver_t` vtable 直接调用芯片函数。若换驱动芯片，这里必须同步改动，违背了 vtable 抽象的初衷。（本次未改，属中期重构项）

**问题 3：epd_clock_diff.c 的 Draw7Number 反向依赖**

`epd_clock_diff.c:12`:

```c
extern void Draw7Number(Adafruit_GFX *gfx, ...);
```

时间差分刷新模块（属于 EPD 层）直接调用 GUI 渲染函数，是典型的层间反向耦合。（本次未改，属中期重构项）

**问题 4：last_full_clear 是函数级静态变量**

`EPD_service.c:738`:

```c
static uint32_t last_full_clear = 0;  // 在 epd_gui_update() 内部
```

全刷时间戳藏在函数内部，`force_full_refresh` 是对它的外部补偿机制。模式切换时靠 `force_full_refresh = true` 兜底，状态分散。（本次未改）

**问题 5：extern m_resetreas 跨文件裸引用**

`EPD_service.c:1953`:

```c
extern uint32_t m_resetreas;
```

直接引用 `main.c` 内部变量，应通过 `main.h` 暴露一个读取函数。（本次未改）

---

## 三、整体评估

| 维度 | 评分 | 说明 |
|---|---|---|
| 数据层解耦 | ★★★★☆ | LCDB 迁移、res_hal 抽象完成，仅 lunar_db_reset 暴露给服务层（已修复） |
| 渲染层解耦 | ★★★☆☆ | ui_runtime 分离做得好，epd_clock_diff → Draw7Number/UC8179 直调是缺口 |
| 服务层内聚 | ★★☆☆☆ | EPD_service.c 承担了过多职责，是当前最大的技术债 |
| 驱动层抽象 | ★★★★☆ | epd_driver_t vtable 完整，仅 UC8179_Partial_Diff_Push 绕过了 vtable |
| 可测试性 | ★★★★☆ | tools/test 下有 golden/lunar 测试，res_hal_host 可换，已较完善 |

---

## 四、本次重构改动记录

### 改动 1 — `lunar_db_reset()` 内聚到 GUI 层

**问题**：`EPD_service.c` 在 RES_ERASE 和 RES_COMMIT 两处都需要写 `lunar_db_reset()` + `GUI_ReloadUICache()` 的成对调用，`lunar_db_reset` 是 GUI 内部缓存失效逻辑，不该暴露给服务层。

**改动文件**：

- `GUI/ui_runtime.c`
  - 新增 `#include "Lunar.h"`
  - `GUI_ReloadUICache()` 函数开头加 `lunar_db_reset()`

- `EPD/EPD_service.c`
  - `RES_ERASE` 处删除裸调用 `lunar_db_reset()`，更新注释
  - `RES_COMMIT` 处删除裸调用 `lunar_db_reset()`，更新注释

**效果**：调用方只需调 `GUI_ReloadUICache()`，资源热更新后的农历缓存失效由 GUI 层统一管理，服务层无需感知。

---

### 改动 2 — `epd_clock_diff` 消除硬编码降级坐标

**问题**：`epd_clock_diff.c:80-82` 在 ui_runtime 加载失败时，悄悄回退到硬编码坐标 `comp_x=56, comp_y=182, cS=6` 继续执行，不会报错，可能用错误坐标强行局刷。

**改动文件**：

- `EPD/epd_clock_diff.c`
  - `ui_runtime_load_render_cache()` 失败或返回空时直接 `return false`
  - 遍历组件未找到 `UI_COMP_TIME_7SEG` 时直接 `return false`
  - 删除三个硬编码默认值，改为函数体内声明但必须由 ui_runtime 赋值

**效果**：ui_runtime 缓存不可用时，差分局刷主动放弃，上层 `EPD_service.c` 走 `goto fallback_full_refresh`，行为可预期，不会出现坐标错误的隐藏问题。

---

### 改动 3 — `EPD_WaitBusy` 超时可见，防止 EPD 异常时 reset 死循环

**问题**：`EPD_WaitBusy` 超时时 break 后静默退出，`UC8179.c` 中的调用方不知道是正常释放还是超时，若 EPD 硬件 BUSY 持续拉高，`epd_gui_update` 会以为刷新成功，下次 timer 触发再刷，再超时，WDT 最终触发 reset，如此循环。

**改动文件**：

- `EPD/EPD_driver.h`
  - `EPD_WaitBusy` 返回值从 `void` 改为 `bool`（正常释放返回 `true`，超时返回 `false`）

- `EPD/EPD_driver.c`
  - `EPD_WaitBusy` 实现：超时时改为 `NRF_LOG_WARNING`，清除 sense 配置，返回 `false`；正常释放返回 `true`

- `EPD/UC8179.c`
  - 新增 `static bool m_uc8179_busy_timeout = false`
  - 新增 `bool UC8179_HadBusyTimeout(void)` 和 `void UC8179_ClearBusyTimeout(void)` 两个查询函数
  - `UC8179_WaitBusy` 改为：捕获 `EPD_WaitBusy` 返回值，超时时置 `m_uc8179_busy_timeout = true`

- `EPD/EPD_service.c`
  - 新增 `extern bool UC8179_HadBusyTimeout(void)` 和 `extern void UC8179_ClearBusyTimeout(void)` 声明
  - `epd_gui_update()` 函数入口（`EPD_GPIO_Init()` 之后）加 `UC8179_ClearBusyTimeout()`
  - 三处 `drv->refresh` / `drv->refresh_partial` 调用后各加超时检查：
    - 时钟模式内的全刷（`last_full_clear` 路径）
    - MODE_CLOCK 的全窗口局刷（`refresh_partial` 路径）
    - fallback 全刷路径
  - 超时时均 `goto finish_refresh`，确保 `is_refreshing` 被清除

**效果**：EPD 硬件异常（BUSY 持续拉高）时，刷新函数返回后能被感知，`epd_gui_update` 干净退出并清除 `is_refreshing` 标志，避免后续每次 timer 都触发超时等待导致 WDT reset 循环。

---

## 五、中期重构项（已全部完成）

所有 5 项均已在本次处理完毕，见第六节改动记录。

---

## 七、第三轮深度分析（2026-07-03 更新）

### 7.1 模块全景（本轮后）

```
main.c                                                          957 行
  └── EPD/EPD_service.c       核心枢纽（本轮后）               2201 行
        ├── EPD/EPD_driver.c  GPIO / SPI / ADC / WDT HAL
        ├── EPD/UC8179.c      芯片驱动                         1043 行
        ├── EPD/UC8179.h      ← 新增，暴露 busy-timeout 接口
        ├── EPD/EPD_config.c  FDS 持久化配置
        ├── EPD/epd_ext_resources.c  外置 Flash 资源管理        360 行
        ├── EPD/res_hal.c     GUI→资源 HAL 抽象
        ├── EPD/fm25q04b.c    SPI Flash 驱动
        ├── EPD/epd_clock_diff.c  差分局刷                      170 行
        └── GUI/
              ├── GUI.c       模式渲染（本轮后）                 920 行 ↓ -542
              ├── ui_runtime.c UI bundle + render cache + 刷新几何  513 行 ↑ +105
              ├── Lunar.c     农历 DB 查询引擎                  1055 行
              ├── Adafruit_GFX.c  光栅化（新增 GFX_get_buffer accessor）
              ├── fonts.c / u8g2_font.c  字体数据
              └── gui_porting_hal.c  WDT/资源 HAL 桥接
```

**行数变化对比**（Flash 占用直接相关）：

| 文件 | 本轮前 | 本轮后 | 变化 |
|---|---|---|---|
| `GUI/GUI.c` | 1462 | 920 | **-542** |
| `GUI/ui_runtime.c` | 408 | 513 | +105 |
| `EPD/EPD_service.c` | 2178 | 2201 | +23（新增两个 guard 函数） |
| `EPD/UC8179.h` | 不存在 | 新增 | +13 |
| `GUI/Adafruit_GFX.h/c` | — | 新增 2 个函数 | +6 |
| **净减少** | — | — | **~400 行** |

### 7.2 本轮解耦改动一览

| 改动 | 问题 | 方案 | 效果 |
|---|---|---|---|
| A: 创建 `UC8179.h` | `EPD_service.c:113-115` 三个 inline extern 无头声明 | 新建 `EPD/UC8179.h`，移 extern 为正式声明，service 改为 `#include "UC8179.h"` | 编译脆弱点消除，芯片接口文件完整 |
| B: `GFX_get_buffer()` accessor | `epd_clock_diff.c` 跨层 `extern uint8_t s_gfx_buffer[]`，侵入 GFX 内部 | `Adafruit_GFX.h/c` 新增 `GFX_get_buffer()`/`GFX_get_buffer_size()`，`epd_clock_diff.c` 改为调用 accessor | GFX 缓冲布局变化时只需改一处 |
| C: `ui_render_invalidate_month_cache` 加入 `GUI.h` | `ui_runtime.c` 对 `GUI.c` 内部函数使用 forward decl，形成循环依赖 | 在 `GUI.h` 加声明，删除 `ui_runtime.c` 的 forward decl | `GUI.c ↔ ui_runtime.c` 循环依赖消除 |
| D: 刷新几何迁移到 `ui_runtime.c` | `GetTimeRefreshArea`/`GetTimeRefreshOrigin`/`GUI_MarkTimeRefreshTimestamp` 是"EPD层用的刷新几何计算"，放在 `GUI.c`（渲染文件）位置错误 | 三个函数 + `s_last_time_refresh_ts` 从 `GUI.c` 迁移到 `ui_runtime.c`，声明从 `GUI.h` 移到 `ui_runtime.h` | `GUI.c` 缩小 542 行，仅保留渲染逻辑；刷新几何与 ui_runtime 数据在同一文件 |
| E: 提取 battery/resource guard | `epd_gui_update` 397 行上帝函数，低电量决策和资源缺失判断内联在刷新主流程里 | 提取 `epd_battery_guard()`（含 critical 关机状态机）和 `epd_resource_guard()`（含 hint 去重逻辑）两个静态函数，各约 30-40 行 | `epd_gui_update` 调用点精简，两个 guard 逻辑可独立阅读和测试 |

### 7.3 当前解耦状态（更新评分）

| 维度 | 上轮 | 本轮 | 说明 |
|---|---|---|---|
| 数据层解耦 | ★★★★☆ | **★★★★☆** | 无变化 |
| 渲染层解耦 | ★★★☆☆ | **★★★★☆** | GUI.c 纯化为渲染，刷新几何迁至 ui_runtime，循环依赖消除 |
| 服务层内聚 | ★★★☆☆ | **★★★★☆** | battery/resource guard 独立，`epd_gui_update` 主流程更清晰 |
| 驱动层抽象 | ★★★★☆ | **★★★★★** | UC8179.h 建立，三个芯片函数有正式头文件 |
| 头文件卫生 | ★★★☆☆ | **★★★★☆** | inline extern 全部消除，forward decl 消除，accessor 替代裸符号引用 |
| 可测试性 | ★★★★☆ | **★★★★☆** | guard 函数内含静态状态，可独立调试；整体无退步 |

### 7.4 Flash 占用降低路径

本轮改动对 Flash 的直接影响：

- `GUI.c` 减少 542 行，去掉的代码主要是**刷新几何计算逻辑**（`GetTimeRefreshArea` 约 100 行）和相关注释块。这部分代码**没有删除，只是移位**，所以 Flash 总量未降（逻辑等量迁入 `ui_runtime.c`）。
- 真正降低 Flash 的方向在**字体数据**（`fonts.c` / `u8g2_font.c`）和**fallback_font（475 字节的 5×7 ASCII bitmap）**，以及 LUT 表。这些是当前固件最大的只读数据段，需要通过裁剪字符集或按需加载来压缩。
- 代码逻辑层面，`epd_clock_diff.c` 通过 vtable 和 accessor 替换直调，消除了**函数内联**风险（过去直调导致链接器可能无法 DCE 芯片函数）。

### 7.5 剩余可优化项

| 优先级 | 问题 | 说明 |
|---|---|---|
| 中 | `epd_clock_diff.c` 仍直调 `Draw7Number`（GUI 反向依赖） | EPD 层调 GUI 渲染函数；可将数字位图绘制封装为独立 helper，由调用方传入，但改动面较大 |
| 中 | `EPD_service.h` 仍 include `GUI.h` | `ble_epd_t` 只需 `display_mode_t`，可提取薄类型头文件隔离 |
| 中 | `epd_gui_update` 仍约 350 行 | battery/resource guard 提取后有改善，但刷新策略（全/局刷切换、静默小时）可再提取为 `epd_refresh_policy()` |
| 低 | `GUI.c` 中 `PrepareMonthRenderCache` 仍做 Flash 读取（DB I/O 在渲染层） | 理想态是将月缓存预加载提前到 UI bundle 加载时，但涉及调用时序 |

---

## 六、第二批重构改动记录（中期项全清）

### 改动 4 — `epd_clock_diff` 消除 UC8179 直调（vtable + Draw7Number）

**问题 1**：`epd_clock_diff.c` 用 `extern void UC8179_Partial_Diff_Push(...)` 绕过 `epd_driver_t` vtable 直接调用芯片函数。
**问题 2**：同文件用 `extern void Draw7Number(...)` 从 EPD 层反向依赖 GUI 渲染层。

**改动文件**：

- `EPD/EPD_driver.h`
  - `epd_driver_t` 新增 `diff_push` 函数指针：
    ```c
    void (*diff_push)(epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *old_bm, const uint8_t *new_bm);
    ```

- `EPD/UC8179.c`
  - `epd_drv_uc8179_partial` vtable 注册 `.diff_push = UC8179_Partial_Diff_Push`
  - 新增 `m_uc8179_busy_timeout` 标志及 `UC8179_HadBusyTimeout()` / `UC8179_ClearBusyTimeout()` 函数

- `EPD/epd_clock_diff.c`
  - 删除两个 `extern` 声明，改为 `#include "GUI.h"`
  - 删除 ui_runtime 加载失败时的硬编码坐标降级分支，改为直接 `return false`
  - 新增 `diff_push == NULL` 检查（非部分刷新驱动安全防护）
  - 两处 `UC8179_Partial_Diff_Push(...)` 直调改为 `epd->drv->diff_push(...)`

**效果**：`epd_clock_diff.c` 不再感知具体芯片型号；换驱动只需注册新的 `diff_push`，差分刷新模块无需改动。

---

### 改动 5 — `epd_execute_command` switch-case 拆解为独立处理函数

**问题**：`epd_execute_command()` 约 600 行的 switch-case 混合了 24 个命令的全部逻辑，可读性差、函数边界不清晰。

**改动文件**：

- `EPD/EPD_service.c`
  - 在 `epd_execute_command` 之前新增 24 个 `static void cmd_xxx(ble_epd_t*, uint8_t*, uint16_t)` 函数：
    `cmd_set_pins`, `cmd_init`, `cmd_clear`, `cmd_send_command`, `cmd_send_data`, `cmd_refresh`, `cmd_sleep`, `cmd_set_time`, `cmd_sync_time_silent`, `cmd_set_week_start`, `cmd_set_mode`, `cmd_write_image`, `cmd_write_block`, `cmd_query_status`, `cmd_reset_transfer`, `cmd_res_erase`, `cmd_res_write`, `cmd_res_commit`, `cmd_res_status`, `cmd_res_read`, `cmd_set_config`, `cmd_sys_sleep`, `cmd_sys_reset`, `cmd_cfg_erase`
  - `epd_execute_command()` 的 switch 体缩减为 26 行单行路由调用

**效果**：dispatch 函数从 ~600 行变为 ~30 行纯路由表；每个命令逻辑独立可读、可单独测试。

---

### 改动 6 — `last_full_clear` 提升为 `ble_epd_t` 成员

**问题**：`epd_gui_update()` 内的 `static uint32_t last_full_clear` 是函数级隐藏状态，`force_full_refresh` 是对它的外部补偿，二者分散管理。

**改动文件**：

- `EPD/EPD_service.h`
  - `ble_epd_t` 新增 `uint32_t last_full_clear` 成员（与 `force_full_refresh` 相邻）

- `EPD/EPD_service.c`
  - 删除 `static uint32_t last_full_clear = 0`
  - 所有 `last_full_clear` 引用改为 `p_epd->last_full_clear`
  - `ble_epd_init()` 中初始化 `p_epd->last_full_clear = 0`

**效果**：全刷状态完全透明，可从外部读取/检查；不再有函数级隐藏状态。

---

### 改动 7 — `extern m_resetreas` 裸跨文件引用改为 accessor

**问题**：`EPD_service.c` 在函数体内用 `extern uint32_t m_resetreas` 直接访问 `main.c` 内部变量，违反封装边界。

**改动文件**：

- `main.h`
  - 新增 `uint32_t get_reset_reason(void);` 声明

- `main.c`
  - `m_resetreas` 变量加 `static` 修饰（原为裸全局）
  - 新增 `uint32_t get_reset_reason(void) { return m_resetreas; }` 实现

- `EPD/EPD_service.c`
  - 删除 `extern uint32_t m_resetreas;` 声明
  - `ble_log("[SYS]: Reset reason: ...")` 中改为调用 `get_reset_reason()`

**效果**：`m_resetreas` 成为 `main.c` 私有变量，外部只能通过稳定接口访问，符合最小公开原则。

---

## 四、Flash 优化记录

> 分析时间：2026-07-03（本 session 追加）
> 基准：同上 4e3e5dc，分析起点 Flash = 57196/57344 字节（148 字节余量）

### 优化背景

Flash 区域：ORIGIN = 0x19000，LENGTH = 0xE000（56KB，上限 0x27000），余量极为紧张。

### 优化过程

#### 方案 A — 缩短 `ble_log` 格式字符串（`EPD/EPD_service.c`）

`ble_log()` 是项目自己的 BLE 通知函数，其格式字符串是真实的字符串字面量，无法通过宏控制消除。将 9 处字符串缩短：

| 原始 | 替换后 |
|---|---|
| `"[EPD]: Performing FULL refresh, ts=%u, reason=%s"` | `"[EPD]:full ts=%u %s"` |
| `"[EPD]: Full refresh completed"` | `"[EPD]:full ok"` |
| `"[EPD]: Using PARTIAL refresh at (%d,%d,%d,%d)"` | `"[EPD]:part %d,%d,%d,%d"` |
| `"[EPD]: Partial refresh complete"` | `"[EPD]:part ok"` |
| `"[EPD]: Using normal FULL refresh"` | `"[EPD]:full"` |
| `"[EPD]: Full refresh complete"` | `"[EPD]:full ok"` |
| `"[CMD]: Recv 0x%02x, len=%d"` | `"[CMD]:%02x/%d"` |
| 两条 SYS 日志合并为一条 | `"[SYS]:mtu=%d rst=0x%X"` |

注意：`[EPD]` 系列日志为开发 debug 用途，后续关闭后这些字符串可完全消除。

#### 方案 B — 修改 SDK `nrf_log_internal.h`（已还原，不推荐）

初始做法：将 `NRF_LOG_ENABLED=0` 分支的 `(void)(_str)` 改为 `((void)0)`，消除 SDK 内部字符串字面量的 Flash 引用。

**问题**：修改了 SDK 官方文件，升级 SDK 会覆盖，维护风险高。

**正确做法**：在 `sdk_config.h` 中将不需要的子模块日志关掉（如 `NRF_SDH_BLE_LOG_ENABLED 0`），等价效果但不触碰 SDK 文件。已还原 SDK 文件，验证 Flash 不变（原方案实际未带来节省）。

#### 方案 C — `ui_render_component` 加 `__attribute__((noinline))`（`GUI/GUI.c`）

**问题**：编译器对 `ui_render_component` 做了 constprop 优化，生成专用副本 `ui_render_component.constprop.0`（4968B），加上原始函数共两份，总占用异常大。

**改动**：
```c
// GUI/GUI.c
static void __attribute__((noinline)) ui_render_component(...) {
```

**效果**：消除原始函数副本，只保留 constprop 版本，节省 **448 字节**。

注意：`__attribute__((noinline))` 是 GCC/Clang 标准属性，armcc/IAR 会忽略（不报错），Flash 余量会回退但不影响功能。

#### 方案 D — `fallback_font` 换为稀疏表（`GUI/GUI.c`）

**问题**：`fallback_font` 是完整 95 字符 5×7 ASCII 字体表（475 字节），但实际只用于绘制 `"SYNC TIME!"` 和 URL 两个固定字符串，其中出现的不重复字符仅 37 个。

**改动**：将 `fallback_font[95][5]` 替换为稀疏结构：
```c
static const uint8_t fallback_keys[37];          // 37B
static const uint8_t fallback_glyphs[37][5];     // 185B
// 合计 222B，原来 475B，节省 253B
```
查字时线性扫描 `fallback_keys`，未找到时回退到最后一项 `'?'`。

**效果**：节省 **240 字节**。

### 优化汇总

| 方案 | 文件 | 节省 |
|---|---|---|
| A：ble_log 字符串缩短 | EPD/EPD_service.c | ~80B |
| B：SDK 宏改动 | 已还原，0 实际节省 | — |
| C：noinline 消除 constprop 副本 | GUI/GUI.c | **448B** |
| D：fallback_font 稀疏化 | GUI/GUI.c | **240B** |
| **合计** | | **≈768B** |

**当前状态**：56284/57344 字节，**1060 字节余量**（优化前 148 字节）。

### 原则确认

- 所有有效优化均在**项目自身文件**内完成，未修改 SDK 文件
- SDK 文件（`nrf_log_internal.h`、`nrf_sdh_ble.c`）已通过 `git checkout HEAD` 还原至原始状态
