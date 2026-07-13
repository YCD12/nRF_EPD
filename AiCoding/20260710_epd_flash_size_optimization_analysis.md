# nRF52811 墨水屏固件 Flash / RAM 优化分析报告（2026-07-13 修订）

## 0. 结论摘要

本报告以生产配置的最终 ELF/BIN 为唯一体积判定依据，不再把 Non-LTO 对象文件大小直接等同于最终固件占用。

当前生产配置：

```text
USE_RTT=0
USE_SYSVIEW=0
USE_BLE_LOG=0
DEBUG_LAYOUT=0
OPT="-Os -g3 -flto"
```

最终实测及实验结果：

| 指标 | 当前 HEAD/最终保留代码 | Review 前本地实验 | 最小体积实验（未保留） |
| --- | ---: | ---: | ---: |
| GNU `size` text | **47,768 B** | 47,640 B | 47,064 B |
| GNU `size` data | 280 B | 280 B | 280 B |
| 实际 Flash，即 text + data / `.bin` | **48,048 B** | 47,920 B | 47,344 B |
| bss | **8,932 B** | 8,932 B | 8,932 B |
| 链接器预留栈 | **2,048 B** | 2,048 B | 2,048 B |

评审结论是：当前 9 KiB 以上 Flash 余量已经足够，704 B 的实验收益不足以覆盖 SDK 修改、公共接口变化和 UI 行为变化的维护风险。因此所有代码体积优化均已撤回，工作区只保留本文档。

应用 Flash 链接区为 56 KiB，即 57,344 B。当前还剩：

```text
57,344 - 48,048 = 9,296 B
```

当前 Flash 占用率为 83.79%，安全余量为 16.21%。

应用 RAM 链接区为 15,656 B。按 `.data + .bss + stack` 计算：

```text
280 + 8,932 + 2,048 = 11,260 B
15,656 - 11,260 = 4,396 B
```

静态 RAM 与预留栈合计占用率为 71.92%，链接期余量为 4,396 B。该余量不等价于运行时栈安全结论，仍需真机栈高水位测试。

实验中最有效的候选方向是消除 WDT 初始化中的 64 位除法，可避免链接 `__aeabi_uldivmod` / `__udivmoddi4`，单项净节省约 742 B。但它需要修改 Nordic SDK 并做真机 WDT/Keil 回归，本轮不实施。手动屏蔽未引用的 GFX 函数和把命令白名单改成区间判断均为 0 B 收益，也不应实施。

---

## 1. 测量口径

### 1.1 为什么不能只看对象文件 `.text`

生产构建同时启用了：

```text
-Os
-flto
-ffunction-sections
-fdata-sections
-Wl,--gc-sections
--specs=nano.specs
```

因此最终链接器会：

1. 删除没有引用的函数和只读数据；
2. 跨编译单元内联、常量传播和合并函数；
3. 把多个源文件的逻辑折叠进 LTO 生成的 `ltrans` 对象；
4. 只从静态库中抽取真正被引用的成员。

Non-LTO `.o` 大小可以用来理解源文件复杂度，但不能回答“这个文件实际占了多少生产 Flash”。例如三角形和位图函数存在于 `Adafruit_GFX.c.o`，但在基线最终 map 中已经不存在，增加条件编译不会继续减少最终 BIN。

### 1.2 Flash 正确口径

GNU `size` 输出：

```text
text   data   bss
```

Flash 装载量至少应按：

```text
Flash = text + data
```

`.data` 的运行地址在 RAM，但初始值同时存储在 Flash，因此不能从 Flash 用量中忽略。当前 `.bin` 大小与 `text + data` 都是 48,048 B。

`.hex` 文件本身是 ASCII 容器，文件大小不能用作 Flash 占用量。

### 1.3 RAM 正确口径

当前应用 RAM 静态预算由以下部分组成：

```text
.data          280 B
.bss         8,932 B
heap             0 B
reserved stack 2,048 B
```

还必须额外关注：

- 中断嵌套时的实际栈峰值；
- `GFX_printf` 的 96 B 局部格式化缓冲区；
- `Draw7Number`、资源 CRC、BLE 响应函数的局部数组；
- SoftDevice 回调和应用调度器调用链；
- 编译器内联造成的栈帧合并。

因此“链接成功”只能证明静态区没有越界，不能证明 2 KiB 栈在所有真机路径下都安全。

### 1.4 可复现命令

生产构建：

```bash
CCACHE_DIR=/tmp/epd-ccache \
CCACHE_TEMPDIR=/tmp/epd-ccache/tmp \
make -f Makefile.nRF52_linux rebuild \
  USE_RTT=0 USE_SYSVIEW=0 USE_BLE_LOG=0 DEBUG_LAYOUT=0
```

查看整体体积：

```bash
tools/gcc/bin/arm-none-eabi-size _build/nrf52811_xxaa.out
tools/gcc/bin/arm-none-eabi-size -A _build/nrf52811_xxaa.out
stat -c '%s' _build/nrf52811_xxaa.bin
```

查看最终存活符号：

```bash
tools/gcc/bin/arm-none-eabi-nm --radix=d -S --size-sort \
  _build/nrf52811_xxaa.out
```

检查高成本运行库并与基线 allowlist 对比：

```bash
tools/gcc/bin/arm-none-eabi-nm _build/nrf52811_xxaa.out | \
  rg '__aeabi_[df]|__aeabi_uldivmod|__udivmoddi4|sprintf|snprintf|vsprintf|vsnprintf'
```

当前预期会看到 WDT SDK 引入的 `__aeabi_uldivmod` 和 `__udivmoddi4`；不应看到软浮点或 stdio 格式化符号。自动检查应锁定这两个已知符号的数量/大小，阻止新增调用，而不是要求当前基线为零。

---

## 2. 当前最终 Flash 热点

LTO 会改变函数归属，下面列的是最终 ELF 中仍真实存在的符号，而不是源文件对象大小。

| 最终符号 | 大小 | 说明 | 优化判断 |
| --- | ---: | --- | --- |
| `ui_render_component.constprop.0` | 6,428 B | UI 组件分发以及被 LTO 合入的日历、文本和提示页逻辑 | 最大业务热点，但不能按单个函数盲目裁剪 |
| `main` | 2,648 B | BLE、定时器、广播、WDT 和启动流程 | 主要是初始化代码，需按功能拆分分析 |
| `epd_execute_command.part.0` | 2,344 B | BLE 命令执行分发及各命令实现 | 协议核心路径，优先保证兼容性 |
| `epd_gui_update` | 2,048 B | 刷新策略、低电保护、全刷/局刷分支 | 功能密集，不适合只靠合并 `if` 优化 |
| `queue_process.lto_priv.0` | 1,204 B | SDK/FDS 队列处理 | SDK 功能成本 |
| `ble_advdata_encode` | 916 B | Nordic 广播数据编码 | 可评估固定广播包替代，但风险较高 |
| `Draw7Number.constprop.0` | 628 B | 七段数字渲染 | 保持现有实现；候选栈优化不值得接口风险 |
| `m_epd` 对应代码路径 | 多符号 | 服务、队列、刷新状态 | 更适合通过功能配置裁剪，而非表达式微调 |
| `ui_runtime_load_render_cache` | 616 B | 外部 UI Bundle 加载和校验 | 是数据驱动 UI 的必要成本 |
| `GUI_ReloadUICache` | 564 B | 模式缓存重载 | 必要路径 |
| `lunar_db_get_record` | 548 B | 农历数据库访问 | 可优化读取次数，不宜退回大常量表 |
| `GFX_print` | 538 B | UTF-8 字符输出和光标推进 | 文本渲染核心 |
| `ble_advertising_start.part.0` | 500 B | 广播状态机 | SDK 功能成本 |

注意：`ui_render_component.constprop.0` 较大，不代表把所有 helper 强制 `noinline` 就会缩小。当前很多 helper 只有一个调用点，内联后反而能消除通用参数和不可达分支。任何 `noinline` 策略都必须 A/B 测量最终 BIN。

---

## 3. 当前 RAM 组成

最终 ELF 中主要静态对象如下：

| 对象 | 大小 | 作用 | 是否建议立即缩减 |
| --- | ---: | --- | --- |
| `s_gfx_buffer` | 4,800 B | GFX 分页缓冲、局刷新旧数字临时区 | 暂不；可做可配置实验 |
| `m_epd` | 640 B | BLE EPD 服务状态、传输状态、命令队列 | 暂不；涉及协议并发行为 |
| `m_ui_render_cached_components` | 432 B | 当前模式最多 18 个组件缓存 | 暂不；当前最大模式有 17 项 |
| `m_u8g2_ext_font_caches` | 432 B | 外部字体缓存 | 可分析命中率后调整 |
| `m_month_render_cache` | 410 B | 31 天标签/节日缓存 | 不建议；用于避免分页重复读外置 Flash |
| `m_manifest` | 396 B | 外置资源 Manifest | 协议必要常驻数据 |
| `m_advertising` | 248 B | Nordic 广播状态 | SDK 必要状态 |
| `APP_SCHED_BUF` | 176 B | 应用调度器事件区 | 可结合峰值队列审计 |
| `m_queue_data` | 140 B | SDK 队列 | 需要事件峰值证据后再改 |
| `m_fifo_data` | 140 B | SDK FIFO | 同上 |
| `m_ui_cached_modes` | 128 B | 最多 8 个 UI 模式 | 当前资源只有 4 模式，但保留扩展余量 |
| `m_req_fifo_data` | 88 B | SoftDevice 请求 FIFO | SDK 必要状态 |

### 3.1 `s_gfx_buffer` 的可选缩减

当前 4,800 B 是最大 RAM 单体。对 800 像素宽屏：

- 单色每行 100 B，可容纳 48 行；
- 三色模式黑/彩平面平分，等效每页约 24 行；
- 七段数字差分刷新需要同时存放 old/new 两个局部位图。

可选实验档位：

| 缓冲区 | RAM 节省 | 预期影响 |
| ---: | ---: | --- |
| 4,096 B | 704 B | 分页次数增加约 17%，风险相对可控 |
| 3,200 B | 1,600 B | 分页和 SPI 调用显著增加，需测刷新耗时 |
| 2,400 B | 2,400 B | 可能影响最大七段局刷临时区，不建议直接使用 |

实施前必须验证所有屏幕宽度、三色模式、差分局刷和资源缺失页面，不能只验证 800×480 日历。

### 3.2 UI 缓存为什么不能简单删除

GFX 使用分页渲染，同一页面会多次调用组件绘制。如果删除月历和组件缓存，每一页都可能重新读取外置 SPI Flash、重新计算农历和节日，虽然省几百字节 RAM，却会增加刷新时间、SPI 活跃时间和功耗。

RAM 优化应优先考虑：

1. 降低局部函数峰值栈；
2. 复用生命周期不重叠的临时缓冲；
3. 根据运行时峰值缩小队列；
4. 最后才是删除用于减少 I/O 的长期缓存。

---

## 4. 已有优化与未采纳实验

### 4.1 生产日志按需裁剪

`USE_BLE_LOG=0` 时：

- `ble_log(...)` 退化为空宏；
- `long_to_str`、`mini_vsnprintf` 和 BLE 日志发送代码不参与编译；
- 调用点中的日志格式字符串可以被预处理器直接删除；
- RTT/SystemView 相关源文件不加入生产构建。

这是有效的功能级裁剪。后续新增日志时必须继续通过同一配置开关，而不是只依赖 `NRF_LOG_*` 空宏。

### 4.2 `nrf_strerror` 生产裁剪

生产配置定义：

```text
NRF_LOG_ENABLED=0
NRF_STRERROR_ENABLED=0
```

最终 ELF 中没有 Nordic 错误字符串字典。当前仍把 `nrf_strerror.c` 放入源文件列表，但最终代码为 0 B；后续可以按构建配置完全不编译该文件，以缩短构建时间，不过不会继续减少最终 Flash。

### 4.3 WDT 64 位除法候选实验（未采纳）

Nordic `nrfx_wdt_init` 原实现：

```c
uint64_t ticks = (p_config->reload_value * 32768ULL) / 1000;
```

即使项目完全没有浮点，这一行也会引入：

```text
__aeabi_uldivmod   约 48 B
__udivmoddi4      702 B
```

实验方案使用恒等变换：

```text
floor(ms × 32768 / 1000)
= (ms / 1000) × 32768 + floor((ms % 1000) × 32768 / 1000)
```

对所有有效 WDT reload 值结果完全一致，且中间量不溢出 32 位。Cortex-M4 有整数 `UDIV` 指令，所以实验固件不再需要 64 位除法运行库。

没有通过给 `nrfx_wdt.c` 开启 LTO 来达到该目的，因为实验发现 WDT IRQ 强符号可能被 LTO 裁掉，只剩启动文件的 weak 默认处理器。虽然直接修改换算表达式的实验可净省约 742 B 并保留强 IRQ，但它修改了 Nordic SDK，仍需要真机 WDT 和 Keil 回归。在当前还有 9,296 B Flash 余量的情况下，收益不足以覆盖维护风险，因此代码已恢复为 SDK 原实现。

### 4.4 七段数字接口和栈优化实验（未采纳）

原接口接受带符号 `int/char`，内部存在 `abs()`、隐式负数语义和 1～10 的动态修正。真实业务只绘制非负的小时、分钟和单个数字。

实验接口收紧为：

```c
uint16_t n
uint8_t  cS   // 1..10
uint8_t  nD   // 1..10
uint16_t color
```

并在入口拒绝无效比例/位数。坐标表从 32 位元素改为 16 位元素，单次调用栈帧从约 172 B 降到约 116 B，减少约 56 B 峰值栈，但 Flash 增加约 40 B，并改变公共接口与原有输入语义。该收益不值得 API 回归风险，因此未保留。

### 4.5 时间同步提示页轻量化实验（未采纳）

实验保留了完整同步地址：

```text
https://ycd12.github.io/EPD-nRF5_DYC/
```

实验删除圆角对话框尺寸计算，改成固定高度的白色横带加两行居中文本。它可以减少少量代码，但改变了用户可见页面。当前空间并不紧张，因此最终恢复原圆角提示框；除非产品明确批准视觉变化，否则不应以体积优化名义修改 UI。

该实验曾验证：

- 不会把文字直接叠加在日历内容上；
- 不会丢失真正的同步入口；
- 比完整圆角弹框代码更小；
- 不再复用 `GFX_LITE_MODE` 改变产品 UI 行为。

### 4.6 GUI 黄金图扩展建议（本轮未修改测试代码）

当前仓库中的自动黄金图只覆盖普通日历。实验期间临时扩展并测得以下五个当前 HEAD 场景，可作为后续正式补测试时的参考：

| 场景 | 覆盖重点 | CRC32 |
| --- | --- | --- |
| calendar | 普通日历、月历缓存 | `0x8678EADB` |
| clock_calendar | 6 倍七段数字、组合布局 | `0xA0897B6D` |
| clock | 10 倍七段数字、全屏时钟 | `0x629DA83A` |
| time_unsynced | 2025 年原始圆角未校时提示 | `0x44A38726` |
| low_battery | 低电量专用页面 | `0xC7F850EB` |

本轮按“只保留文档”的决策恢复了测试代码，因此上述后四个场景尚未进入自动回归。后续若真正修改 GUI，应先独立提交测试覆盖，再提交视觉或体积优化，避免测试和行为变化混在同一批修改中。

---

## 5. 已验证无效或不应实施的方向

### 5.1 手动屏蔽未引用的 GFX API

三角形、普通位图、`GFX_drawUTF8` 等未使用函数在基线最终 map 中已经被 `--gc-sections`/LTO 删除。增加 `#if GFX_LITE_MODE`：

- 最终 BIN 节省 0 B；
- 会改变公共头文件 API；
- 硬编码宏会导致 `-DGFX_LITE_MODE=0` 在 `-Werror` 下编译失败；
- 增加完整模式和精简模式的测试矩阵。

因此这些条件宏不应进入最终代码。

不能删除的真实 GFX 能力包括：

- `GFX_drawCircle` / `GFX_fillCircle`：月历当天标记；
- `GFX_drawRoundRect` / `GFX_fillRoundRect`：低电和资源缺失页面；
- `GFX_drawDottedLine`：月历网格；
- `GFX_write`：`GFX_printf` 的最终输出路径，并被 GUI 直接调用；
- `GFX_getUTF8Width`：大量文本居中和右对齐。

### 5.2 把命令白名单改成枚举区间

把多个 `cmd == ...` 改成 `cmd >= first && cmd <= last` 在最终 LTO 固件中节省 0 B，同时让行为隐式依赖枚举连续性。以后插入保留命令可能意外改变紧急性或资源锁行为。因此恢复显式白名单。

### 5.3 仅删除 `-lm`

当前 map 会显示 `LOAD ... libm.a`，但没有任何 libm 成员被抽取。删除 `-lm` 可以作为构建配置清理和防止未来直接调用数学函数的措施，但当前 Flash 收益是 0 B。

基本 `float/double` 四则运算由 libgcc helper 支持，并不由 `-lm` 开关“阻断”。要防止误引入软浮点，应在最终 ELF 中检查：

```text
__aeabi_f*
__aeabi_d*
```

### 5.4 替换标准 `sprintf`

当前生产 ELF 不包含：

```text
sprintf
snprintf
vsprintf
vsnprintf
```

GUI 使用自有 `gfx_vformat`，BLE 日志使用条件编译的 `mini_vsnprintf`。因此“替换标准 sprintf 可节省 1～1.5 KiB”不是当前项目的有效待办。

### 5.5 只读取 Manifest CRC 不能证明资源完整

Manifest 自身 CRC 只能证明 Manifest 结构未损坏，不能证明字体、农历库和 UI Bundle 数据仍与各条目 CRC 一致。

当前分层是合理的：

- 启动/唤醒：校验 Manifest magic、version、条目范围和 Manifest 自身 CRC；
- COMMIT：读取每个资源并计算完整 CRC；
- 运行时：信任已经完成 COMMIT 的激活 Manifest；
- 主 Manifest 损坏：尝试备份 Manifest。

启动时不重复扫描全部外置 Flash 是启动性能优化，不应描述成 Flash 代码体积优化，也不应取消 COMMIT 的完整资源 CRC。

---

## 6. 后续 Flash 优化路线

### P0：建立自动体积预算，防止回涨

建议在构建后校验：

```text
生产 BIN <= 49,152 B（48 KiB 软门槛）
绝对上限  = 57,344 B
最低安全余量 >= 8,192 B
```

当前 48,048 B 低于 48 KiB 软门槛 1,104 B。该软门槛适合作为后续功能增长预警，不是当前必须继续压缩的理由。

同时检查高成本符号。当前允许且仅允许 WDT SDK 已知引入的：

```text
__aeabi_uldivmod
__udivmoddi4
```

以下符号应保持不存在：

```text
__aeabi_f*
__aeabi_d*
sprintf / snprintf / vsnprintf
```

预期收益：不直接节省体积，但能阻止一次小改动重新拉入 0.5～3 KiB 运行库。

### P1：对真实 UI 存活路径做功能级裁剪

`ui_render_component.constprop.0` 是最大业务热点。可按产品能力增加独立配置，而不是裁底层 API：

| 可选功能 | 粗略潜力 | 风险 |
| --- | ---: | --- |
| 时间未同步完整提示页/极简提示 | 50～150 B | 需要产品确认视觉 |
| 资源缺失 fallback 5×7 字体和页面 | 300～800 B | 删除后空 Flash 无法给用户提示 |
| 低电量复杂图标/双边框 | 100～300 B | 影响低电可读性 |
| 农历节日/调休标记 | 300～800 B | 核心产品功能，不建议默认裁剪 |
| 动态 UI Bundle 兼容校验 | 300～700 B | 会降低外部资源安全和可升级性 |

只有明确存在“极简 SKU”时才建议增加这些宏。生产默认版本不应为了几百字节静默删除用户可见功能。

### P1：精简格式化能力，而不是删除文本输出

当前 `gfx_vformat` 支持 `%d/%u/%x/%X/%s/%c`、宽度和补零。可以统计全部调用格式后：

1. 删除没有调用的格式说明符；
2. 把纯静态字符串改为 `GFX_write`，避免进入格式解析和使用 96 B 栈缓冲；
3. 对日期、温度、电量提供专用整数输出 helper；
4. 保留统一的 UTF-8 `GFX_print/GFX_write` 路径。

预计 Flash 潜力 150～500 B，主要收益还包括降低峰值栈。若实施，必须先把五场景黄金图纳入自动测试。

### P1：审查所有 64 位运算

当前业务代码没有额外的 64 位除法；最终 ELF 中两个 helper 仅来自未修改的 Nordic WDT SDK。后续新增以下写法时需要警惕：

```c
(uint64_t)a * scale / divisor
timestamp 的 64 位除法或取模
浮点到整数的通用转换
```

优先使用“商 + 余数”的等价变换、定点数或保证范围的 32 位计算。不能为了体积直接删掉防溢出逻辑。

### P2：固定广播包替代通用 `ble_advdata_encode`

`ble_advdata_encode` 最终约 916 B。如果广播内容完全固定，可以自行构造 AD structure，保留设备名和必要 Flags/UUID，可能节省 300～700 B。

风险包括：

- 广播长度边界；
- UUID 编码；
- 扫描响应包兼容；
- Nordic SDK 后续配置变化。

只有在真机 iOS/Android/网页蓝牙扫描矩阵验证后才适合实施。

### P2：评估 FDS 的替代方案

FDS 是较大的 SDK 子系统，但当前用于配置持久化、垃圾回收和掉电安全。若配置数据始终只有一个固定小记录，可以设计双槽页：

```text
slot A: magic + generation + payload + CRC
slot B: magic + generation + payload + CRC
```

写入新槽成功后再废弃旧槽，可替代部分 FDS 功能。理论潜力 1.5～3 KiB，但这是高风险存储重构，需要断电注入、擦写寿命和升级兼容测试，不属于本轮安全优化。

### P3：SDK 功能裁剪

继续按最终 map 审计：

- 是否必须使用完整 `ble_advertising` 状态机；
- `nrf_ble_gatt` 是否所有能力都需要；
- FDS queue/fifo 配置是否大于实际峰值；
- 未使用 IRQ 和 nrfx driver 是否仍被强制 `-u` 保留。

每删除一个 SDK 模块都要验证 DFU、重连、MTU 协商、长资源传输和低功耗唤醒。

---

## 7. 后续 RAM 优化路线

### P0：先测栈高水位

建议在调试固件中：

1. 启动时用固定字节填充应用栈；
2. 分别执行全刷、局刷、资源 COMMIT、低电关机、BLE 断连和 DFU；
3. 扫描未被覆盖的栈区；
4. 保证最坏路径至少还有 512 B 安全余量。

同时为应用源启用 `-fstack-usage` 的非 LTO 分析构建，重点关注：

- `GFX_printf`；
- `Draw7Number`；
- `send_resource_read_response`；
- `epd_ext_crc32_flash`；
- UI 文本拼接；
- BLE 事件回调到刷新调度的嵌套路径。

### P1：给 GFX 缓冲增加编译期档位

建议定义：

```c
#ifndef GFX_BUFFER_SIZE
#define GFX_BUFFER_SIZE 4800U
#endif
```

先实验 4,096 B 档位。验收内容：

- 扩展后的五场景黄金图 CRC 不变；
- 800×480 单色和三色全刷；
- 6 倍和 10 倍七段局刷；
- 最坏刷新时长增幅不超过 10%；
- SPI 写入调用次数和功耗可接受。

预计静态 RAM 节省 704 B。没有时延数据前不建议直接作为默认值。

### P2：压缩 UI 模式和组件缓存上限

当前资源：

```text
mode_count = 4
单模式最大 component_count = 17
```

固件上限：

```text
UI_CACHE_MAX_MODES = 8
UI_RENDER_COMPONENT_CACHE_MAX = 18
```

把模式上限从 8 改为 4 可省 64 B；组件上限从 18 改为 17 可省 24 B。但这会减少外部 UI Bundle 的未来扩展空间，收益很小，暂不建议。

更好的做法是把上限写进 Bundle schema 和固件能力响应，让资源构建工具在生成阶段检查，而不是出现运行时 `NRF_ERROR_NO_MEM`。

### P2：命令队列与调度器峰值审计

`m_epd` 中的 BLE 命令队列按最大 MTU 保存完整命令。进一步缩小队列或让大块数据走单独流式缓冲，可能节省数百字节。

必须先记录：

- BLE 连续写入速度；
- 最长 EPD 操作期间积压命令数；
- 资源写入 ACK 时序；
- 队列满时客户端重试行为。

不能只因为当前测试没有积压就把队列降为 1。

### P3：缓存生命周期复用

可研究 `m_month_render_cache`、资源 CRC 临时块、局刷 old/new buffer 是否存在严格不重叠生命周期。如果能证明互斥，可以使用具名 union 或统一 workspace。

禁止使用没有类型和生命周期说明的全局“万能临时数组”，否则会造成异步 BLE/刷新路径互相覆盖。

---

## 8. 验收标准

### 8.1 构建与体积

- 生产 GCC 构建无 warning/error；
- `text + data` 与 `.bin` 大小一致；
- 生产 BIN 不超过 48 KiB 软门槛；
- 链接区余量不少于 8 KiB；
- bss/data/stack 预算表随修改更新；
- 最终 ELF 不包含软浮点和 stdio 格式化符号，且 64 位除法 helper 不超过当前 WDT 基线。

### 8.2 配置矩阵

至少验证：

| 配置 | 用途 | 要求 |
| --- | --- | --- |
| `USE_RTT=0 USE_BLE_LOG=0` | 生产 | 必须通过且满足 56 KiB |
| `USE_RTT=0 USE_BLE_LOG=1` | 轻量 BLE 日志 | 当前超 56 KiB 生产区 1,216 B；不得当作生产布局 |
| `USE_BLE_LOG=1 DEBUG_LAYOUT=1` | BLE 日志调试 | 已验证可构建；当前 text+data 为 64,680 B |
| `USE_RTT=1 USE_SYSVIEW=0` | RTT 调试 | 使用 debug layout 或明确允许超生产区 |
| `USE_SYSVIEW=1 DEBUG_LAYOUT=1` | 性能分析 | 必须保留 IRQ 和时间测量功能 |
| Keil nRF52811 target | 发布兼容 | 发布前必须通过原工程编译 |

### 8.3 GUI 回归

执行：

```bash
make -f tools/test/Makefile.golden test
```

当前自动测试只有普通日历场景。任何后续 GUI 优化开始前，应先把时钟+日历、全屏时钟、未校时和低电页面纳入自动测试；届时五个场景必须全部通过。若 UI 变化是产品需求，应先人工确认图像，再更新对应单个场景 CRC，不能批量无条件更新所有 golden。

### 8.4 真机回归

- WDT 正常喂狗时不误复位；
- 故意停止喂狗后能够进入强 `WDT_IRQHandler` 并最终复位；
- BLE 扫描、连接、MTU、断连重连；
- 全刷、局刷、跨小时全刷、夜间静默；
- 资源 ERASE/WRITE/COMMIT/STATUS/READ；
- 主 Manifest 损坏后的备份恢复；
- 低电提示和临界电量关机；
- DFU 进入、升级和回滚。

当前已知配置限制：单独设置 `USE_BLE_LOG=1` 会在 56 KiB 链接区溢出 1,216 B；它不会影响默认生产配置，但调试时必须使用 `DEBUG_LAYOUT=1`，并按 debug 烧录流程操作，避免覆盖 FDS/Bootloader 布局。

---

## 9. 本轮最终判断

本轮最终只保留本文档，所有代码、SDK、Makefile 和测试改动均恢复到 HEAD：

- 当前 Flash 为 48,048 B，56 KiB 链接区余量为 9,296 B；
- 当前 `.data + .bss + 2 KiB stack` 为 11,260 B，RAM 链接余量为 4,396 B；
- 704 B 最小体积实验和约 56 B 栈实验数据只作为候选方案记录，不代表当前代码；
- WDT 保持 Nordic SDK 原实现，避免引入真机和 SDK 升级风险；
- `Draw7Number`、时间同步页面、GFX API 和 EPD 命令判断保持原行为；
- 当前没有为了节省小量空间而改变公共接口或用户可见功能。

在没有明确新增功能导致 Flash/RAM 接近门槛之前，不建议继续实施微型体积优化。下一步最高价值工作是建立自动体积门槛、补齐多场景 GUI 回归和真机栈高水位证据；只有测量显示资源确实不足时，再从本文 P1/P2 候选中选择独立、可回滚的方案。
