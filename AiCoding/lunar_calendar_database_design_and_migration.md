# 农历与节日调休数据化迁移设计方案 (Lunar Calendar & Holiday DB Migration Plan)

## 1. 背景与目标 (Background & Objectives)
当前系统中，日历渲染依赖大量的内置计算逻辑（包括农历公农转换、天干地支生肖计算、二十四节气计算、公历农历节日判断、母亲节/父亲节/感恩节等规则节日计算，以及法定调休班/休状态匹配）。这些动态计算带来了以下问题：
1. **代码体积占用大**：包含庞大的节气角度计算、大小月状态表、调休硬编码数组等。
2. **扩展性差**：新增年份或调整调休必须重新编译固件。
3. **计算冗余**：MCU 需在每次刷新或生成月历缓存时，对本月每天重复进行高开销的公农转换。

本方案旨在将**所有日历相关的复杂计算下放到网页/脚本编译端**，在固件编译或资源打包时，自动生成 2026–2027 年的每日日历数据库（LCDB - Lunar Calendar Database），并封装为 Type 6 (`EPD_EXT_RES_TYPE_LUNAR_DB`) 写入外置 Flash。其余年份数据均按照 2026 年显示。MCU 端仅作为数据库查询引擎，实现极简化、数据驱动的日历填充与渲染。

---

## 2. 功能模块梳理与显示框架评估 (Functional Module Review & Display Framework Evaluation)

结合当前代码库的深度梳理，本项目目前形成了“硬件驱动 - GUI绘制 - 蓝牙交互 - Web配置 - 本地模拟”的完整工具链体系。

### 2.1 当前项目主要功能模块
1. **墨水屏驱动与电源管理模块 (`EPD/`)**：
   * 负责墨水屏控制芯片（如 UC8179 等）的 SPI 通信、初始化序列下发、刷新波形（LUT）调优、以及高低温温区判定。
   * 集成电池电压采样与硬件低功耗控制（低电量提示与系统深度关机）。
2. **GUI 图形与文本绘制模块 (`GUI/`)**：
   * 基于 Adafruit GFX 引擎，实现点、线、圆、位图以及中西文字体的动态渲染。
   * 承载日历模式（月历网格绘制）、时钟模式（数码管大字绘制）及分屏模式的排版渲染逻辑。
3. **蓝牙通信与数据传输模块 (`main.c`, `EPD/EPD_service.c`)**：
   * 实现 Nordic 协议栈的 BLE 广播与自定义服务注册。
   * 支持指令交换（如校时、模式切换、驱动下发）及大容量二进制数据传输（带 CRC16 校验的图像分块传输与断点续传）。
4. **Web 蓝牙控制台与布局工具 (`html/`)**：
   * 采用 Web Bluetooth API，提供跨平台的控制端。
   * 内置图像处理（裁剪、抖动）与可视化的 UI 布局编辑器（`ui_editor.js`），支持用户在浏览器中直观定义界面元素。
5. **本地 GUI 模拟器 (`emulator.c`)**：
   * 提供在 Windows 环境下独立于 MCU 的显示层仿真，方便在不需要频繁烧录真机的前提下，对布局和渲染行为进行实时查看和快速调试。

---

### 2.2 “UI与数据分离、网页配置样式”框架评估

#### 方案核心思想
将原先硬编码在固件 C 源码（`GUI/GUI.c`）中的绘图 API 序列与布局参数，抽象为可配置的二进制模板指令集（`UI_BUNDLE`）；同时将海量农历算法与调休数据库（`LCDB`）剥离至网页端预生成。设备端只负责根据当前状态，对 Flash 中的布局数据进行**“绑定变量 -> 循环译码 -> 映射绘制”**。

```
+----------------+     BLE传输      +------------------+
| 网页端模板编辑器 | -------------> | 外置 Flash (LCDB) |
+----------------+                  +------------------+
                                             |  读取单日记录
                                             v
+----------------+     解包渲染     +------------------+
| 固件 UI 渲染器  | <------------- | 运行时状态数据链  | (SSID, Temp, Time)
+----------------+                  +------------------+
```

#### 方案可行性与优势评估
* **👍 极佳的审美体验与设计自由度**：
  * **所见即所得**：UI 开发者在网页控制台上可直接通过拖拽调整间距、大小、对齐与字体，即时预览渲染效果。
  * **动态换肤**：无需重新编译代码，只需生成新的二进制布局包并下发，即可让墨水屏在瞬间切换从“简约时钟”到“卡通节日插画”的完全不同布局风格。
* **⚡ 突破硬件资源瓶颈 (nRF52811)**：
  * nRF52811 仅有 192KB ROM 与 24KB RAM。若内置全套阴阳历公式、大月表、节气高精度浮点运算、及各节日硬编码列表，随着年份推移，将迅速突破 Flash 空间。
  * 数据分离后，固件剥离大量静态数组和数学换算逻辑，使应用区 ROM 占比大幅回落，给 BLE 连接层与 Secure DFU 安全性留足余量。
* **🛠️ 降低开发门槛与维护开销**：
  * 调休安排（每年国家公布的班/休假期）与节日变更不再依赖固件升级，只需更新外置 Flash 中的 `LCDB` 和 `UI_BUNDLE`。
  * 实现了“一固件多设备”，同一套编译产物可通过下发不同的 UI 模板适应不同尺寸、不同驱动的屏幕。

#### 潜在风险与规避策略
1. **Flash 读取延迟对局刷的影响**：
   * *重新评估*：无论是何种显示模式，日历相关的数据（农历、生肖、节气、节日、调休标志）在**一天 24 小时之内是完全静态的**，只有在每天子夜（00:00）跨天更新时才需要读取外置 Flash。每分钟的时钟数字更新（如 `HH:MM`）是直接从 MCU 内部 RTC 时间（RAM 变量）动态计算并直接通过局刷渲染的，**完全不需要读取外置 Flash**。
   * *规避策略*：
     * **日历记录单日缓存 (RAM Cache)**：固件定义一个 12 字节的静态变量 `static daily_record_t s_today_record`。在每天开机或跨天检测通过时，仅从 Flash 中读取一次 12 字节记录，后续该日内的所有局刷刷新（或由于页面跳转、温度变化引起的重绘）直接读取该 RAM 结构体，**将外置 Flash 读取频次降到 1 次/天**，局刷读取延迟风险直接降为零。
     * **全刷天然掩盖**：子夜跨天时伴随的是墨水屏整屏物理全刷（清除残影，耗时 3~15 秒），而读取 Flash 中 12 字节记录加字串仅耗时微秒级，在时间轴上完全被全刷开销掩盖。
2. **解释器内存溢出与死锁风险**：
   * *规避策略*：
     * **无动态分配 (Zero-Allocation)**：解析引擎不使用任何 `malloc`/`free` 等堆分配操作。UI 模板采用流式读取或直接映射到栈/预分配静态结构体，防止 RAM 碎片化与溢出。
     * **坐标边界硬截断 (Coordinate Clipping)**：模板中声明的所有坐标 `(x, y, w, h)` 在调用 GFX 引擎前，必须经由边界截断器校验。对于 $800 \times 480$ 屏幕，任何超出 `x + w > 800` 或 `y + h > 480` 的非法元素一律自动裁剪或拒绝绘制，从底层物理杜绝写显存越界导致崩溃。
     * **扁平化结构限制 (Component Limit)**：不支持循环语句、条件分支判断和深层组件嵌套。最大组件数量硬编码限制（如 `MAX_UI_ITEMS = 16`），解析器只需通过一个扁平的 `for` 循环依次读取各个组件并绘制，执行时间高度确定，不存在死锁和死循环路径。
     * **静态校验与魔数**：模板文件头部增加校验字段（CRC32 与专属 Magic），在加载前进行比对，非法模板直接拒绝解析并退回资源缺失屏（Fallback Screen）。

---

### 2.3 跨天日期自动更新机制 (Automatic Date Transition Mechanism)

为了实现无人值守的日历自动更新，系统设计了基于 RTC 时钟中断的自动判定与重绘触发链条：

1. **跨天自动判定**：
   * MCU 底层的应用定时器或 RTC 定时中断每秒/每分钟唤醒主循环。
   * 系统维护一个全局状态变量 `static uint8_t s_last_rendered_day`，记录当前屏幕上显示的是哪一天。
   * 每次主循环唤醒后，获取当前 RTC 时间结构体 `current_time.tm_mday`，对比 `current_time.tm_mday != s_last_rendered_day`。
2. **重绘与全刷触发链**：
   * 一旦判定跨天（即跨越到 00:00），系统自动执行以下步骤：
     1. 更新状态变量：`s_last_rendered_day = current_time.tm_mday`。
     2. 自动检索：调用 `lunar_db_get_record(current_time.tm_year, current_time.tm_mon, current_time.tm_mday, &s_today_record)` 读取新一天的日历数据块。
     3. 绑定新数据：将读取到的节日、节气、农历文本等字段更新至显示缓冲区。
     4. **发起全屏刷新**：调用 `EPD_Display_All` 执行物理全刷，以完全清除墨水屏在前一天频繁局刷时累积的屏幕残影（Ghosting），并将日期指示线和日期文字彻底更新。
3. **静默校时兼容**：
   * 若用户在日间通过蓝牙下发了 `EPD_CMD_SYNC_TIME_SILENT` 进行校时，即便时间跨越了跨天边界，系统也会自动静默检测并触发全刷，无需人工干预。

---

### 2.4 数据、UI与算法解耦的深度分析与优化空间 (Decoupling Analysis & Optimization Space)

通过将系统拆分为 **“数据（Data）”**、**“显示（UI）”** 和 **“逻辑算法（Algorithms）”** 三个核心层，我们实现了清晰的边界解耦：
1. **数据（Data）**：
   * **特点**：全静态日历表与调休表 (`LCDB`) 固化于外置 Flash，MCU 端仅在跨天或初始化时按需读取。
   * **解耦优势**：MCU 不必保留庞大的数组与结构体，运行时仅持有当前日期的极简缓存。
2. **显示（UI）**：
   * **特点**：界面排版（文本坐标、线框、字号大小）抽象成模板数据 (`UI_BUNDLE`)，完全脱离固件编译。
   * **解耦优势**：实现“界面即资源，所见即所得”，不需要改动任何驱动与绘制的核心 C 代码就能调整布局样式。
3. **逻辑算法（Algorithms）**：
   * **特点**：复杂的农历公农转换、二十四节气高精度角度计算、规则节日偏移公式全部在网页编译端（Python 脚本 / Web JS）单向计算完毕。
   * **解耦优势**：MCU 被提纯为“只查表，不计算”的纯粹渲染引擎。

#### 进一步的优化改进空间与潜在问题分析

基于当前设计，在整体架构上还存在以下进一步的优化方向与防御性措施：

##### 优化 1：基于“轻量字典 (Token Map)”全局共享字符串池，消除多年份冗余
* **现状问题**：在目前的年份 Block 中，各年份都配备独立的字符串池。类似“元旦节”、“国庆节”、“春节”、“立春”、“大寒”等固定词汇在 2026 年和 2027 年的数据块中是重复存储的，这占用了宝贵的 Flash 存储。
* **改进方案**：引入一个**全局静态节日与节气字典 (Static Label Token Map)**。
  * 将 24 节气和 10 个最常见传统节日的中文直接编译进 MCU ROM（或置于 Master Header 共享池），仅需不到 200 字节。
  * `daily_record_t` 的 `solar_term` (1B) 和 `holiday_type` 已经天然指示了节气与工作状态。
  * 只有遇到罕见的非固定特殊自定义标签时，才利用偏移量指向年份的局部字符串池。此优化可将 `LCDB` 的 Flash 空间开销再次压缩 30% 以上。

##### 优化 2：基于“虚拟绑定 (Data Binding ID)”映射动态系统数据，统一渲染管线
* **现状问题**：`UI_BUNDLE` 模板如何能够画出动态变化的数据（例如当前的电池电压、蓝牙连接图标、时钟分秒）？如果由 GFX 在模板解析外手动插手绘制，会导致“UI与逻辑”并没有彻底剥离。
* **改进方案**：引入 **运行时动态数据绑定表 (Data Bind Vector)**。
  * 在 UI 模板中，文本或图标组件的 `data_source` 字段不仅可以代表静态文本，还能绑定特定的 `Data Bind ID`：
    * `0x01` -> 绑定当前 RTC 分钟大字 (`HH:MM`)
    * `0x02` -> 绑定电池电压数值 (`%d mV` / 百分比电量)
    * `0x03` -> 绑定蓝牙连接状态图标 (通过图标帧映射)
    * `0x04` -> 绑定外接传感器温度值
  * 固件渲染器在解析 `UI_BUNDLE` 到特定组件时，若检测到 `data_source` 存在 Binding ID，则直接读取 RAM 中的对应状态量，统一套用模板的坐标、字体进行绘制，保证了渲染管线的一致性。

##### 优化 3：支持“动态字体子集化 (Dynamic Font Subsetting)”与 Flash 字库解耦
* **现状问题**：精美排版高度依赖多种字体和字号。若将全部字体的完整字库置于 Flash 中，对于 192KB ROM 的 MCU 和受限的 Flash 极为吃力。
* **改进方案**：**动态字体子集打包**。
  * 网页端编译器在打包 `UI_BUNDLE` 时，静态扫描模板中所用到的全部文字（包括节日文本、星期、数字等）。
  * 提取这些字符的点阵数据（Glyph Bitmap），连同模板一起打包成专属的 **UI 字库子集 (Font Subset)** 写入外置 Flash。
  * MCU 渲染时只需根据字体的内部 Index 寻找偏移量渲染，从而避开了加载大型字库的需要。

##### 优化 4：本地模拟器与真机像素渲染对齐
* **潜在问题**：网页端布局与 MCU 渲染因为所使用的绘图底层（HTML5 Canvas vs Adafruit GFX）不同，可能导致实际墨水屏上的文字环绕、排版位置与网页设计存在像素级偏差。
* **改进方案**：在 `emulator.c` 模拟器中集成与 MCU 100% 相同的 `UI_BUNDLE` 字节码解析引擎。当在网页端编辑完后，一键导出到本地模拟器运行，通过 Windows 端仿真对齐像素，闭环验证排版效果。

##### 优化 5：Flash 双区冗余备份 (Ping-Pong Buffer) 与原子切换
* **潜在问题**：用户通过网页端 Web Bluetooth 下发更新 `UI_BUNDLE` 或 `LCDB` 时，如果写入中途蓝牙连接意外中断，或者设备因低电量关机，会导致 Flash 中模板数据损坏。此时设备重启会因解析到残缺布局而白屏或死机。
* **改进方案**：
  * 在 Flash 预留扇区中开辟 **Area A** 和 **Area B** 两个大小对齐的镜像槽位（如各 4KB 扇区对齐）。
  * 固件内保存一个超轻量元的 Meta-Pointer 指示当前的“活动槽位”（通过一个扇区的最后 4 字节状态判断）。
  * 每次写入新模板时，永远只改写非活跃槽位，写入完成后计算全包 CRC32。校验通过后，以单字节原子操作改写 Meta-Pointer。
  * 若升级异常中断，固件在上电时检测到新槽位校验失败，会自动继续使用旧槽位中的有效排版，确保系统具备“防砖 (Anti-Brick)”自愈能力。

##### 优化 6：零拷贝字符串流式渲染 (Zero-Copy Streaming Rendering)
* **潜在问题**：读取 `LCDB` 的节日字符串池或自定义特殊标签时，如果需要将不定长 UTF-8 字符串一次性加载到固件 RAM 缓冲区（如 `char label_buf[64]`）再进行绘制，在内存高度受限的 nRF52811 上，面对恶意超长文本或异常溢出会存在栈破坏风险。
* **改进方案**：
  * 抛弃传统“读出完整字串 -> 存入 RAM 缓冲 -> 调用绘图”的三步法。
  * 将 GFX 文字绘制改为**流式读取绘制（Stream-Rendering）**：在字符渲染循环中，每次仅通过 Flash 读取接口读取下一个 UTF-8 字符（最多占 3 字节），直接传入字模绘制函数写显存，遇到 `\0` 即终止。
  * 这样无论节日或自定义文本多长，固件所需的 RAM 缓冲开销恒定为 `O(1)`（仅 3 字节），从物理机制上彻底免疫超长文本栈溢出风险。

##### 优化 7：模板 Schema 语义版本向前兼容 (Schema Version Handshake)
* **潜在问题**：随着网页控制台与 UI 编辑器的后续功能迭代，势必会引入全新的绘图指令（如圆角矩形、虚线）或更高阶的动态绑定 ID。如果旧版本固件直接加载包含这些新特性的模板，可能导致解析引擎报错挂起。
* **改进方案**：
  * UI 模板文件头部加入固定的 `schema_major` 与 `schema_minor` 语义化版本标识。
  * 固件解析器上电加载模板时，执行版本握手：
    * 若 `schema_major > firmware_parser_major`（主版本越界），说明模板指令集格式已发生破坏性不兼容，固件直接跳转至默认的安全降级界面（Fallback Page），并通过 BLE 上报“版本不匹配”状态码，提示用户升级 MCU 固件。
    * 若仅有 `schema_minor` 不一致，解析引擎直接跳过无法识别的未知命令码，继续向后解析并渲染其他已知组件，保持完美的向下兼容性。

##### 优化 8：屏幕规格与布局模板硬件匹配校验 (Hardware Constraint Check)
* **潜在问题**：设备引脚和墨水屏驱动型号（如 UC8179、SSD1619 等）是由网页端下发并在固件中动态切换的。若用户将专为 7.5 寸三色屏（800x480）设计的精细 UI 模板，误传给 4.2 寸黑白屏（400x300）的价签，会导致显示区域错位、文字乱序甚至内存越界绘制。
* **改进方案**：
  * 在 `UI_BUNDLE` 的头部元数据中，增加 `target_screen_width` (uint16_t)、`target_screen_height` (uint16_t) 和 `required_color_depth` (uint8_t) 三个约束字段。
  * 固件在解析模板前，比对当前底层驱动的物理宽高和色彩模式。若匹配失败，则拒绝执行并回退到系统内置的简易信息界面，避免硬件与软件显示模板的不匹配产生逻辑异常。

##### 优化 9：UI 组件刷新频率约束与低功耗保护 (Refresh Frequency Rate Limiting)
* **潜在问题**：电子纸屏幕（E-Paper）由于物理墨水胶囊机制，不能像 LCD/OLED 那样进行无休止的高频刷新。如果模板设计者在网页端绑定了以秒为单位的变化量（如 `seconds` 数字，或每 5 秒滚动的传感器数据），墨水屏将持续局刷，导致屏幕极易出现严重“烧屏”（物理极化损坏）并迅速耗尽电池电量。
* **改进方案**：
  * 对 `UI_BUNDLE` 中绑定的 `Data Binding ID` 设定刷新速率限值标签。
  * 固件渲染引擎强制校验：任何触发局部刷新的数据源（如时钟），更新频率下限为 60 秒。如果检测到数据刷新请求频次低于限值（例如秒级更新），解析器会自动将其忽略或强制降级为分钟级更新，在固件级别筑牢墨水屏寿命及低功耗保护墙。

##### 优化 10：字模缺失的优雅回退与降级机制 (Glyph Fallback Engine)
* **潜在问题**：在“动态字库子集化”模式下，如果网页端打包时未涵盖某些罕见字符，或者由于未来调休数据更新引入了新的汉字（如特定新节日名称），当 MCU 读取该字符并在 Flash 字库子集中寻址时，会发生寻址失败，导致显示空白或死机。
* **改进方案**：
  * 固件字模加载引擎中增加 Glyph Fallback 路由。
  * 若从外置字库索引中未查找到对应字符的点阵信息，一律回退渲染为系统预设的通用缺字字符（如空白框 `□` 或问号 `?`），同时维持绘制管线不中断，保证界面整体稳定性。

##### 优化 11：基于组件“脏标记”的局部刷新动态矩形合成 (Dynamic Bounding Box for Partial Refresh)
* **现状问题**：在目前的 `ui_mode_record_t` 定义中，局部刷新区域（如 `partial_x, partial_y, partial_w, partial_h`）是作为一个硬编码的静态矩形固化在模式记录中的。这带来了一个严重的设计耦合：如果用户在网页端拖动时钟数字（或电量图标）到屏幕的其他位置，这个局刷矩形也必须在网页端手动或自动重新计算并下发给模式记录。如果用户在同一个界面放置了两个需要局刷的动态组件（如：左下角是每分钟变化的时钟，右上角是每分钟变化的传感器读数），单矩形局刷将无法满足要求，或者被迫合成一个覆盖全屏的超大局刷矩形，导致局刷性能下降、闪烁加剧。
* **改进方案**：
  * 去除 `ui_mode_record_t` 中硬编码的局刷矩形坐标。
  * 固件端渲染器在每分钟（或事件触发）重绘时，首先比对每个 UI 组件绑定的数据源 ID 的“新值”与“旧值”（保存在 RAM 状态链中）。
  * 仅将发生数值改变的组件标记为“脏（Dirty）”。
  * 渲染引擎自动计算所有“脏”组件的包围盒（Bounding Box），将其合并（Union）为一个或多个局刷脏矩形（Dirty Rects），并直接将此动态合成的矩形下发给墨水屏驱动执行局部刷新。
  * 该优化彻底解耦了“排版位置”与“局刷区域”，允许用户在网页端任意拖拽摆放多个局刷组件而无需考虑局刷区域的繁琐配置。

##### 优化 12：通道级抽象色彩映射 (Abstract Color Channel Mapping)
* **现状问题**：不同的墨水屏具备不同的显存物理通道。例如三色屏（UC8179）拥有 Black/White 和 Red 两个独立的显存 Buffer；四色屏（JD79668）或彩色屏则有不同的通道映射。如果 `UI_BUNDLE` 模板中的组件硬编码了具体的物理色值（如 RGB565 或特定物理颜色代码），会导致同一套模板无法跨屏幕驱动重用。
* **改进方案**：
  * 在模板中采用**逻辑通道色 (Logical Color Channel)** 抽象表示颜色：
    * `0x00` -> 通道 0（背景色 / 白色）
    * `0x01` -> 通道 1（主前景色 / 黑色）
    * `0x02` -> 通道 2（辅助前景色 / 红色或灰色）
  * MCU 的 GUI 渲染引擎在解析模板时，根据当前 active 墨水屏驱动的物理通道特性，将这些逻辑通道映射到具体的物理显存（如 `0x01` 写入 B/W Buffer，`0x02` 写入 Red Buffer）。
  * 此举实现了 UI 样式与屏幕物理颜色的彻底解耦，模板只需关心“哪里是强调色，哪里是背景色”，具体色彩呈现由硬件驱动层决定，极大提升了模板的跨屏重用度。

##### 优化 13：常量名词中文字符串字典集成 (Constant Names Dictionary Integration)
* **现状问题**：在目前的资源分配中，用于翻译农历月（"正月".."腊月"）、农历日（"初一".."三十"）、天干地支（"甲子".."癸亥"）、生肖（"鼠".."猪"）以及24节气的常量字符串，是作为 8 个独立的 Type 2 资源存放在 Flash 中的。MCU 在渲染界面时，为了拼装“丙午年 腊月 初八 大寒”等字串，需要通过 8 次不同的 Resource ID 去寻址和读取 Flash，不仅产生多次 I/O 延迟开销，而且增加了固件管理 8 个不同 Entry ID 的代码复杂度。
* **改进方案**：
  * 将这些 100% 恒定不变的静态名词汉字字串字典，统一打包集成到 `LCDB` 主头部（Master Header）之后的第一个 **静态常量名字典区 (Static Name Dictionary Block)** 中。
  * 数据库的 Resource ID 缩减为 1 个。MCU 渲染农历文本时，仅需要对这单个 `LCDB` 资源执行一次偏移寻址，即可按索引直接流式读取出对应的汉字字串。
  * 此方案不仅消除了 8 个资源 Entry 的冗余描述符，还将多路 Flash I/O 合并为单路，极大地精简了 C 端的资源读取代码和网页打包逻辑。

##### 优化 14：时区与夏令时策略的客户端解耦 (Timezone & DST Offloading)
* **现状问题**：设备可能被销往全球不同时区，且不同国家有不同的夏令时（DST）切换规则。如果将时区转换和夏令时规则硬编码在 MCU 固件中，不仅需要引入数百字节的时区规则数据库，还需要处理复杂的本地 RTC 闰秒 and 时差修正算法。
* **改进方案**：
  * 坚守“MCU 纯渲染，计算全靠客户端”的解耦原则。
  * 所有的时区计算、夏令时切换规则全部在手机/网页客户端执行。客户端在发起 BLE 时间同步时，直接计算出当前时区偏移后的 **显示本地时间（Local Time）** 写入 MCU RTC。
  * MCU RTC 仅负责维护这唯一的一个显示本地时间，无需感知任何时区和 DST 的存在，从而让极简的 RTC 逻辑和每日跨天逻辑完全从全球复杂的时钟政治学中解耦出来。

##### 优化 15：文件读取 HAL 硬件抽象隔离 (HAL Read Abstraction for Simulation)
* **现状问题**：在目前的工程架构中，UI 模板和资源数据库读取函数（如 `epd_ext_resource_read`）是与 nRF SDK 平台底层的 SPI Flash 物理驱动高度绑定的。当我们在 PC 端模拟器 `emulator.c` 中编译和运行相同的 GUI 时，模拟器因为没有物理 Flash 芯片，不得不编写一套完全不同的读取分支代码，容易造成“真机与模拟器实现不一致”的隐患。
* **改进方案**：
  * 定义统一的资源读取硬件抽象层（HAL）接口。
  * 在 nRF MCU 平台：底层对接物理 SPI Flash 驱动。
  * 在 PC 仿真平台：底层利用 C 语言标准文件操作接口 `fread()` 映射读取本地 `resource_bundle.bin` 镜像文件。
  * 这样所有的核心逻辑（`GUI.c` / `Lunar.c`）均只调用统一的 HAL 接口，实现 **100% 同源跨平台编译**，使得在 PC 模拟器端可以极其方便、一致地测试和验证布局渲染与日历库。

##### 优化 16：墨水屏异步物理刷新与 CPU 深度睡眠解耦 (Asynchronous EPD Refresh & Low-Power Sleep)
* **现状问题**：跨天更新（午夜 00:00）或触发全屏刷新时，电子纸屏幕的物理加载和变色过程一般需要 3~4 秒（以 UC8179 为例）。如果 MCU 在这几秒钟内使用 `while(BUSY == 1)` 进行阻塞等待，CPU 将持续运行在 64MHz 的高频工作模式，产生巨大的功耗（几毫安电流），这对于依靠纽扣电池运行的超低功耗价签来说是极其致命的。
* **改进方案**：
  * 将“发出重绘画面”和“等待刷新完成”的逻辑异步解耦。
  * 当固件向 EPD 控制器发出开始刷新的 SPI 命令后，立即将 CPU 的主外设全部断电，并通过 `__WFE()` 或 `__WFI()` 指令让 CPU 进入极低功耗的 System ON 深度睡眠模式。
  * 利用 nRF52 的 GPIOTE 外设，把墨水屏驱动板的 `BUSY` 物理引脚（下降沿/上升沿）注册为系统唤醒中断源。
  * 当墨水屏物理刷新完成、`BUSY` 信号翻转时，硬件自动触发 GPIOTE 中断唤醒 CPU。此时 CPU 再起来关闭 EPD 的供电管脚（LDO En Pin），重新进入睡眠。
  * 该方案把刷新期间 CPU 的工作时间由 4000 毫秒压缩至低于 10 毫秒，实现了 CPU 运算与屏幕物理刷新延迟的深度解耦与极致省电。

##### 优化 17：多年份国家法定调休配置的结构化支持 (Structured Multi-Year Holiday Configuration)
* **现状问题**：在原版 `build_ext_resources.py` 打包脚本中，调休配置是一个扁平化的 C 风格数组 `HOLIDAYS`，且只绑定了一个全局常数 `HOLIDAY_YEAR = 2026`。这种设计在面对 2026 和 2027 多年份的统一数据库生成时，脚本将无法区分 2027 年的节日和调休数据，导致 2027 年的数据无法打入 `LCDB` 的 `holiday_type`。
* **改进方案**：
  * 将 `build_ext_resources.py` 中的 `HOLIDAYS` 重构为以**年份为 Key 的字典（Dictionary）格式**：
    ```python
    HOLIDAYS = {
        2026: [0x0101, 0x0102, 0x0103, 0x1104, ...], # 0x1XXX 代表上班
        2027: [0x0101, ...],
    }
    ```
  * 打包脚本在编译 `LCDB` 数据库的 Daily Records Table 时，先按当前年份定位到对应的调休数据，再将其写入 `holiday_type`。此举彻底解决了多年份日历库下，调休信息无法按年份正确关联的缺陷。

##### 优化 18：字库子集自动化提取与打包流水线 (Automated Font Subset Pipeline)
* **现状问题**：精细的界面需要多种中文字体字号，但完整中文字库体积太大。虽然提出了字库子集化方案，但若每次修改界面（比如添加了一个新节气或微调了提示词）都需要人工去手动提取新字库子集并打包，其开发和维护成本将变得极其高昂。
* **改进方案**：
  * 在 `build_ext_resources.py` 资源打包脚本中集成 **自动化字集提取器（Automated Font Subset Extractor）**。
  * 脚本在构建时：
    1. 自动扫描 `ui_layout` 模板中的所有静态文本；
    2. 自动收集 `SOLAR_FESTIVALS`、`LUNAR_FESTIVALS` 以及常量名字典中的所有汉字；
    3. 合并去重生成最终的“必用汉字清单”；
    4. 自动检索原始大字库文件（`fonts.c` 中的原点阵），自动提取出 these 必用字的 Glyph Bitmaps，打包成专用的 Mini-Font 资源并与 `UI_BUNDLE` 协同写入外置 Flash。
  * 这种全自动化的字模剪裁流水线，能够让开发者在网页端随意编辑 UI 而无需任何手动的字库裁剪工作。

##### 优化 19：基于状态反馈的蓝牙资源写入流控与自适应分片 (Flow-Controlled BLE Resource Writing)
* **现状问题**：通过 BLE 的 `EPD_CMD_RES_WRITE (0x41)` 指令写入 `UI_BUNDLE` 或 `LCDB` 时，由于外置 SPI Flash 执行页写入（Page Write）和扇区擦除（Sector Erase）需要毫秒级的物理硬件延迟，若网页端不间断地高速发送分片包，可能会在 MCU 端的 BLE 环形缓冲区中产生积压甚至溢出丢包。
* **改进方案**：
  * **引入状态应答流控**。
  * MCU 接收到每一个分片包并成功写入 Flash 后，通过 BLE 回复一个包含当前已写入偏移量的应答包。
  * 网页端采用异步等待应答（Promise-based Acknowledge）机制，收到上一片的写入确认后再发送下一片。同时，网页端利用 `EPD_CMD_RES_STATUS (0x43)` 查询设备最大支持的 MTU 分片大小，实现**大小自适应**，彻底保障超长资源传输过程中的零丢包与高可靠性。

##### 优化 20：MCU ROM 内置默认 Fallback 资源（容灾应急机制）
* **现状问题**：在目前的架构中，所有的界面模板（`UI_BUNDLE`）和日历数据库（`LCDB`）均依赖外置 Flash 读取。如果在出厂烧录时未下载资源包，或者外置 Flash 芯片发生硬件开路、虚焊或损坏，设备开机将因为魔法数不匹配或读失败直接进入 HardFault 崩溃死机循环，无法给用户任何提示，也无法再次开启蓝牙接收修复数据。
* **改进方案**：
  * **固件 ROM 内置备用紧急资源**。
  * 在 MCU 内部 Flash（ROM）中，静态固化一个极简的默认 UI 模板（`DEFAULT_UI_BUNDLE`，约 200 字节）和单年份极简农历库（`DEFAULT_LCDB`，约 3KB）。
  * 固件启动时，如果读取外置 Flash 失败或校验 CRC32 报错，解析器会自动无缝切回内部 ROM 读取这套内置备用资源，在屏上显示“外置 Flash 损坏或未初始化，请通过蓝牙重新同步资源”等应急提示。
  * 此方案不仅提供了极佳的工业级容灾自愈能力，还使得刚下线的空白新板也能够正常开机、开屏显示测试状态，方便工厂批量测试。

##### 优化 21：严格的绘图坐标与越界安全校验 (Rigid Component Boundary Verification)
* **现状问题**：`UI_BUNDLE` 模板中的所有组件位置（`x, y`）与尺寸（`w, h`）均由网页端编辑下发。如果网页端计算错误、传输产生位翻转或者下发了恶意构造的 UI 模板，导致某个组件的坐标值（例如 `x = 850, y = 500`）超出了墨水屏物理分辨率范围（800x480）。MCU 的渲染引擎若直接对其缓冲区执行像素写操作，会造成底层显存指针越界写，进而导致内存被篡改、HardFault 死机等严重系统崩溃。
* **改进方案**：
  * **引入全组件坐标动态裁剪与校验（Clipping & Bound-check）**。
  * GUI 渲染引擎在解析和渲染任何 UI 组件前，强制对组件坐标和边界执行严格的合法性验证：
    ```c
    if (comp->x >= SCREEN_WIDTH || comp->y >= SCREEN_HEIGHT ||
        (comp->x + comp->w) > SCREEN_WIDTH || (comp->y + comp->h) > SCREEN_HEIGHT) {
        // 判定组件坐标越界，拒绝绘制，并优雅跳过或限制在边界内裁剪（Clip）绘制
    }
    ```
  * 这一内置的安全边界拦截器能完全隔离外部输入带来的内存越界风险，确保固件底层的绝对安全性。

##### 优化 22：网页端与固件特性的向下兼容与协商 (Forward Feature Negotiation)
* **现状问题**：随着固件不断迭代升级，未来可能会引入新型的 UI 组件（例如温湿度趋势曲线组件、BLE 信号强度图标等）。如果网页端升级了布局编辑器，用户制作了新版界面，但设备上运行的依旧是老固件。一旦老固件强行解析含有新组件的 `UI_BUNDLE`，会导致识别不了未知组件类型，引发渲染故障甚至死机。
* **改进方案**：
  * **引入特性标志协商机制（Feature Flag Negotiation）**。
  * 固件在蓝牙广播或特征值读取中，除了上报固件版本外，增加上报一个 32 位的**特性掩码（Feature Mask）**，每一位代表设备所支持的组件类型和新特性。
  * 网页端连接设备后，读取该掩码。若发现固件版本过低、不支持新组件，布局编辑器将自动隐藏新组件，降级输出与当前固件完全兼容的资源包。
  * 该方案能完美解决软硬件版本迭代的兼容性断层，为后续功能拓展留出免死机、自适应的安全通道。

---

## 3. 数据库结构设计 (Database Format Design)

数据库在 Flash 中作为一个单一的 Resource Block（资源类型 6，ID 1）存储。整体结构由 **主头部 (Master Header)**、**年份偏移表 (Year Offset Table)** 和 **年份数据块 (Year Data Blocks)** 组成：

```
+-------------------------------------------------------+
|                 Master Header (16B)                   |
+-------------------------------------------------------+
|            Year Offset Table (year_count * 4B)         |
+-------------------------------------------------------+
|                 Year 2026 Block                       |
|  - Year Header (8B)                                   |
|  - Daily Records (365 * 12B)                          |
|  - String Pool (UTF-8, Variable Length)               |
+-------------------------------------------------------+
|                 Year 2027 Block                       |
|  - Year Header (8B)                                   |
|  - Daily Records (365 * 12B)                          |
|  - String Pool (UTF-8, Variable Length)               |
+-------------------------------------------------------+
```

### 3.1 主头部 (Master Header - 16字节)
| 偏移 (Offset) | 字段名 (Field) | 类型 (Type) | 说明 (Description) |
| --- | --- | --- | --- |
| 0x00 | magic | uint32_t | 识别魔法数，固定为 `0x4C434442` ("LCDB") |
| 0x04 | version | uint8_t | 格式版本，当前为 `1` |
| 0x05 | year_count | uint8_t | 包含的年份总数，例如 `2` (表示 2 年) |
| 0x06 | start_year | uint16_t | 起始年份，例如 `2026` |
| 0x08 | record_size| uint8_t | 单个每日记录大小，固定为 `12` 字节 |
| 0x09 | reserved | uint8_t[7] | 保留字节，用作对齐 |

### 3.2 年份偏移表 (Year Offset Table)
紧接在主头部后面，长度为 `year_count * 4` 字节。
每个元素为 `uint32_t`，指示该年份的数据块（Year Block）相对于本资源起始位置的偏移地址。

### 3.3 年份数据块 (Year Block)
每个年份的数据块是独立的，包含三个部分：
1. **年份头部 (Year Header - 8字节)**
   * `year` (uint16_t): 本块对应的公历年份（如 2026）。
   * `total_days` (uint16_t): 当年的总天数（365 或 366）。
   * `string_pool_offset` (uint32_t): 字符串池相对于本块起始位置的偏移量。
2. **每日记录表 (Daily Records Table)**
   * 长度为 `total_days * 12` 字节。每天占 12 字节，索引从 0 开始（对应当年的第 1天到第 365/366 天）。
3. **字符串池 (String Pool)**
   * 存储该年份所有节日、特殊自定义文本的 UTF-8 字符串，以 NUL (`\0`) 分割。

### 3.4 每日记录结构 (`daily_record_t` - 12字节)
```c
typedef struct {
    uint8_t lunar_month;     // 农历月份 (1-12)。最高位 Bit 7 表示是否为闰月 (1:闰月, 0:平月)
    uint8_t lunar_day;       // 农历日期 (1-30)
    uint8_t jiazi_year;      // 六十甲子年索引 (0-59)，对应干支年 (0=甲子, 1=乙丑...)
    uint8_t jiazi_month;     // 干支月 (0-59, 预留用于高精度八字排盘)
    uint8_t jiazi_day;       // 干支日 (0-59, 预留)
    uint8_t solar_term;      // 24节气索引 (0: 无节气, 1-24: 对应立春到大寒)
    uint8_t holiday_type;    // 调休状态 (0: 普通工作日/周末, 1: 法定休假"休", 2: 调休上班"班")
    uint8_t reserved;        // 对齐字节
    uint16_t festival_offset;// 节日名称在当块字符串池中的偏移量。0xFFFF 表示当天无节日
    uint16_t special_label;  // 特殊自定义显示字符串的偏移量。0xFFFF 表示无
} daily_record_t;
```

---

## 3. 网页端/脚本编译方案 (Web-side Compiler Design)

在 `tools/build_ext_resources.py` 中，新增 `parse_lunar_db_resources()` 执行预生成。支持 2026–2027 年的数据生成，其余年份以 2026 年数据填充。

### 3.1 农历转换与节气算法移植
使用已有的内置换算数据（`LUNAR_SOLAR_1_1`、`LUNAR_MONTH_DAYS`、`LUNAR_JIEQI_YEAR_BITS`、`LUNAR_JIEQI_BASE_DAYS`），在 Python 脚本中实现农历和节气转换逻辑：

1. **公历到农历天数计算**：
   * 基于公历日期计算当前年份的 Day of Year 偏移。
   * 计算该年份农历正月初一（Solar 1-1）的公历 Day of Year。
   * 累加计算出对应的农历月份、日期以及是否为闰月。
2. **节气计算**：
   * 根据当年在节气比特表 `LUNAR_JIEQI_YEAR_BITS` 的状态，微调当年 24 个节气的基准日期，计算出确切公历日期。

### 3.2 节日与调休预处理逻辑
网页/脚本在打包时，基于当前公历日期 `(year, month, day)` 执行以下检测，并将匹配结果拼装写入：

1. **静态节日匹配**：
   * 公历静态节日（`SOLAR_FESTIVALS`）：如 1月1日 "元旦节"。
   * 农历静态节日（`LUNAR_FESTIVALS`）：如 5月5日 "端午节"。
2. **规则节日动态计算**：
   * **除夕**：春节（农历1月1日）的前一天。通过判断次日是否为正月初一动态标定（支持农历 12月29日 或 30日）。
   * **母亲节**：5月的第二个星期日。
   * **父亲节**：6月的第三个星期日。
   * **感恩节**：11月的第四个星期四。
3. **调休状态计算**：
   * 检索调休数据（`HOLIDAYS`）。若日期匹配且标志位为 `1`，写入 `holiday_type = 2` ("班")；若匹配且标志位为 `0`，写入 `holiday_type = 1` ("休")；其余为 `0`。

### 3.3 字符串去重与打包装装
* 针对每年的节日文字，建立局域字符串池进行去重，计算各个字符的字节偏移值（如 "春节" -> offset 0，"元宵节" -> offset 7）。
* 使用 `struct.pack` 格式化为 12 字节记录。
* 计算块校验与总 Manifest 信息。

---

## 4. 固件查询引擎方案 (Firmware Engine Design)

### 4.1 头部与偏移量缓存
为减少每次查表时的 Flash 读取开销，`GUI/Lunar.c` 中维护一组轻量全局变量，在首次调用时自动初始化：
```c
static bool s_db_inited = false;
static uint8_t s_year_count = 0;
static uint16_t s_start_year = 0;
static uint8_t s_record_size = 12;
static uint32_t s_year_offsets[2]; // 缓存所有年份的物理偏移量 (2026, 2027)
```

### 4.2 数据库初始化 (`lunar_db_init`)
```c
int lunar_db_init(void) {
    if (s_db_inited) return 0;
    
    epd_ext_resource_entry_t entry;
    if (epd_ext_resource_get_info(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, &entry) != NRF_SUCCESS) {
        return -1;
    }
    
    uint8_t header[16];
    if (epd_ext_resource_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 0, header, 16) != NRF_SUCCESS) {
        return -2;
    }
    
    uint32_t magic = *(uint32_t*)&header[0];
    if (magic != 0x4C434442) return -3; // 魔数校验
    
    s_year_count = header[5];
    s_start_year = *(uint16_t*)&header[6];
    s_record_size = header[8];
    if (s_year_count > 12) s_year_count = 12;
    
    // 缓存各年份偏移量表
    if (epd_ext_resource_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 16, 
                              s_year_offsets, s_year_count * 4) != NRF_SUCCESS) {
        return -4;
    }
    
    s_db_inited = true;
    return 0;
}
```

### 4.3 记录查询与越界平稳降级 (`lunar_db_get_record`)
当查询的年份不在数据库范围内时（例如查询 2025 年），引擎将平稳降级至 `start_year` (2026年)，其余年份均按照 2026 年显示，防止系统崩溃：
```c
int lunar_db_get_record(uint16_t year, uint8_t month, uint8_t day, daily_record_t* out_record) {
    if (!s_db_inited && lunar_db_init() != 0) return -1;
    
    if (year < s_start_year || year >= s_start_year + s_year_count) {
        year = s_start_year; // 平稳降级
    }
    
    uint8_t year_idx = year - s_start_year;
    uint32_t block_offset = s_year_offsets[year_idx];
    
    uint16_t total_days = 0;
    if (epd_ext_resource_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 
                              block_offset + 2, &total_days, 2) != NRF_SUCCESS) {
        return -2;
    }
    
    // 计算 Day of Year (doy) 索引
    uint16_t doy = get_day_of_year(year, month, day);
    if (doy >= total_days) doy = total_days - 1;
    
    uint32_t record_addr = block_offset + 8 + doy * s_record_size;
    if (epd_ext_resource_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 
                              record_addr, out_record, sizeof(daily_record_t)) != NRF_SUCCESS) {
        return -3;
    }
    return 0;
}
```

---

## 5. 显示模式拆解与接口适配 (Mode Decoupling & GUI Adaptation)

为了保持 `GUI/GUI.c` 渲染代码最小化改动，需对兼容接口进行精确映射适配：

### 5.1 天干地支与生肖的 60-甲子索引映射
在标准中华历法中，天干和地支按 60 年循环一次：
* 甲子年对应的索引为 0，乙丑为 1，癸亥为 59。
* 天干对应 `jiazi_year % 10`。
* 地支对应 `jiazi_year % 12`。
* 生肖与地支一一对应，对应 `jiazi_year % 12`。

兼容接口实现：
1. **`LUNAR_SolarToLunar`**：
   * 从数据库查询当前公历日期的 `daily_record_t`。
   * 将 `IsLeap`、`Month`（去Bit7）、`Date` 写入 `Lunar_Date` 结构体。
   * 将 `lunar->Year` 临时赋值为 `rec.jiazi_year` 充当甲子索引。
2. **`LUNAR_GetStem` / `LUNAR_GetBranch` / `LUNAR_GetZodiac`**：
   * 分别直接返回 `lunar->Year % 10` 和 `lunar->Year % 12`。
3. **中文文字映射表**：
   将所有的月份名称、初一/三十日期名称、天干/地支/生肖中文名称声明为 MCU 内部的静态常量只读数组（ROM 占用极小，总共约数百字节，但省去了每次去 Flash 中查找小短文字符串的繁琐开销）：
   ```c
   static const char* const c_stems[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
   static const char* const c_branches[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
   static const char* const c_zodiacs[] = {"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};
   ```

### 5.2 节日与二十四节气优先级过滤
`GUI.c` 中的 `GetFestival` 函数被大大简化，它接收查询日期并决定当天绘制的副标题：
1. **第一优先级：数据库记录中的 `festival_offset`**
   * 若不为 `0xFFFF`，调用 `lunar_db_get_string()` 读取对应的节日名称文本。
2. **第二优先级：数据库记录中的 `solar_term`**
   * 若不为 `0`，调用 `LUNAR_GetJieQiName(solar_term - 1)` 获取对应的二十四节气名称。对于 "清明"（节气索引为 6），后补 "节" 字作为节日输出。
3. **兼容函数 `GetJieQiStr` / `GetJieQi`**：
   * 顺次向后扫描 1~16 天的 `daily_record_t`，通过 `solar_term` 标志快速定位最近的下一个节气，省去原本极其复杂的数学计算逻辑。

### 5.3 调休状态戳渲染
在生成月历网格缓存 `PrepareMonthRenderCache` 时：
* 去掉之前读取整个 `holiday_adjustments` 头部和遍历整个数组的逻辑。
* 替换为在遍历月历天数时，直接调用 `LUNAR_GetHolidayMark(year, month, day, &has_mark, &is_work)`：
  * 若 `rec.holiday_type == 1`：`has_holiday_mark = 1`，`holiday_work = 0` (标记为 "休")。
  * 若 `rec.holiday_type == 2`：`has_holiday_mark = 1`，`holiday_work = 1` (标记为 "班")。
  * 其余 `rec.holiday_type == 0`：`has_holiday_mark = 0`，`holiday_work = 0`。

---

## 6. 文件修改清单 (Files to Modify)

| 目标文件 (File Path) | 修改说明 (Key Modifications) |
| --- | --- |
| `EPD/epd_ext_resources.h` | 增加 `EPD_EXT_RES_TYPE_LUNAR_DB` (6U) 与 `EPD_EXT_LUNAR_DB_ENTRY` (1U) 定义。 |
| `tools/build_ext_resources.py` | 增加 `parse_lunar_db_resources` 预处理流程，将旧版 Lunar/Holiday 从打包清单移出，替换为新版数据库打包逻辑。 |
| `GUI/Lunar.h` | 声明 `daily_record_t`、查询接口及兼容方法。 |
| `GUI/Lunar.c` | 实现 `lunar_db_init`, `lunar_db_get_record`, `lunar_db_get_string`；保留基础时间转换运算（如 `transformTimeStruct`），移除所有节气/农历换算表和复杂算法。 |
| `GUI/GUI.c` | 简化 `GetFestival` 和 `PrepareMonthRenderCache` 内的节日调休匹配，移除不再使用的私有静态解析函数。 |

---

## 7. 解耦再拆解与优化设计 (Decoupling Re-decomposition & Optimized Design)

> 本章基于对现有代码库的实测核对（`GUI/Lunar.c`、`GUI/GUI.c`、`EPD/epd_ext_resources.h`、`tools/build_ext_resources.py`、`emulator.c`），把第 2.4 节抽象罗列的 22 项优化，重新组织成"可落地的分层契约 + 分阶段路线图"，作为当前方案的执行蓝本。

### 7.1 现状与目标差距核对 (Gap Assessment)

当前项目处于"UI 模板化已先行、农历数据化未启动"的半程状态。核对结论如下：

| 能力项 | 文档目标 | 代码现状 | 状态 |
| --- | --- | --- | --- |
| 农历数据库 `LUNAR_DB` (Type 6) | 单一 Type6 资源 | 仍为 Type2 农历表 + Type3 调休表 | ❌ 未启动 |
| `daily_record_t` / `lunar_db_get_record` | 查表引擎 | 全库无此符号 | ❌ 未启动 |
| MCU 农历/节气运算 | 全部下放脚本端 | `LUNAR_SolarToLunar` / `GetJieQi` 仍运行时计算 | ❌ 未启动 |
| 打包端多年份 | 年份字典 | `HOLIDAY_YEAR=2026` 单年 + 扁平 `HOLIDAYS` | ❌ 未启动 |
| 单日 RAM 缓存 `s_today_record` | 12B/天 | 用整月缓存 `m_month_render_cache`(31天) | ⚠️ 替代实现，待收敛 |
| `UI_BUNDLE` 模板渲染 | 数据驱动 | `ui_bundle_header_t` + 双层渲染缓存已实现 | ✅ 已做 |
| 坐标越界硬校验 | Clipping | `ui_rect_fits_screen` 已实现 | ✅ 已做 |
| 资源写入 Staging + Commit | 防砖 | `commit_staging` 已实现（单区，非 A/B） | ⚠️ 部分 |
| 动态数据绑定 | 通用 Bind ID | 写死枚举 `ui_text_data_id_t` + `switch` | ⚠️ 弱耦合残留 |
| 局刷区域 | 动态脏矩形 | `ui_mode_record_t.partial_*` 硬编码 | ⚠️ 强耦合残留 |
| Schema/屏幕规格/逻辑色 | 头部约束字段 | `ui_bundle_header_t` 仅 `version/width/height` | ❌ 缺失 |
| 模拟器资源读取 HAL | 统一接口 | `emulator.c` 未抽象，路径与真机分叉 | ❌ 缺失 |

**核心矛盾**：UI 侧成本已付但算法侧收益（ROM 瘦身）未拿到；`Lunar.c` 仍占 37KB。因此路线图必须优先补齐数据/算法层。

### 7.2 解耦再拆解：四层契约模型 (Four-Layer Contract Model)

把原"数据 / UI / 算法"三层，细化为**带明确接口契约的四层**，新增最底部的 HAL 层以打通真机与模拟器：

```
+-----------------------------------------------------------+
| L3 表现层 Presentation (GUI.c + Adafruit_GFX)             |
|   职责：纯绘制。译码 UI_BUNDLE -> 绑定运行时数据 -> 画像素   |
|   契约：只认 render_ctx_t（组件 + 数据绑定向量 + 逻辑色）    |
+-----------------------------------------------------------+
| L2 逻辑层 Logic (Lunar.c 瘦身后 + GUI 组织层)              |
|   职责：查表、优先级过滤、脏标记合成，不做数学换算            |
|   契约：lunar_db_get_record() / ui_bundle_get_*() / dirty  |
+-----------------------------------------------------------+
| L1 数据层 Data (外置 Flash: LCDB + UI_BUNDLE + FontSubset)|
|   职责：静态数据的唯一真源，脚本端预生成                     |
|   契约：Master Header / 单日记录 / 常量名字典 / 模板字节码   |
+-----------------------------------------------------------+
| L0 资源 HAL (res_hal_read/erase/write)                     |
|   职责：屏蔽 SPI Flash 与 PC fread 差异，真机/模拟同源       |
|   契约：res_hal_read(type,id,offset,buf,len)               |
+-----------------------------------------------------------+
```

**跨层铁律**：
1. 上层只能通过契约接口访问下层，禁止跨层直读物理地址。
2. 任何"每天内静态"的数据（农历/节气/节日/调休）只在跨天时穿越 L1→L2 一次，缓存在 RAM。
3. 任何"分钟级动态"的数据（HH:MM、电量、温度）只在 L3 通过数据绑定向量读取 RAM，绝不触碰 L1。

### 7.3 关键接口与数据结构设计 (Interfaces & Structures)

#### (1) L0 资源 HAL —— 收敛优化 15
统一读取入口，`GUI.c` / `Lunar.c` 只依赖它，真机对接 SPI Flash，PC 对接 `resource_bundle.bin`：
```c
/* res_hal.h —— 平台无关资源读取契约 */
ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len);
/* nRF: 转发 epd_ext_resource_read()；PC: fseek+fread 到 bin 镜像 */
```

#### (2) L1/L2 单日缓存 —— 收敛整月缓存为单日缓存（优化 1/13）
用文档的 12B 单日记录替换 `m_month_render_cache`，把"每天 1 次 Flash"落到实处：
```c
static daily_record_t s_today_record;    // 12B，跨天时刷新一次
static uint8_t        s_last_rendered_day;// 跨天判定
/* 常量名词字典合并为 1 个 LCDB 内部块，渲染农历文本单路 I/O */
```

#### (3) L2 UI 头部扩展 —— 补齐前向兼容与硬件匹配（优化 7/8/12）
在现有 `ui_bundle_header_t` 追加约束字段，加载前握手校验：
```c
typedef struct {
    uint16_t version;
    uint8_t  schema_major;      // 主版本越界 -> Fallback + BLE 上报
    uint8_t  schema_minor;      // 次版本不识别 -> 跳过未知命令
    uint16_t target_width;      // 与当前驱动物理宽比对
    uint16_t target_height;
    uint8_t  required_color_depth;
    /* ... 原 width/height/offset 字段保留 ... */
} ui_bundle_header_t; // 扩展后
```
渲染时颜色改用**逻辑通道** `0x00背景/0x01主前景/0x02辅前景`，由驱动层映射到 B/W 或 Red buffer，替换现有硬编码 `GFX_RED/GFX_BLACK`。

#### (4) L3 数据绑定向量 —— 消除写死枚举（优化 2）
把 `ui_text_data_id_t` 的 `switch` 升级为 RAM 绑定表，组件 `data_source` 存 Bind ID：
```c
typedef struct { uint8_t id; const void* val; uint8_t kind; } data_bind_t;
/* 0x01 HH:MM  0x02 电量  0x03 蓝牙图标  0x04 温度 ... */
static data_bind_t s_bind_vec[DATA_BIND_MAX];
```
新增动态源只登记向量项，不再改渲染 `switch`。

#### (5) L3 动态脏矩形 —— 解除排版与局刷耦合（优化 11）
删除 `ui_mode_record_t.partial_x/y/w/h`，改为运行时脏标记合成：
```c
/* 每分钟重绘：比对各绑定组件 新值!=旧值 -> 标脏 -> 合并包围盒 -> 局刷 */
bool ui_collect_dirty_rects(dirty_rect_t out[], uint8_t* n);
```
配合刷新频率下限 60s（优化 9），保护墨水屏寿命。

### 7.4 分阶段落地路线图 (Phased Roadmap)

| 阶段 | 目标 | 关键改动 | 依赖 | 收益 |
| --- | --- | --- | --- | --- |
| **P0 打包地基** | 多年份 + LCDB 生成 | `build_ext_resources.py`：`HOLIDAYS` 改年份字典；新增 `parse_lunar_db_resources` 生成 Type6 | 无 | 解除 2027 调休缺陷（优化 17） |
| **P1 查询引擎** | MCU 只查表 | 新增 `daily_record_t`、`lunar_db_init/get_record/get_string`；`Lunar.c` 删换算表；`s_today_record` 单日缓存 | P0 | ROM 大幅回落，I/O 降至 1次/天（优化 1/13） |
| **P2 UI 解耦收尾** | 去残留耦合 | 动态脏矩形 + 数据绑定向量 | P1 | 网页端自由拖拽多动态组件（优化 2/11） |
| **P3 健壮性** | 防错前向兼容 | Schema 握手 + 屏幕规格校验 + 逻辑色通道 + Glyph Fallback | P2 | 跨屏/跨版本不死机（优化 7/8/10/12） |
| **P4 同源与省电** | 工程化 | 资源 HAL 抽象 + 模拟器对齐 + 异步 EPD 刷新睡眠 | P1 | 真机/模拟一致、极致省电（优化 15/16） |

> 每阶段以"可编译 + 模拟器渲染通过"为完成标准；P0/P1 为主目标关键路径，务必先行。

### 7.5 验收标准与风险 (Acceptance & Risks)

**验收**：
- ROM：完成 P1 后 `Lunar.c` 目标 ≤ 8KB（当前约 37KB）。
- I/O：单日内 LCDB 读取次数 = 1（用日志计数验证）。
- 一致性：同一 `resource_bundle.bin` 在 `emulator.c` 与真机渲染像素一致。
- 兼容：旧屏误传 800×480 模板时进入 Fallback 而非崩溃。

**风险与规避**：
1. *数据/代码双轨期*：P0→P1 之间会同时存在旧 Type2/3 与新 Type6，需用编译开关或 manifest 版本隔离，避免两套逻辑同跑。
2. *单日缓存跨天竞态*：跨天判定与 BLE 静默校时可能并发触发读记录，需在跨天更新处加轻量重入保护。
3. *Schema 演进*：P3 前下发的旧模板无 schema 字段，解析器须对缺失字段按 `schema_major=1` 兜底，保证平滑升级。

### 7.6 文件修改清单增补 (Additional Files to Modify)

在第 6 章基础上补充：

| 目标文件 | 增补修改说明 | 所属阶段 |
| --- | --- | --- |
| `EPD/res_hal.h` / `res_hal.c`（新增） | 定义并实现平台无关资源读取 HAL | P4 |
| `emulator.c` | 对接 `res_hal` 读取本地 `resource_bundle.bin`，删除分叉读取分支 | P4 |
| `GUI/GUI.c` | 扩展 `ui_bundle_header_t`；删除 `ui_mode_record_t.partial_*`，新增脏矩形合成与数据绑定向量；颜色改逻辑通道 | P2/P3 |
| `EPD/epd_ext_resources.h` | 新增 schema/屏幕规格/逻辑色相关常量 | P3 |
| `main.c` | 跨天判定链（`s_last_rendered_day`）与异步刷新睡眠（GPIOTE BUSY 唤醒）挂接 | P1/P4 |

### 7.7 补充建议：让解耦"可验证、可维护、诚实降级" (Supplementary Recommendations)

> 前几节解决了"如何分层"，本节解决"如何保证分层不在演进中失效"。核心观点：真正决定解耦成败的不是再多列优化，而是给已有分层加上**编译期/CI 能验证的护栏**、消灭**双份数据契约**、并把降级从"静默错误"改成"诚实提示"。

#### 建议 A：防止"纸面解耦"落空的三道工程护栏
分层契约最大的风险是约定写在文档里、代码却层层直穿。建议让边界由编译/CI 强制，而非靠自觉：

1. **物理拆分 `GUI.c`（当前约 83KB 单文件）**：现状 L2 逻辑（查表、优先级过滤）、L3 渲染（GFX 绘制）、Fallback 屏混在一个文件。按层拆为 `lunar_db.c`（查询）/ `ui_runtime.c`（字节码译码）/ `ui_render.c`（绘制）。**文件即边界**，跨层调用在链接期即可暴露，比口头约定可靠。
2. **黄金图回归测试（锁死"模拟器 == 真机"契约，补齐优化 4/15 临门一脚）**：CI 用 `emulator.c` 渲染固定的 `resource_bundle.bin`，对输出做像素哈希并与提交的 golden PNG 比对。任何布局/数据改动引起的像素漂移自动拦截，避免"同源渲染"沦为口号。
3. **RAM/ROM 静态预算断言**：nRF52811 仅 24KB RAM。对 `MAX_UI_ITEMS * sizeof(component) + 各渲染缓存` 增加 `_Static_assert` 预算，并在 CI 检查 linker map。"零动态分配"必须配一个"总量不超标"的守门，防止组件数增长时悄然爆栈。

#### 建议 B：数据契约的"单一真源"（当前最隐蔽的耦合）
同一份二进制布局目前被定义两遍——打包端 `struct.pack(...)` 与固件端 `daily_record_t` / `ui_component_record_t`。改一边忘另一边就会字段错位，且极难排查。

* **收敛到单一权威源**：把结构体字段与所有共享枚举（`holiday_type`、`solar_term`、`data_bind_id`、组件 `type`）集中定义，由其生成 C 头 / Python 常量 / JS 常量（可先做简单 codegen 脚本或共享 `schema.json`）。
* **往返自检兜底**：打包脚本 pack 出记录后，用与 C 等价的 unpack 回读校验并纳入 CI，杜绝 C/Python/JS 三处枚举漂移。

#### 建议 C：算法下放的边界与"诚实降级"UX（对现方案的修正意见）
现方案对超出 2026–2027 的年份"**静默按 2026 显示**"。脱网久置后 RTC 会自行走到 2028，屏幕将呈现**看似正确、实则错误**的农历/调休，用户无从察觉，UX 风险高。

* **方案 A（推荐）**：不静默降级，显示"农历数据待更新"角标/提示，宁可承认过期也不呈现假数据。
* **方案 B**：保留一个**极小的兜底农历算法**（仅算农历日，不含调休/节气），只在越界时启用，以换取脱网可用性；是否采用取决于产品定位（价签 vs 桌面日历）。
* **字库子集与 LCDB 的隐藏耦合**：字集提取（优化 18）须把 UI 文本 + LCDB 节日/自定义标签**合并为同一字符需求集**；打包时若某字不在字库应**直接构建失败**（错误左移到编译期），运行时 Glyph Fallback（优化 10）仅作保命，编译期拦截才是治本。

#### 建议 D：范围控制，警惕过度设计
22 项优化对 192KB/24KB 的 MCU 偏多，部分收益/成本不成正比，建议显式标注优先级、只跑"必做"：

* `jiazi_month` / `jiazi_day`（八字排盘预留）——价签场景基本无用，却占 2B/天 × 730 天。
* 时区/DST 客户端解耦（优化 14）、特性掩码协商（优化 22）——除非确定全球多设备，可后置。
* **建议给每项优化打上 `必做 / 可做 / 暂缓` 三档**，路线图只推进"必做"，其余按真实需求再启。

#### 建议落地归属
| 建议 | 对应护栏/修正 | 建议阶段 |
| --- | --- | --- |
| A1 拆分 `GUI.c` | 编译期边界 | P2 |
| A2 黄金图回归测试 | CI 契约锁定 | P4（依赖 HAL 对齐） |
| A3 RAM/ROM 静态预算 | 资源守门 | P1 起持续 |
| B 单一契约源 + 往返自检 | 维护性 | P0（打包重构同期做最省） |
| C 诚实降级 + 字集编译期拦截 | 正确性/UX | P1（降级）/ P0（字集） |
| D 优先级三档标注 | 范围控制 | 立即，用于校准路线图 |

### 7.8 P0/P1 落地记录 (Implementation Log)

> 本节记录已实际提交的代码改动与验证结果，作为路线图 P0/P1 的完成凭证。

#### 已完成改动
1. **P0 打包端（`tools/build_ext_resources.py`）**：新增 `parse_lunar_db_resources` 生成 Type6 LCDB（主头 + 年份偏移表 + 逐年块，每日 12B 记录 + 去重字符串池）；农历/节气/节日/调休算法由 C 1:1 移植到 Python，在打包期一次性预计算。`HOLIDAYS` 改为年份字典，解除 2027 调休缺陷。
2. **P1 固件端**：
   - `EPD/epd_ext_resources.h` 新增 `EPD_EXT_RES_TYPE_LUNAR_DB (6)` 及入口/魔数常量。
   - `GUI/Lunar.h`/`Lunar.c` 新增 `daily_record_t` 与 `lunar_db_init/get_record/get_string`（惰性初始化，零拷贝流式读字符串）。
   - **兼容层改为 DB 支撑**：`LUNAR_SolarToLunar`、`GetJieQi`、`GetJieQiStr` 重写为查库实现，删除位域换算表逻辑（`GetBitInt`/`SolarToInt`/表读取）。`Lunar.h` 中 `Lunar_Date.Year` 在库模式下临时承载六十甲子索引。
   - `GUI/GUI.c`：`GetFestival` 改为读 `festival_offset` + `solar_term`（清明补"节"）；`PrepareMonthRenderCache` 调休标志改取 `daily_record_t.holiday_type`；移除 `GetFestivalCount`/`ReadFestival`/`ReadHolidayHeader` 及 `Festival`/`MONTH_HOLIDAY_MARK_MAX`。

#### 对建议 C 的修正落地
LCDB 年份范围由 `2026–2027` 扩为 **`2025–2027`**。原因：设备默认时间戳为 `2025-01-01`，未校时开机若把 2025 静默降级为 2026，会显示"看似正确实则错误"的农历。纳入 2025 后，未校时默认年份即为精确数据。（脱网走到 2028+ 的诚实降级角标仍列为后续项。）

#### 验证方式（数据层等价回归，替代黄金图的先行手段）
新增 `tools/test/test_lunar_db.c`（+ 桩头 `tools/test/sdk_errors.h`），在 PC 上编译真实固件源 `GUI/Lunar.c`，以模拟的 `epd_ext_resource_read` 读取 `tools/epd_resources.bin`，**逐日**对比"旧算法路径复刻"与"新数据库路径"的农历日期、干支、二十四节气、节日、调休：

```bash
gcc -std=c11 -IGUI -IEPD -Itools/test tools/test/test_lunar_db.c GUI/Lunar.c -o /tmp/test_lunar_db
/tmp/test_lunar_db tools/epd_resources.bin
# ==== LCDB equivalence: 1095 days checked, 0 mismatch ====
```

结果：2025–2027 全年 **1095 天 0 处不一致**；固件 `make -f Makefile.nRF52_linux`（`-Werror`）通过，`text=55484B`，在 56KB 预算内。

> 说明：Win32 GDI 模拟器不便于 Linux 像素回归，故本阶段先用"数据层逐日等价"精确锁定切换的真实风险点（数据是否等价）；GUI 排版语义不变。像素级黄金图回归（建议 A2）待 P4 资源 HAL 对齐后补齐。

### 7.9 P2 部分落地：单日缓存与诚实降级 (Implementation Log, cont.)

> 本轮在 P1 之上补齐两项**低风险高价值**改动；**GUI.c 物理拆分（建议 A1）暂缓**——目前仅有数据层等价测试、无渲染回归，拆分属大范围机械重构且短期收益偏可维护性，风险/收益不划算，待像素级黄金图回归（A2）就位后再做。

#### 已完成改动
1. **单日记录缓存（优化1）**：`Lunar.c` 新增按 (year,month,day) 的单槽缓存 `s_rec_cache_*`。LCDB 只读，按日期缓存永久有效、无需失效。月历渲染循环中同一天被 `LUNAR_SolarToLunar`/`GetFestival`/调休连续查询，命中后**每日读盘由 3 次降为 1 次**（月刷新 93→31 次读）。
2. **诚实降级（建议 C 方案 A 的最小落地）**：
   - `Lunar.h`/`Lunar.c` 新增 `bool lunar_db_year_supported(uint16_t year)`：数据库缺失或年份超出覆盖范围时返回 `false`。
   - `GUI.c` 的 `UI_COMP_TIME_SYNC_TIP` 触发条件扩展为「未校时(默认 2025-01) **或** 当前年份农历数据缺失/越界」。复用既有提示框（无新增易碎绘制代码），因两种情况的修复手段都是联网同步/更新。当外置 Flash 无 Type6（旧固件升级但未重烧资源）或 RTC 走到 2028+ 时，用户会看到更新提示而非"看似正确、实则错误"的农历。

#### 验证
- 数据层等价回归：2025–2027 全年 **1095 天 0 不一致**（缓存不影响正确性）。
- 固件 `-Werror` 编译通过，`text=55676B`（较 P1 +192B），`bss` +20B，仍在 56KB 预算内。

#### 暂缓项与后续
- **建议 A1（拆分 `GUI.c` 为 `lunar_db.c`/`ui_runtime.c`/`ui_render.c`）暂缓**：依赖渲染回归护栏，置于 A2 黄金图之后。
- ~~后续可做：`s_last_rendered_day` 跨天判定链~~ → **已落地，见 7.13**（实际接在 `ble_epd_on_timer`，配合单日缓存实现严格 1 读/天）。
- 后续可做：脱网久置的降级角标细化文案（区分"请校时" vs "农历待更新"）。

### 7.10 缺陷修复：资源热更新后农历库缓存未失效 (Bug Fix)

**现象**：真机"整刷固件 → 开机 → 网页更新资源"后，日历农历数据不显示，必须再手动复位一次才出现。

**根因**：`lunar_db_init` 幂等且**缓存失败结果**（`s_db_inited=true, s_db_valid=false`）。开机时外置 Flash 尚无 Type6（资源为开机后才下发），首次查询触发的 init 失败被永久缓存，之后即使 `RES_COMMIT` 写入了新数据库也不再重试，直到重启。GUI 侧的 UI/月历缓存虽已由 `GUI_ReloadUICache` 在提交后失效，但农历库这层的初始化缓存没有同步失效。

**修复**：
- `GUI/Lunar.c`/`Lunar.h` 新增 `void lunar_db_reset(void)`：清 `s_db_inited/s_db_valid` 与单日缓存 `s_rec_cache_valid`，下次查询自动重新 `init`。
- `EPD/EPD_service.c`：`EPD_CMD_RES_COMMIT` 提交成功后调用 `lunar_db_reset()`（与既有 `GUI_ReloadUICache` 并列）；`EPD_CMD_RES_ERASE` 成功后同样调用，避免读到已失效数据。

**效果**：网页"更新资源"提交后**无需重启设备即时生效**。固件 `-Werror` 编译通过（`text=55724B`），等价回归 1095 天 0 不一致。

### 7.11 缺陷修复：资源上传被周期刷新打断 & 擦除后红屏 (Bug Fix)

真机验证发现两个与"资源写入 vs 屏幕刷新共享 SPI"相关的问题，本轮一并小修（均为局部、低风险改动）。

#### 问题 #1：上传被周期刷新打断导致失败
**现象**：网页更新资源过程中偶发失败。
**根因**：时钟定时器 `ble_epd_on_timer` 到点(每分钟/整天)会调度一次屏幕刷新；刷新期间 `is_refreshing=true` 会拒绝随后到达的资源写入命令（返回 `NRF_ERROR_BUSY`），造成上传中断。原有机制只能"刷新时拒资源"，缺少"上传时停刷新"的反向保护。
**修复**：引入**资源会话**（`EPD_service.h` 新增 `res_session_active` / `res_session_deadline`）。
- `epd_service_on_write` 收到 `RES_ERASE/WRITE/COMMIT` 时置位会话并把空闲截止时间续期为 `now + EPD_RES_SESSION_TIMEOUT_S`(15s)。
- `ble_epd_on_timer` 在会话激活且未超时期间**跳过周期刷新**（`force_update` 除外）；空闲超过 15s（视为上传中断）自动结束会话恢复刷新，避免永久冻屏。
- `RES_COMMIT` 执行完毕(无论成败)清除会话，随后立即恢复刷新以呈现新资源。

#### 问题 #3（初步小修）：强制清空全部后红屏乱码
**现象**："强制清空全部"擦除外置 Flash（字库/UI/农历）后出现红屏与异常全刷。
**根因**：擦除只清 Flash，未失效 RAM 中的 UI/月历渲染缓存；后续渲染仍按旧缓存去读已擦除区域(全 `0xFF`)，得到乱码填充成红屏。
**修复**：`EPD_CMD_RES_ERASE` 成功后调用 `GUI_ReloadUICache`（与 `RES_COMMIT` 一致）。全量擦除时重建必然失败，从而把 UI/月历缓存标记为无效，后续渲染回退到**干净白屏**而非乱码；配合会话暂停刷新，擦除后约 15s 超时或下次刷新时呈现白屏。
> 无资源时的"兜底提示屏"（明确提示需联网下发资源）属体验增强，随 P3 诚实降级统一实现，本轮不做。

#### 说明
- 问题 #2（连接时 BLE 日志刷屏）为网页握手期正常诊断输出，非缺陷，暂不处理，后续可加日志开关/降噪。
- 固件 `-Werror` 编译通过（`text=55868B`，`bss=12068B`，仍在预算内）。

### 7.12 缺陷修复：外置 Flash 清空后红屏（无资源兜底加固） (Bug Fix)

**现象**：网页"强制清空整颗外置 Flash"(`resource_manager.js` 的 `eraseRange(0, flashTotal)`，把 Flash 全擦为 `0xFF`)后，即使重新烧录固件，屏幕仍显示红屏乱码/异常全刷。

**分析**：
- 代码本已有兜底：`epd_gui_update` 在 `manifest` 无效(空 Flash 全 `0xFF`→magic/CRC 校验失败)时会走 `DrawResourceMissingScreen`(内置位图字体，自包含)。但存在两处缺口：
  1. **红残影未清除**：兜底屏只做一次"以白为主"的全刷。三色墨水屏在旧固件读到已擦除数据后可能刷出大面积**红色残影**，单次白刷未必能驱除，需先用驱动的 `clear` 波形做一次去残影清屏。
  2. **触发条件不足**：兜底只覆盖"manifest 整体无效"。若 Flash 处于部分状态(`manifest` 侥幸有效但某个 UI mode/字体缺失)，仍会进入 `DrawGUI` 读到 `0xFF` 渲染成红色乱码。

**修复**（`EPD/EPD_service.c`，均为局部低风险改动）：
- 新增 `epd_draw_resource_missing_screen()`：**时序与正常全刷路径完全一致**——先 `epd->drv->init(epd)`(硬件复位 + PSR/CDI)，再 `write_image` 写入提示屏，最后 `refresh` 显示；原 `manifest` 无效分支改为调用它。
- 扩展兜底触发：`manifest` 有效但当前模式(日历/时钟/时钟+日历)的 UI 组件加载不出来(`!GUI_ModeExists`)时，同样走提示屏，绝不进 `DrawGUI` 渲染乱码。低电量页(内置绘制)与图片模式(显示用户图像、不依赖 UI 模板)不受此判断影响。

**关键教训（真机多轮迭代）**——下表按真机现象逐步试错。⚠️注意：**"原因"列 A~G 全是当时的误判**（真正根因见后文 H），保留于此以复盘"被表象带偏"的过程：

| 尝试 | 兜底时序 | 真机现象 | 当时（错误）的归因 |
|---|---|---|---|
| A | `write_image` + `refresh`（无 init/clear） | 红屏（旧图残留） | ~~未驱动足够像素/旧红图未清~~ |
| B | `clear(true)` + `write_image` + `refresh` | 白屏、无文字 | ~~`write_image` 未生效~~ |
| C | `drv->init` + `write_image` + `refresh` | 红黑白**噪点乱码** | ~~`EPD_Reset` 打乱 PTIN/PTL 寻址~~ |
| D | `clear(false)` + `write_image` + `refresh`（无 init） | **只白底、无提示** | ~~`write_image` 没写进 RAM~~ |
| E | 走全屏局刷 | **屏幕无任何反应** | ~~局刷差分无种子~~ |
| F | 复用全刷流程，`DrawGUI`→`DrawResourceMissingScreen`（仍用 `write_image`） | 黑白红**麻点** | ~~`write_image` PTIN/PTL 寻址不可靠~~ |
| G | **顺序写整帧**：`EPD_WriteCmd(DTM1)`+流式写黑平面+`EPD_FillRAM(DTM2,0xFF)`+`refresh` | **黑白**麻点（红消失） | ~~与 `UC8179_Clear` 同构、绕开 PTIN/PTL~~ |

| H（真正根因） | 修掉 `DrawResourceMissingScreen` 的 `ph` 钳位 bug | 预期：白底 + 黑字提示 | 见下 |

**⚠️ 真正的根因（H）——`DrawResourceMissingScreen` 的分页缓冲区被算坏，与刷新时序、写入方式全都无关！**

A~G 的所有"时序/写入方式"分析都是**被表象误导的弯路**。真正的 bug 只有一行：

- Linux 构建 `Makefile.nRF52_linux` 里 `nrf52811_xxaa: -D__HEAP_SIZE=0`（固件不用堆，GFX 用静态缓冲 `s_gfx_buffer`）。
- GUI 里页高 `ph = (__HEAP_SIZE - 512) / (width/8) = (0-512)/100 = -5`（负数）。
- `DrawGUI` 直接把 `ph=-5` 传给 `GFX_begin`，其 `if (buffer_height <= 0 || > max) buffer_height = max_height`(=76) **兜底**，`page_height=76`、三色减半=38 → **正常**。
- 但 `DrawResourceMissingScreen` 多写了一句 `if (ph <= 0) ph = 1;`，把 `ph` 钳成 **1**（正数，绕过了 `GFX_begin` 的兜底）→ `page_height=1`，三色 `page_height/=2` → **0**。
- `GFX_begin_3c` 里 `color = buffer + wb*0 = buffer`，于是 `gfx->color == gfx->buffer` 成立——**GFX 把三色屏误判为四色屏**，`GFX_fillScreen`/`GFX_drawPixel` 全按 4C 位格式处理，缓冲区内容与字节数彻底错乱；`total_pages = HEIGHT / 0` 还触发整数除零。→ **黑白(红)麻点**。

这一个 bug 自洽解释了**全部现象**：

| 版本 | 兜底做法 | 现象 | 用该 bug 的解释 |
|---|---|---|---|
| A/C/F | 直接 `write_image` 画坏缓冲区 | 红/噪点/麻点 | 4C 误判 → DTM1/DTM2 都是乱数据 |
| B/D | 先 `clear` 填白整帧，再画坏缓冲区 | 白屏无提示 | 整帧已白；坏分页几乎没写进东西 |
| G | 顺序写：DTM2 我单独填 0xFF、DTM1 流式写坏缓冲区 | **黑白**麻点（红没了）| DTM2 干净→无红；DTM1 仍是坏缓冲区→黑白麻点 |

即：**`write_image` 一直是好的**；`clear`/`write_image`/局刷之争全程是伪命题。元凶是 `DrawResourceMissingScreen` 与 `DrawGUI` 对 `ph` 处理不一致。

**最终修复（H）**：
- `GUI/GUI.c`：删除 `DrawResourceMissingScreen` 里的 `if (ph <= 0) ph = 1;`，与 `DrawGUI` 完全一致——把负 `ph` 交给 `GFX_begin` 的 `<=0 → max_height` 兜底。这是本问题的**唯一必需修复**。
- `EPD/EPD_service.c`（G 保留，非必需但更稳健）：兜底提示屏用与 `UC8179_Clear` 同构的顺序整帧写（`EPD_WriteCmd(DTM1)` + 逐页流式写黑平面 + `EPD_FillRAM(DTM2,0xFF)` + `refresh`），彻底不碰 PTIN/PTL；`resources_missing` 分支直接调用并 `goto finish_refresh`；保留"画过即跳过"守卫（`resource_hint_drawn`）。
- `fallback_paint` 标题色黑色（提示屏黑字白底）。

**教训**：跨函数复制同一段"魔法计算"（`ph = (__HEAP_SIZE-512)/(width/8)`）却各自加了不同的边界处理，是本次绕大弯的根源。负值/0 值这类边界，应在**同一处**（`GFX_begin`）统一兜底，调用方不要各自再加钳位。

**效果（待真机确认）**：外置 Flash 为空/损坏时正常显示"请更新资源"提示屏（白底 + 边框 + 黑字）。固件 `-Werror` 编译通过（`text=56092B`，`bss=12068B`，仍在预算内）。

### 7.13 落地收尾：诚实降级正式化 + 跨天判定链 (Implementation Log)

> 兜底提示屏（7.12/H）已真机验证通过，本节将其正式归入 **P3 诚实降级**，并补上此前列为"后续可做"的 **`s_last_rendered_day` 跨天判定链**（见 7.9 暂缓项）。均为小范围、低风险改动。

#### 1. 诚实降级兜底提示屏（正式归入 P3）

7.11 曾记"无资源兜底提示屏……随 P3 诚实降级统一实现，本轮不做"。经 7.12 多轮真机迭代（A~H），现已落地并验证：

- 触发：`epd_gui_update` 中 `resources_missing =` 唤醒失败 / `manifest` 无效 / 当前模式 `!GUI_ModeExists`。
- 渲染：`epd_draw_resource_missing_screen()` 用与 `UC8179_Clear` 同构的**顺序整帧写**（`EPD_WriteCmd(DTM1)` + 逐页流式写黑平面 + `EPD_FillRAM(DTM2,0xFF)` + `refresh`），彻底不碰 PTIN/PTL 局部窗口寻址。
- **真正根因（H）**：`DrawResourceMissingScreen` 的 `if (ph <= 0) ph = 1;` 在 `__HEAP_SIZE=0`(Linux 构建) 下把页高钳成 1、三色减半为 0，导致 GFX 把三色屏误判为四色屏 + 除零 → 麻点。删除该钳位、与 `DrawGUI` 一致交由 `GFX_begin` 兜底后彻底解决。
- 至此 P3"诚实降级"的**核心用户可见部分**（资源缺失/年份越界→明确更新提示，而非乱码或"看似正确实则错误"）已闭环。Schema 握手 / 屏幕规格校验 / 逻辑色通道 / Glyph Fallback 仍属 P3 后续项。

#### 2. `s_last_rendered_day` 跨天判定链

- 位置：`ble_epd_on_timer`（刷新调度中枢；`main.c` 的时钟回调仅转发至此，故实际接在此处）。
- 逻辑：`current_day = timestamp / 86400`；本地日期变化且模式为**日历 / 时钟+日历**时，置 `force_full_refresh` 并调度一次全刷。放在低电量 / 资源会话早退之后，跨天若逢暂停期，恢复后下一 tick 补触发。
- 价值：
  - **正确性**：时钟+日历模式平时只对时间做差分局刷，日期/星期/农历/节气/节日不随分钟更新；跨天全刷确保午夜后立即刷新，不必等下一次整点全刷。
  - **I/O**：跨天后 `lunar_db` 单日缓存(按年月日)自动失效，新一天记录重读一次即缓存，配合单日缓存实现"今日记录严格 1 读/天"。

#### 验证
- 数据层等价回归：2025–2027 全年 **1095 天 0 不一致**。
- 兜底提示屏：空 Flash 上电真机显示"请更新资源"提示屏（白底 + 边框 + 黑字），正常。
- 固件 `-Werror` 编译通过，`text=56156B`，`bss=12076B`，仍在预算内。

#### 仍暂缓
- 建议 A1（拆分 `GUI.c`）、A2（黄金图回归）、P3 其余（Schema 握手 / 屏幕规格校验 / 逻辑色通道 / Glyph Fallback）、P4（资源 HAL / 模拟器对齐 / 异步刷新省电）按路线图后续推进。
