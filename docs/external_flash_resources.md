# FM25Q04B 外置 Flash 资源说明

本文档记录当前固件和网页端实际使用的 FM25Q04B 外置 Flash 资源布局、manifest 格式、资源类型和数据内容格式。

## Flash 区域划分

FM25Q04B 按 `4Mbit / 512KB` 处理，地址范围为 `0x00000-0x7FFFF`，扇区大小 `4KB`，页编程大小 `256B`。

| 区域 | 地址范围 | 用途 |
|---|---:|---|
| 保留/未用 | `0x00000-0x00FFF` | 当前未放资源 |
| 资源数据区 | `0x01000-0x7CFFF` | 字体、农历、节假日、UI 数据 |
| 空洞/保护区 | `0x7D000-0x7DFFF` | 当前未使用 |
| Manifest | `0x7E000-0x7EFFF` | 当前有效 manifest，同时也是 staging manifest |
| Reserved/Scratch | `0x7F000-0x7FFFF` | 保留/调试 scratch |

注意：早期规划中主 manifest 和 staging manifest 分开；当前实际实现中二者都指向 `0x7E000`。网页端写入资源数据并校验后，将 manifest 写入 `0x7E000`，固件 commit 时验证该 sector 并在 RAM 中激活。

## Manifest 格式

Manifest 地址为 `0x7E000`，当前大小为 `396 bytes`。

```c
typedef struct {
    uint32_t magic;          // 0x45505253, 小端字节为 "SRPE"
    uint16_t version;        // 1
    uint16_t resource_count; // 当前 20
    entry[24];               // 每项 16 bytes
    uint32_t crc32;          // manifest CRC32
} manifest;
```

每个 entry：

```c
typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} entry;
```

当前网页资源包 manifest CRC 为 `0x3542DC2B`。

## 资源类型

| type | 含义 |
|---:|---|
| `1` | 字体 FONT |
| `2` | 农历 LUNAR |
| `3` | 节假日 HOLIDAY |
| `4` | UI 模板 UI |
| `5` | UC8179 局刷 LUT，历史兼容类型，当前资源包不再生成 |

说明：UC8179 LUT 当前固件使用内置参数生成方式，运行时不读取外置 Flash 中的 `type=5` 资源；网页资源包也不再生成 LUT entry。

## 当前资源内容

当前资源包共 `20` 个 entry，资源数据实际占用约 `12005 bytes`，资源区剩余约 `494KB`。

| type/id | 名称 | 地址 | 长度 |
|---|---|---:|---:|
| `1/1` | `u8g2_font_wqy9_t_lunar` | `0x01000` | `4315` |
| `1/2` | `u8g2_font_wqy12_t_lunar` | `0x020DC` | `4830` |
| `1/3` | `u8g2_font_helvB14_tn` | `0x033BC` | `287` |
| `1/4` | `u8g2_font_helvB18_tn` | `0x034DC` | `389` |
| `2/1` | `lunar_month_names` | `0x03664` | `91` |
| `2/2` | `lunar_leap_names` | `0x036C0` | `8` |
| `2/3` | `lunar_date_names` | `0x036C8` | `217` |
| `2/4` | `lunar_day_names` | `0x037A4` | `28` |
| `2/5` | `lunar_zodiac_names` | `0x037C0` | `48` |
| `2/6` | `lunar_stem_names` | `0x037F0` | `40` |
| `2/7` | `lunar_branch_names` | `0x03818` | `48` |
| `2/8` | `lunar_jieqi_names` | `0x03848` | `168` |
| `2/9` | `lunar_month_days` | `0x038F0` | `28` |
| `2/10` | `lunar_solar_1_1` | `0x0390C` | `28` |
| `2/11` | `lunar_jieqi_year_bits` | `0x03928` | `18` |
| `2/12` | `lunar_jieqi_base_days` | `0x0393C` | `24` |
| `3/1` | `solar_festivals` | `0x03954` | `180` |
| `3/2` | `lunar_festivals` | `0x03A08` | `132` |
| `3/3` | `holiday_adjustments` | `0x03A8C` | `82` |
| `4/1` | `ui_bundle` | `0x03AE0` | `1029` |

## 数据内容格式

### 字体资源

字体资源直接从 `GUI/fonts.c` 中解析 u8g2 字库原始 byte array。

固件侧通过 token 引用外置字体，渲染时按需读取，`u8g2_font.c` 中维护一个 `256B` 字体读取缓存。

### 农历资源

农历资源包含名称字符串和计算表：

- 名称类：UTF-8 固定宽度字符串表。
- 数值类：小端 `uint32_t` 表或 byte 表。

当前包含：

- 月份名、闰月名、日期名、星期名。
- 生肖、天干、地支、节气名。
- 农历月大小表、春节日期表、节气计算表。

### 节假日资源

节日表格式：

```text
month:uint8 + day:uint8 + name[10] UTF-8
```

调休/放假表格式：

```text
year:uint16 + count:uint16 + uint16 list
```

`uint16` 日期编码中包含月份、日期和休/班标记，固件读取后判断当天是休假还是调休上班。

### UI Bundle

UI bundle 是一个单独资源：`type=4, id=1`。

```text
header:    28 bytes
mode:      16 bytes / item
component: 24 bytes / item
string:    当前只有 1 个 0 字节占位
```

Header 字段：

```c
magic              // 0x314C4955, 小端字节为 "UIL1"
version            // 1
screen_width       // 800
screen_height      // 480
mode_count
component_count
mode_offset
component_offset
string_offset
string_length
reserved0
reserved1
```

Mode record 字段：

```c
mode_id
update_policy
component_start
component_count
partial_x
partial_y
partial_w
partial_h
flags
reserved
```

Component record 字段：

```c
type
data_id
font_id
flags
x
y
w
h
fg
bg
p0
p1
p2
text_offset
```

当前 UI 模式：

| mode id | 名称 | 更新策略 |
|---:|---|---|
| `1` | 全屏日历 | daily |
| `2` | 分屏时钟日历 | minute，局刷区域 `0,169,384,150` |
| `3` | 全屏时钟 | minute |
| `250` | 低电量页 | manual |

### UC8179 LUT

当前资源包不再包含 UC8179 LUT 资源。固件中只保留每档温度的 `T1/T2` 参数，局刷时在 RAM 中生成 42B LUT 表并立即写入 UC8179 寄存器。

历史外置 LUT 格式如下，仅供兼容参考：

```text
6 tables * 42 bytes = 252 bytes
```

6 个表顺序为：

```text
LUTC, LUTWW, LUTKW, LUTWK, LUTKK, LUTBD
```

每个 42B 表格式：

```text
[waveform, T1, T2, T3=0, T4=0, repeat=1, padding 36B]
```

当前固件已将 UC8179 LUT 改为内置优先，因此这些 LUT 外置资源属于历史兼容/待清理资源。

## 运行时加载时机

当前 FM25Q04B 在固件启动后会初始化 SPI1 并读取资源索引；资源预加载完成后会进入 deep power-down。外置 Flash 不是持续读写，而是在下面几个场景按需唤醒并读取小块数据。

说明：FM25Q04B 的 VCC 没有独立电源开关，固件无法真正断开供电；这里的“关闭”指发送 `0xB9` deep power-down 命令让 Flash 进入低功耗状态。下一次读写会先发送 `0xAB` release power-down 自动唤醒。

### 上电/复位启动

入口：`ble_epd_init()`。

| 步骤 | 读取内容 | 地址/资源 | 是否缓存到 RAM | 说明 |
|---|---|---|---|---|
| 初始化 Flash | JEDEC ID、状态寄存器 | SPI 命令 | 是，缓存 JEDEC ID | `fm25q04b_init()` 初始化 SPI1，`fm25q04b_probe()` 确认芯片在线 |
| 读取 manifest | manifest sector 前 `396B` | `0x7E000` | 是，缓存完整 manifest | `epd_ext_resources_init()` 每次会重新校验 manifest CRC |
| 预加载 UI cache | UI bundle header | `type=4,id=1` 起始 `28B` | 是 | 校验 magic、version、屏幕尺寸、mode/component/string 范围 |
| 预加载 UI mode 表 | mode records | `type=4,id=1` 的 mode 区 | 是，只缓存 mode 表 | 当前只缓存 header + mode records，不缓存全部 component |
| 进入低功耗 | deep power-down | SPI 命令 `0xB9` | - | 启动预加载完成后立即休眠，后续资源访问自动唤醒 |
| 不读取 | 字体、农历、节假日、UI component | - | - | 启动阶段不主动加载整包字体或农历表 |

启动完成后，RAM 中常驻的外置资源相关缓存主要包括：

- `m_manifest`：完整 manifest 和 entry 表。
- `m_jedec_id`：Flash JEDEC ID。
- UI cache：UI bundle header 和 mode records。
- 局刷组件 cache：首次分屏分钟局刷时缓存 `MODE_CLOCK_CALENDAR` 中带 `PARTIAL_ONLY` 标记的组件。
- 月历渲染 cache：首次绘制某个月时缓存 1-31 号的农历/节日文本和休/班标记。
- u8g2 字体缓存：`256B`，只有渲染文字后才会填充。

### 网页连接后的资源检查

入口：网页发送 `RES_STATUS(0x43)` 和 `RES_READ(0x44)`。

| 步骤 | 读取内容 | 读取方式 | 说明 |
|---|---|---|---|
| 查询状态 | JEDEC ID、manifest CRC、资源数量、容量/已用空间 | `RES_STATUS` | 固件返回 RAM 中的 manifest 状态；如果 manifest 无效，仍返回 Flash 在线状态 |
| 读取 manifest | `0x7E000` 附近 manifest 小块 | `RES_READ` raw read | 网页完整解析 manifest，和内置资源包比较 |
| 抽样校验资源 | 每个资源首尾小块 | `RES_READ` raw read | 网页默认不读回完整 10KB+ 资源，只做抽样和 CRC/长度比对 |

网页检查不会写 Flash。只有用户点击更新或清空时才会进入擦写流程。

如果 Flash 处于 deep power-down，资源命令会在底层驱动中自动唤醒。

### 网页更新/强制清空

入口：`RES_ERASE(0x40)`、`RES_WRITE(0x41)`、`RES_COMMIT(0x42)`。

| 步骤 | 访问内容 | 说明 |
|---|---|---|
| 擦除资源区 | 指定地址范围，按 `4KB` sector 擦除 | 更新资源时擦除资源数据区和 manifest sector；强制清空可擦除全片 |
| 写资源数据 | `type=1/2/3/4` 对应的资源 payload | 以 BLE 分片写入，Flash 内部按 `256B` page program |
| 写 manifest | `0x7E000` | 当前实现中主 manifest 与 staging manifest 同地址 |
| commit 后重载 | manifest、UI header、UI mode 表 | commit 成功后刷新 RAM manifest，并重新加载 UI cache |

更新过程中会频繁擦写 Flash，应避免同时触发传图、刷新和 DFU。

BLE 断开后，固件会再次发送 deep power-down，让外置 Flash 回到低功耗状态。

### 整点全刷

入口：周期刷新事件中 `need_full_refresh == true`，调用 `DrawGUI()`。

| 阶段 | 读取内容 | 是否每次整点都会读 | 说明 |
|---|---|---|---|
| 模式定位 | UI cache 中的 mode record | 不读 Flash | mode 表启动时已缓存 |
| 加载组件 | 当前 mode 的 component records | 会读 Flash | `ui_load_mode_components()` 在进入分页绘制前一次性读取当前 mode 的组件到栈上小数组，避免每一页重复读组件 |
| 月历预计算 | 当月每天的农历/节日文本、休/班标记 | 首次绘制该月时读取 | 结果缓存到 RAM；分页绘制和同月后续整点全刷复用缓存，不再重复读农历/节假日表 |
| 农历转换 | 农历月大小、春节日期、节气等表项 | 仅月历 cache 缺失或月份变化时读取 | `LUNAR_SolarToLunar()` 在生成月历 cache 时集中调用 |
| 月历绘制 | RAM 中的当月渲染 cache | 不读 Flash | 全屏日历和分屏日历只读取缓存好的文本/标记 |
| 文本渲染 | 当前字体 glyph 数据 | 会按需读 Flash | u8g2 按字形查表读取，使用 `256B` 字体缓存；不会一次性加载完整字体 |
| LUT | UC8179 局刷 LUT | 不读 Flash | 当前 LUT 已改为内置参数生成 |

全刷时外置 Flash 访问量最大。优化后主要来自两类：当前 mode 的 UI component records、字体 glyph 数据。月历/农历/节假日只在月份 cache 未命中时集中读取一次。

如果设备已断开 BLE，刷新结束后会将 FM25Q04B 重新置入 deep power-down。下一次分钟局刷通常不需要唤醒 Flash；下一次整点全刷如需要读取字体或 UI component，会自动唤醒，读完并刷新结束后再次休眠。

### 分钟局刷

入口：分屏时钟日历模式 `MODE_CLOCK_CALENDAR` 的局刷路径，调用 `DrawGUI_ClockOnly()`。

| 阶段 | 读取内容 | 说明 |
|---|---|---|
| 获取局刷区域 | UI cache 中的 mode partial rect | 优先使用启动时缓存的 mode 表 |
| 加载组件 | `MODE_CLOCK_CALENDAR` 的 `PARTIAL_ONLY` component records | 首次局刷从 UI bundle 读取并缓存到 RAM；后续分钟局刷不再读取 UI component |
| 时间文本/数字 | 数字字体 glyph 数据 | 从外置字体资源按需读取，命中 `256B` 字体缓存时不再访问 Flash |
| 农历转换 | 不读取 | `DrawGUI_ClockOnly()` 不再调用 `LUNAR_SolarToLunar()`；局刷只允许时间相关组件访问外置资源 |
| 日历/节假日 | 通常不绘制 | 带 `PARTIAL_ONLY` 筛选，只绘制局刷时间组件；普通日历组件不参与渲染 |

分钟局刷理论访问量比整点全刷小很多。当前已避免农历/节假日读取，并缓存局刷组件；稳定运行后的分钟局刷主要只剩数字字体 glyph 的按需读取。

如果后续 UI 模板误把农历文本字段标记为 `PARTIAL_ONLY`，固件会因为局刷路径没有 `Lunar` 数据而跳过该字段，避免在分钟局刷中重新访问农历资源。

### 低电量页

入口：`DrawGUI()` 中电压低于阈值时切换到 `UI_MODE_LOW_BATTERY`。

| 读取内容 | 说明 |
|---|---|
| 低电量 UI mode/component | 从 UI bundle 读取对应 mode 的 component records |
| 字体 glyph | 文本渲染时按需读取字体 |
| 不读取农历/节假日 | 低电量分支不调用农历转换 |

### 资源缺失提示页

入口：manifest 无效、UI bundle 缺失或资源未更新时的提示界面。

该页面使用 `GUI.c` 内置的 5x7 ASCII fallback 字体，不依赖外置 Flash 字体和 UI bundle。它的目标是保证资源损坏时仍能提示用户用网页更新资源。

### 外置 Flash 访问总结

| 场景 | 主要读取资源 | 读取量特征 | 风险点 |
|---|---|---|---|
| 上电启动 | JEDEC、manifest、UI header/mode | 小 | manifest 损坏会导致资源不可用，但不应阻塞擦除/更新 |
| 网页检查 | manifest、资源首尾抽样 | 中等，由网页请求驱动 | BLE 连续读需要避免 GATT 写入重入 |
| 网页更新 | 擦除、写资源、写 manifest、读回抽样 | 大，含擦写 | 需要低电压保护和逐片响应 |
| 整点全刷 | UI component、字体 glyph；月份 cache 未命中时读取农历/节假日 | 中到大 | 同月后续整点全刷复用月历 cache，避免重复读农历/节假日 |
| 分钟局刷 | 首次读 UI partial component，之后主要读数字字体 | 小 | 不再读取农历/节假日资源；UI partial component 会缓存 |
| 低电量页 | UI component、字体 glyph | 小 | 不读农历/节假日 |

## 低功耗策略

当前采用方案 A：外置 Flash 作为冷数据仓库，运行时按需唤醒。

| 时机 | 外置 Flash 状态 | 说明 |
|---|---|---|
| 固件启动 | 唤醒/初始化 | 初始化 SPI1、probe JEDEC、读取 manifest、预加载 UI cache |
| 启动预加载后 | deep power-down | 减少空闲功耗 |
| BLE 连接后资源检查/更新 | 自动唤醒 | `RES_STATUS/READ/ERASE/WRITE/COMMIT` 会按需访问 Flash |
| BLE 断开 | deep power-down | 如果屏幕正在刷新，则刷新结束后再休眠 |
| 分钟局刷 | 通常保持休眠 | 时间日历模式局刷不读农历/节假日，也不再读取 UI component |
| 整点全刷 | 按需自动唤醒 | 需要读取 UI component、字体 glyph 或月份 cache 未命中时读取农历/节假日 |
| 刷新结束且 BLE 已断开 | deep power-down | 刷新完成后再次降低外置 Flash 功耗 |

## 生成文件

资源由 `tools/build_ext_resources.py` 生成：

- `tools/epd_resources.bin`：完整外置 Flash 资源镜像，包含资源数据和 manifest。
- `html/js/resource_bundle.js`：网页端内置资源包，包含 base64 数据、manifest、entry 列表和 UI JSON。

设备通过网页端 BLE 资源协议写入外置 Flash，并在连接后读取 manifest CRC 和抽样数据判断是否需要更新。
