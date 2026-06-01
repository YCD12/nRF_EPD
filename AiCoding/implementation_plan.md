# nRF_EPD 架构重构 - 显示与硬件解耦实现计划

## 目标

根据 [promote.txt](file:///D:/Desk/hema/me/New_Plane/promote.txt) 的约束和 [nRF_EPD_display_hardware_decoupled_design.md](file:///D:/Desk/hema/me/New_Plane/nRF_EPD_display_hardware_decoupled_design.md) 的方案设计，对 [nRF_EPD](file:///D:/Desk/hema/me/New_Plane/nRF_EPD) 项目进行架构重构。

核心思想：**Web 端负责"生成要显示什么"，MCU 端负责"可靠地把它显示出来"，DFU 永远优先于复杂 GUI。**

---

## User Review Required

> [!IMPORTANT]
> 这是一个**大型重构任务**，涉及 MCU 固件、BLE 协议、FDS 存储、Web 端多个模块的改造。建议按设计文档的**分阶段方案**逐步实施。
>
> **本次实施范围建议**：先做 **阶段 0 + 阶段 1 + 阶段 2 的 MCU 侧基础**，具体包括：
> 1. 新增协议头文件和数据结构定义
> 2. 新增 Need Sync 兜底页和状态模型
> 3. 新增 DISPLAY_STREAM 接收通路（MCU 端接收网页端 raw_1bpp 直接写 EPD RAM）
> 4. 对应的 Web 端基础传输支持
>
> 请确认：
> - 是否同意先实施上述阶段？还是需要一次实施更多阶段？
> - 现有旧协议命令（WRITE_IMAGE / WRITE_BLOCK 等）是否保留兼容？（推荐保留）
> - Need Sync 画面是否需要特定设计？（建议使用简单英文文本："NRF_EPD Need Sync" + 设备名）

## Open Questions

> [!IMPORTANT]
> 1. **Need Sync 兜底画面内容**：设计文档建议 MCU 内置一个极简的 Need Sync 画面。是否可以用纯代码生成的简单位图（黑底白字 "Need Sync"）？还是需要更精美的图标？
> 2. **旧 GUI 代码是否保留**：阶段 1-2 期间是否保留现有 `GUI/` 目录下的日历、时钟、字体代码不删除，仅新增 DISPLAY_STREAM 通路？（推荐先保留，阶段 3 再逐步迁移删除）
> 3. **Web 端 Canvas 传图 UI**：是否需要在现有 `html/index.html` 中新增 DISPLAY_STREAM 传输入口？还是单独创建新页面？

---

## 现有代码分析

### 当前架构

```text
nRF_EPD/
├── main.c              -- 系统初始化、主循环、BLE、WDT、电源管理
├── EPD/
│   ├── EPD_config.c/h  -- FDS 配置读写
│   ├── EPD_driver.c/h  -- GPIO/SPI/EPD 底层驱动
│   ├── EPD_service.c/h -- BLE Service、命令处理、GUI 调度
│   └── UC8179.c        -- UC8179 驱动实现
├── GUI/
│   ├── GUI.c/h         -- 日历/时钟/画布渲染 (需要迁移到 Web)
│   ├── Adafruit_GFX.*  -- 图形库
│   ├── Lunar.c/h       -- 农历计算 (需要迁移到 Web)
│   ├── fonts.c/h       -- 字体数据
│   └── u8g2_font.*     -- 中文字体库
├── html/
│   ├── index.html      -- Web Bluetooth 控制台
│   ├── dfu.html        -- 独立 DFU 页面
│   ├── js/             -- ble_transfer, main, paint, dfu 等
│   └── css/main.css
├── Keil/               -- 应用和 DFU Bootloader 工程
├── SDK/                -- Nordic SDK 17.1.0
└── tools/              -- OTA 打包工具
```

### 已有能力（保留）
- ✅ BLE Peripheral (`NRF_EPD_xxxx`)
- ✅ EPD 自定义 Service (CMD/DATA/NOTIFY)
- ✅ WRITE_BLOCK + CRC16 分块传输 + 断点续传
- ✅ Buttonless DFU
- ✅ WDT、低电量保护
- ✅ Need Sync 模式和 reset_reason 判断（已部分实现）
- ✅ GET_DEVICE_INFO + ENTER_DFU 命令（已实现）
- ✅ Feature flags 和 protocol version（已实现）

### 已需要新增
- 🔲 DISPLAY_STREAM 协议（BEGIN/WRITE_CHUNK/END/REFRESH）
- 🔲 Data Object 持久化协议（mode_state、calendar、text bitmap 等）
- 🔲 协议头文件（epd_protocol.h、epd_objects.h、epd_status.h）
- 🔲 FDS 存储模块（epd_fds.c/h）
- 🔲 Mode Manager 模块
- 🔲 Calendar Offline 模块
- 🔲 Text Bitmap Cache 模块
- 🔲 Web 端 DISPLAY_STREAM 编码和传输

---

## Proposed Changes

### 阶段 0：协议和数据结构定义

---

#### [NEW] [epd_protocol.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/protocol/epd_protocol.h)

定义设计文档第 7 节的所有 BLE 命令、对象类型、Feature Flags。保持与现有命令 ID 兼容，新命令使用新 ID 段。

```c
// 新增命令（与现有命令共存）
#define EPD_CMD_BEGIN_DISPLAY_STREAM  0x10
#define EPD_CMD_WRITE_DISPLAY_CHUNK   0x11
#define EPD_CMD_END_DISPLAY_STREAM    0x12
#define EPD_CMD_ABORT_DISPLAY_STREAM  0x13

#define EPD_CMD_BEGIN_OBJECT          0x20  // 注意：与旧 SET_TIME 0x20 冲突
// ... 需要重新规划 ID 避免冲突
```

> [!WARNING]
> 设计文档的新命令 ID（0x10-0x13, 0x20-0x23）与现有命令 ID 有冲突：
> - 现有 `EPD_CMD_SET_TIME = 0x20`，新 `EPD_CMD_BEGIN_OBJECT = 0x20` 冲突
> - 现有 `EPD_CMD_SET_WEEK_START = 0x21`，新 `EPD_CMD_WRITE_OBJECT_CHUNK = 0x21` 冲突
>
> **建议方案**：保留现有旧命令 ID 不变，将新命令放到 0x40-0x4F 段：
> ```c
> #define EPD_CMD_BEGIN_DISPLAY_STREAM  0x10  // 0x10-0x13 无冲突
> #define EPD_CMD_WRITE_DISPLAY_CHUNK   0x11
> #define EPD_CMD_END_DISPLAY_STREAM    0x12
> #define EPD_CMD_ABORT_DISPLAY_STREAM  0x13
> #define EPD_CMD_REFRESH_DISPLAY       0x14
> 
> #define EPD_CMD_BEGIN_OBJECT          0x40  // 避开 0x20-0x23
> #define EPD_CMD_WRITE_OBJECT_CHUNK    0x41
> #define EPD_CMD_END_OBJECT            0x42
> #define EPD_CMD_ABORT_OBJECT          0x43
> 
> #define EPD_CMD_SET_MODE_V2           0x44
> #define EPD_CMD_RENDER_DYNAMIC        0x45
> #define EPD_CMD_SET_TIME_V2           0x46
> ```

#### [NEW] [epd_objects.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/protocol/epd_objects.h)

定义 Display Stream Header、Block、Chunk、mode_state、calendar_year_data 等所有数据结构。

#### [NEW] [epd_status.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/protocol/epd_status.h)

定义状态码、错误码、状态通知结构。

---

### 阶段 1：Need Sync 兜底与状态模型

---

#### [NEW] [app_mode.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/application/app_mode.c)
#### [NEW] [app_mode.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/application/app_mode.h)

Mode Manager 模块：
- 管理 `epd_mode_state_t` 结构
- `need_sync` 状态管理
- mode 切换逻辑
- FDS 保存/读取 mode_state

#### [NEW] [epd_need_sync_page.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/renderer/epd_need_sync_page.c)
#### [NEW] [epd_need_sync_page.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/renderer/epd_need_sync_page.h)

内置 Need Sync 兜底画面：
- 极简位图，不依赖中文字库
- 显示 "NRF_EPD Need Sync" + 设备蓝牙名
- 使用内置小数字/英文字体
- 预估 Flash 占用 < 2 KB

#### [MODIFY] [EPD_service.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/EPD/EPD_service.h)

- ✅ `need_sync` 字段已存在
- ✅ `mode_ui_version / data_version / glyph_cache_version` 已存在
- ✅ `ble_epd_set_need_sync()` 已存在
- 新增 `epd_mode_state_t` 引用（或移到 app_mode.h）

#### [MODIFY] [EPD_service.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/EPD/EPD_service.c)

- ✅ `ble_epd_set_need_sync()` 已实现
- ✅ GET_DEVICE_INFO 已返回 need_sync
- 需要在 `epd_gui_update()` 中处理 `MODE_NEED_SYNC` 渲染 Need Sync 页面
- 需要在 GET_INFO 响应中增加 mode_ui_version、data_version、glyph_cache_version

#### [MODIFY] [main.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/main.c)

- ✅ reset_reason 判断已实现
- ✅ 异常复位 → Need Sync 已实现
- 无需额外修改

#### [MODIFY] [GUI.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/GUI/GUI.c)

- 在 `DrawGUI()` 中增加 `MODE_NEED_SYNC` case，调用 Need Sync 页面渲染

---

### 阶段 2：DISPLAY_STREAM 直接显示

---

#### [NEW] [app_display_stream.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/application/app_display_stream.c)
#### [NEW] [app_display_stream.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/application/app_display_stream.h)

Display Stream 接收模块：
- 接收 BEGIN_DISPLAY_STREAM 命令，解析 header
- 接收 WRITE_DISPLAY_CHUNK，校验 CRC16，直接写入 UC8179 RAM
- 接收 END_DISPLAY_STREAM，校验 frame CRC32
- 校验通过后刷新，失败时不刷新
- 不写 FDS
- RAM 占用：只需 chunk 缓冲 + header（~300 bytes）

状态机：
```text
IDLE → DISPLAY_PREPARE → RECEIVING_DISPLAY → VERIFY_FRAME → REFRESH_EPD → DONE
                                                        └→ ABORT_DISPLAY
```

#### [MODIFY] [EPD_service.c](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/EPD/EPD_service.c)

- 在 `epd_execute_command()` 中新增 DISPLAY_STREAM 命令处理分支
- 将 `EPD_CMD_BEGIN_DISPLAY_STREAM`、`WRITE_DISPLAY_CHUNK`、`END_DISPLAY_STREAM` 标记为 urgent 命令

#### [MODIFY] [EPD_service.h](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/EPD/EPD_service.h)

- 新增 display_stream 状态字段到 `ble_epd_t`

---

### 阶段 2（Web 端）：Display Stream 编码和传输

---

#### [NEW] [display-stream.js](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/html/js/display-stream.js)

Web 端 Display Stream 模块：
- Canvas → raw_1bpp 编码（black plane + red plane）
- 生成 display_stream_header
- CRC16/CRC32 计算
- 按 chunk 分包
- 传输进度回调

#### [NEW] [crc.js](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/html/js/crc.js)

CRC 工具模块：
- CRC16-CCITT（与 MCU 端一致）
- CRC32

#### [NEW] [epd-plane-encoder.js](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/html/js/epd-plane-encoder.js)

平面编码器：
- Canvas ImageData → 黑白红三色分离
- 生成 raw_1bpp black plane
- 生成 raw_1bpp red plane
- 后续可扩展 RLE

#### [MODIFY] [index.html](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/html/index.html)

- 新增 "Display Stream" 传图入口
- 引入新 JS 模块

#### [MODIFY] [main.js](file:///D:/Desk/hema/me/New_Plane/nRF_EPD/html/js/main.js)

- 集成 display-stream 传输功能
- 连接后读取 GET_DEVICE_INFO 并显示 need_sync 状态

---

### 文档

---

#### [NEW] docs/architecture.md

整体架构说明文档。

#### [NEW] docs/ble-display-stream.md

DISPLAY_STREAM 协议详细文档。

---

## 文件目录结构（新增/修改总览）

```text
nRF_EPD/
├── application/           [NEW DIR]
│   ├── app_mode.c/h       [NEW] Mode Manager
│   └── app_display_stream.c/h [NEW] Display Stream 接收
├── protocol/              [NEW DIR]
│   ├── epd_protocol.h     [NEW] 命令和协议定义
│   ├── epd_objects.h      [NEW] 数据结构定义
│   └── epd_status.h       [NEW] 状态码定义
├── renderer/              [NEW DIR]
│   └── epd_need_sync_page.c/h [NEW] Need Sync 兜底页
├── EPD/
│   ├── EPD_service.c      [MODIFY] 新增命令处理
│   └── EPD_service.h      [MODIFY] 新增字段
├── GUI/
│   └── GUI.c              [MODIFY] 新增 MODE_NEED_SYNC
├── html/
│   ├── index.html         [MODIFY] 新增传输入口
│   └── js/
│       ├── display-stream.js     [NEW]
│       ├── crc.js                [NEW]
│       └── epd-plane-encoder.js  [NEW]
├── docs/
│   ├── architecture.md    [NEW]
│   └── ble-display-stream.md [NEW]
└── main.c                 [保持不变]
```

---

## Verification Plan

### Automated Tests

1. **编译检查**：修改后确认 Keil 工程文件引用更新（需手工确认 .uvprojx 文件包含新增源文件路径）
2. **CRC 一致性**：Web 端 CRC16 函数与 MCU 端 `crc16_compute()` 输出一致性测试
3. **协议兼容性**：现有旧命令（WRITE_IMAGE、WRITE_BLOCK、SET_TIME 等）在新代码中仍能正常工作

### Manual Verification

1. **Need Sync 页面**：异常复位后设备显示 Need Sync 画面
2. **GET_DEVICE_INFO**：网页连接后能读取 need_sync 状态
3. **DISPLAY_STREAM**：网页 Canvas 生成 800×480 画面 → BLE 分包发送 → MCU 接收写入 EPD RAM → 刷新显示
4. **DFU 不受影响**：OTA zip 生成 → DFU 升级 → 升级后应用启动正常
5. **旧功能兼容**：现有图片传输、日历模式、时钟模式仍可正常工作

---

## Flash/RAM 预算估算

| 模块 | 预估 Flash | 预估 RAM | 说明 |
|------|-----------|---------|------|
| protocol/*.h | ~0.5 KB | 0 | 纯头文件定义 |
| app_display_stream | ~2 KB | ~400 B | chunk buffer + state |
| app_mode | ~1 KB | ~100 B | mode_state |
| epd_need_sync_page | ~1.5 KB | ~200 B | 极简位图 |
| epd_status | ~0.5 KB | 0 | 纯定义 |
| **总计** | **~5.5 KB** | **~700 B** | |

> [!NOTE]
> 后续阶段删除 GUI/ 中的日历、农历、大字体代码后，预估可释放 **30-50 KB Flash**，远超新增量。

---

## 约束检查

- ✅ 不修改 Nordic SDK 源码
- ✅ 不破坏现有 BLE 命令兼容性
- ✅ 不引入大型第三方库
- ✅ 不使用动态内存作为核心路径
- ✅ 不使用浮点 printf
- ✅ 保留 DFU 能力
- ✅ 不引入完整中文字库或 framebuffer
- ✅ 所有新增数据有 magic、version、CRC
