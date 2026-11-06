# nRF_EPD 显示与硬件解耦完整设计方案

版本：v1.0  
目标硬件：nRF52811 + UC8179 + 7.5 寸 800x480 黑白红三色墨水屏  
核心目标：降低 MCU Flash/RAM/FDS 压力，保留 BLE DFU 能力，将 GUI 生成能力迁移到网页端，MCU 仅作为可靠的低功耗显示执行终端。

---

## 1. 设计背景

当前 `nRF_EPD` 项目已经包含 MCU 固件、EPD 驱动、GUI 目录、Web Bluetooth 控制台、独立 DFU 页面、Keil 应用工程、Secure DFU Bootloader 工程、SDK、OTA 工具和模拟器。现有工程已经具备图片模式、日历模式、时钟模式、图像分块传输、CRC16 校验、断点续传、Buttonless DFU、电池检测、低电量页等能力。

当前主线硬件以 `nRF52811 + UC8179 + 7.5 寸 800x480 黑白红三色屏` 为目标。nRF52811 的资源非常有限，只有 192 KB Flash / 24 KB RAM，还需要同时保留 SoftDevice、BLE、DFU、FDS、EPD 驱动和应用逻辑。因此 MCU 端不能继续承担完整 GUI、中文字库、节假日规则、农历算法、复杂布局和大图片缓存。

本方案将系统重新定义为：

```text
Web 端负责生成显示内容
MCU 端负责可靠接收、校验、保存小数据、解释固定模式、驱动 EPD 显示
```

---

## 2. 总体设计原则

### 2.1 一句话原则

```text
完整 UI 不保存，模式切换由网页端实时下发；
MCU 只保存离线运行所需的小数据、状态和必要中文词 bitmap；
正常休眠唤醒时按固定区域离线更新；
异常重启、数据过期或状态不可信时显示 Need Sync。
```

### 2.2 关键约束

1. DFU 永远优先，不能为了 GUI 功能牺牲 OTA 升级能力。
2. MCU 不保存完整 UI 背景、不保存完整 800x480 framebuffer。
3. MCU 不内置完整中文字库、不内置完整节假日数据库、不内置农历算法。
4. 模式 UI 背景固定，变化区域坐标固定。
5. 每次 MCU 只运行一个模式。
6. FDS 只保存小数据和状态，不保存大画面。
7. 日历模式可以一次同步一年数据。
8. 中文词不建议保存 UTF-8 后再由 MCU 渲染，推荐保存网页端渲染好的 text bitmap cache。
9. 异常复位、上电重启、WDT 复位后不再假设屏幕内容可信，直接显示 Need Sync。
10. 正常 RTC/低功耗唤醒后，才允许 MCU 基于固定背景做局部更新。

---

## 3. 总体架构

```text
┌─────────────────────────────────────────────┐
│                 Web / Mobile                 │
│                                             │
│  GUI布局设计                                 │
│  Canvas/SVG渲染                              │
│  日历/节假日/农历/节气生成                    │
│  中文词渲染成 bitmap                         │
│  黑白红 plane 分离                           │
│  DISPLAY_STREAM 打包                         │
│  DATA_OBJECT 打包                            │
│  BLE 分包传输                                │
│  DFU zip 升级                                │
└───────────────────┬─────────────────────────┘
                    │ BLE GATT
                    │
┌───────────────────▼─────────────────────────┐
│                nRF52811 MCU                 │
│                                             │
│  BLE Service / Status Notify                 │
│  DISPLAY_STREAM 接收并直接写 EPD RAM          │
│  DATA_OBJECT 校验并保存 FDS                   │
│  mode_state / calendar_data / glyph cache    │
│  固定模式区域表                               │
│  离线局部更新                                 │
│  Need Sync / Low Battery 兜底页               │
│  Buttonless DFU                              │
└───────────────────┬─────────────────────────┘
                    │ SPI / GPIO
                    │
┌───────────────────▼─────────────────────────┐
│                 UC8179 EPD                  │
│              800x480 B/W/R                  │
└─────────────────────────────────────────────┘
```

---

## 4. 数据分类

### 4.1 不保存到 FDS 的数据

这些数据由网页端在线下发，MCU 接收后直接显示，不长期保存：

```text
完整 UI 背景
完整 800x480 bitmap
模式切换时的完整画面
复杂字体渲染结果的大画面
图片模式原图
网页端预览配置
```

原因：

```text
1. 体积大，会占用 FDS 和 Flash。
2. 更新频率低，不需要 MCU 长期持有。
3. 墨水屏本身断电保持显示。
4. 异常重启后统一 Need Sync，不依赖旧画面恢复。
```

### 4.2 保存到 FDS 的数据

这些数据体积小、离线运行需要、断电后仍有价值：

```text
mode_state
current_mode
time_cache
calendar_year_data
holiday/workday flags
solar_term id
holiday text id
text bitmap cache
fixed region config，可选
offline_policy
last_sync_time
valid_until
need_sync flag
battery/config 状态
```

### 4.3 MCU 固化的数据

这些数据非常小、稳定，建议直接固化在 MCU：

```text
Need Sync 兜底页
Low Battery 页面
数字字体 0~9
英文小字体
常用符号：: - / . %
模式固定区域坐标，第一版推荐固化
简单电池图标
状态图标
```

---

## 5. 网页端模块设计

网页端是 GUI 引擎、数据生成器、设备管理器。

### 5.1 Web UI 设计模块

功能：

1. 提供日历模式、时钟模式、图片模式、看板模式等入口。
2. 提供固定 UI 模板设计。
3. 管理模式 ID、UI 版本、UI CRC。
4. 根据目标设备分辨率生成 800x480 画布。
5. 维护每个模式的动态区域定义。
6. 支持预览最终墨水屏效果。

输出：

```text
mode_id
ui_id
ui_version
ui_crc32
fixed background canvas
dynamic region metadata
```

### 5.2 Canvas 渲染模块

功能：

1. 将 UI 模板渲染到 Canvas。
2. 将中文、节日、节气、图标绘制到画布。
3. 处理旋转、镜像、裁剪、缩放。
4. 处理黑白红三色映射。
5. 将图像分离为 black plane 和 red plane。
6. 生成 raw_1bpp 数据。
7. 后续可增加 RLE_1BPP。

输出：

```text
black_plane_blocks
red_plane_blocks
display_stream_header
frame_crc32
```

### 5.3 日历数据生成模块

功能：

1. 生成指定年份 365/366 天的基础数据。
2. 生成星期、月份、日期。
3. 生成农历数据。
4. 生成节气数据。
5. 生成节假日/调休/工作日 flags。
6. 生成 holiday_text_id、solar_term_text_id、lunar_text_id。
7. 生成 valid_from / valid_until。
8. 生成 data_version 和 payload_crc32。

输出：

```text
EPD_OBJ_CALENDAR_YEAR_DATA
```

### 5.4 中文词 bitmap cache 生成模块

功能：

1. 收集本年度需要离线显示的中文词。
2. 包括节日、节气、休、班、假、调休等。
3. 使用网页端字体渲染成小 bitmap。
4. 生成 text_id 到 bitmap 的索引。
5. 支持 raw_1bpp，后续支持 rle_1bpp。
6. 生成 glyph_cache_version 和 CRC。

注意：第一版建议保存“词 bitmap”，不是“单字 glyph 组合”。这样 MCU 不需要中文字库和排版逻辑。

输出：

```text
EPD_OBJ_TEXT_BITMAP_CACHE
```

### 5.5 BLE 传输模块

功能：

1. 扫描并连接 `NRF_EPD` 设备。
2. 读取设备信息 GET_INFO。
3. 读取当前 mode_state。
4. 根据 need_sync / data_version / glyph_cache_version 决定同步内容。
5. 发送 DISPLAY_STREAM。
6. 发送 DATA_OBJECT。
7. 发送 SET_MODE。
8. 接收 MCU 状态通知。
9. 处理 chunk 重传。
10. 处理错误码和用户提示。

### 5.6 DFU 模块

功能：

1. 保持现有独立 DFU 页面。
2. 支持应用态进入 Buttonless DFU。
3. 支持选择 OTA zip。
4. 支持连接 DfuTarg。
5. 支持升级进度显示。
6. 升级后提示重新连接并同步数据。

---

## 6. MCU 端模块设计

MCU 是显示执行器、状态机和离线小数据运行器。

### 6.1 main/system 模块

功能：

1. 初始化时钟、GPIO、SPI、BLE、FDS、EPD。
2. 读取 reset_reason。
3. 判断是否允许离线恢复。
4. 运行主状态机。
5. 管理低功耗进入和唤醒。
6. 喂狗。
7. 调度 BLE 事件、FDS 事件、EPD 事件。

启动判断规则：

```text
上电复位 / WDT复位 / 异常复位：
  不信任屏幕内容
  显示 Need Sync
  设置 need_sync = 1

正常 RTC/低功耗唤醒：
  允许读取 mode_state
  如果数据有效，执行离线更新
  如果数据无效，显示 Need Sync
```

### 6.2 BLE Service 模块

功能：

1. 保留现有 EPD 自定义 Service。
2. 支持命令写入。
3. 支持数据分包写入。
4. 支持状态 Notify。
5. 支持 GET_INFO。
6. 支持 GET_MODE_STATE。
7. 支持 DISPLAY_STREAM。
8. 支持 DATA_OBJECT 保存。
9. 支持 SET_MODE。
10. 支持 ENTER_DFU。

### 6.3 Display Stream 模块

功能：

1. 接收网页端临时画面流。
2. 不保存到 FDS。
3. 校验 chunk CRC16。
4. 将通过校验的 block 直接写入 UC8179 RAM。
5. 在 END_DISPLAY_STREAM 后校验 frame/block 完整性。
6. 校验通过后刷新屏幕。
7. 校验失败时不刷新，返回错误。

关键规则：

```text
可以边接收边写 EPD RAM
但不能边接收边 refresh
只有 END 后整帧校验通过才 refresh
```

原因：EPD RAM 即使被写坏，只要没有 refresh，屏幕内容不会改变。

### 6.4 Persistent Data Object 模块

功能：

1. 接收需要保存的对象。
2. 校验 object header。
3. 校验 payload CRC32。
4. 根据 data_type 分发给不同存储模块。
5. 保存到 FDS。
6. 更新 mode_state 中的 data_version / glyph_cache_version。
7. 返回 SAVE_OK 或错误码。

支持对象：

```text
EPD_OBJ_MODE_STATE
EPD_OBJ_TIME_CACHE
EPD_OBJ_CALENDAR_YEAR_DATA
EPD_OBJ_TEXT_BITMAP_CACHE
EPD_OBJ_OFFLINE_POLICY
EPD_OBJ_DEVICE_CONFIG
```

### 6.5 FDS Storage 模块

功能：

1. 初始化 FDS。
2. 读写 mode_state。
3. 读写 time_cache。
4. 读写 calendar_year_data。
5. 读写 text_bitmap_cache。
6. 读写 offline_policy。
7. 管理 schema_version。
8. 执行 CRC 校验。
9. 执行 FDS GC。
10. 处理 FDS_FULL。

第一版建议：

```text
小对象直接 update record
大一点的 calendar/text bitmap 拆 chunk record
写入完成后再更新 meta
```

### 6.6 Mode Manager 模块

功能：

1. 管理 current_mode。
2. 管理 mode_state。
3. 判断是否 need_sync。
4. 判断是否允许 offline_update。
5. 判断当前模式需要哪些数据。
6. 提供每个模式的固定区域表。
7. 响应 SET_MODE。

### 6.7 Calendar Offline 模块

功能：

1. 读取 calendar_year_data。
2. 根据当前日期找到当天记录。
3. 读取 holiday_text_id / solar_term_text_id / flags。
4. 根据固定区域绘制日期、星期、节日、节气、休/班。
5. 局部刷新。
6. 如果当前日期超过 valid_until，设置 need_sync。

不做：

```text
不计算农历
不计算节气
不计算调休
不重新排版整个月历
不生成完整 UI
```

### 6.8 Text Bitmap Cache 模块

功能：

1. 根据 text_id 查找 bitmap item。
2. 校验 bitmap item CRC。
3. 将 bitmap 贴到指定 region。
4. 支持 raw_1bpp。
5. 后续支持 rle_1bpp。

不做：

```text
不解析字体文件
不渲染中文字体
不做复杂排版
```

### 6.9 EPD Driver 模块

功能：

1. UC8179 初始化。
2. 写 black plane。
3. 写 red plane。
4. 全刷。
5. 后续支持局刷。
6. Sleep。
7. Busy 等待。
8. 低电量时拒绝大刷新。

### 6.10 DFU 模块

功能：

1. 保留 Buttonless DFU。
2. 收到 ENTER_DFU 后停止 display stream。
3. 等待 FDS 写入完成。
4. 保存状态。
5. 断开 BLE。
6. 进入 Bootloader。
7. 升级完成后应用启动，显示 Need Sync 或等待 Web 重新同步。

---

## 7. BLE 协议设计

### 7.1 Characteristic 建议

```text
EPD_CMD_CHAR
  Web -> MCU
  发送命令

EPD_DATA_CHAR
  Web -> MCU
  发送大数据 chunk

EPD_STATUS_CHAR
  MCU -> Web notify
  返回状态、进度、错误码

EPD_INFO_CHAR
  Web read / notify
  读取固件版本、协议版本、能力集
```

### 7.2 命令定义

```c
#define EPD_CMD_GET_INFO              0x01
#define EPD_CMD_GET_MODE_STATE        0x02
#define EPD_CMD_GET_STATUS            0x03

#define EPD_CMD_BEGIN_DISPLAY_STREAM  0x10
#define EPD_CMD_WRITE_DISPLAY_CHUNK   0x11
#define EPD_CMD_END_DISPLAY_STREAM    0x12
#define EPD_CMD_ABORT_DISPLAY_STREAM  0x13
#define EPD_CMD_REFRESH               0x14

#define EPD_CMD_BEGIN_OBJECT          0x20
#define EPD_CMD_WRITE_OBJECT_CHUNK    0x21
#define EPD_CMD_END_OBJECT            0x22
#define EPD_CMD_ABORT_OBJECT          0x23

#define EPD_CMD_SET_MODE              0x30
#define EPD_CMD_RENDER_DYNAMIC        0x31
#define EPD_CMD_SET_TIME              0x32

#define EPD_CMD_ENTER_DFU             0xF0
#define EPD_CMD_REBOOT                0xF1
```

### 7.3 对象类型

```c
#define EPD_OBJ_DISPLAY_STREAM        0x01  // 不保存，直接显示

#define EPD_OBJ_MODE_STATE            0x10  // 保存 FDS
#define EPD_OBJ_TIME_CACHE            0x11  // 保存 FDS
#define EPD_OBJ_CALENDAR_YEAR_DATA    0x12  // 保存 FDS
#define EPD_OBJ_TEXT_BITMAP_CACHE     0x13  // 保存 FDS
#define EPD_OBJ_OFFLINE_POLICY        0x14  // 保存 FDS
#define EPD_OBJ_DEVICE_CONFIG         0x15  // 保存 FDS
```

### 7.4 设备信息结构

```c
typedef struct {
    uint16_t protocol_version;
    uint32_t fw_version;
    uint32_t feature_flags;

    uint16_t screen_w;
    uint16_t screen_h;
    uint8_t  color_mode;

    uint16_t max_chunk_size;
    uint32_t max_object_size;

    uint16_t battery_mv;
    uint8_t  current_mode;
    uint8_t  need_sync;

    uint32_t mode_ui_version;
    uint32_t data_version;
    uint32_t glyph_cache_version;
} epd_device_info_t;
```

### 7.5 Feature Flags

```c
#define EPD_FEATURE_DISPLAY_STREAM     (1u << 0)
#define EPD_FEATURE_RAW_1BPP           (1u << 1)
#define EPD_FEATURE_RLE_1BPP           (1u << 2)
#define EPD_FEATURE_RED_PLANE          (1u << 3)
#define EPD_FEATURE_FDS_CACHE          (1u << 4)
#define EPD_FEATURE_TEXT_BITMAP_CACHE  (1u << 5)
#define EPD_FEATURE_OFFLINE_CALENDAR   (1u << 6)
#define EPD_FEATURE_PARTIAL_REFRESH    (1u << 7)
#define EPD_FEATURE_DFU                (1u << 8)
```

---

## 8. Display Stream 格式

### 8.1 Header

```c
#define EPD_DISPLAY_MAGIC 0x53445045u // 'EPDS'

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;

    uint32_t stream_id;
    uint32_t ui_version;
    uint32_t ui_crc32;

    uint16_t screen_w;
    uint16_t screen_h;
    uint8_t  color_mode;     // 1=BW, 2=BWR
    uint8_t  stream_type;    // 0=full, 1=partial

    uint16_t block_count;
    uint32_t total_len;
    uint32_t frame_crc32;
} epd_display_stream_header_t;
#pragma pack(pop)
```

### 8.2 Block

```c
#pragma pack(push, 1)
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;

    uint8_t  plane;       // 0=black, 1=red
    uint8_t  format;      // 0=raw_1bpp, 1=rle_1bpp
    uint16_t stride;

    uint32_t data_len;
    uint32_t data_crc32;

    uint8_t  data[];
} epd_display_block_t;
#pragma pack(pop)
```

### 8.3 Chunk

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  cmd;
    uint32_t transfer_id;
    uint16_t seq;
    uint32_t offset;
    uint16_t len;
    uint16_t chunk_crc16;
    uint8_t  payload[];
} epd_chunk_t;
#pragma pack(pop)
```

### 8.4 完整性规则

```text
Chunk 级：CRC16
Block 级：data_crc32
Frame 级：frame_crc32
Header 级：magic/version/length/screen/color 检查
```

---

## 9. FDS 数据结构设计

### 9.1 FDS File ID

```c
#define FDS_FILE_SYSTEM          0x1000
#define FDS_FILE_MODE_STATE      0x1001
#define FDS_FILE_TIME_CACHE      0x1002
#define FDS_FILE_CALENDAR        0x1003
#define FDS_FILE_TEXT_BITMAP     0x1004
#define FDS_FILE_CONFIG          0x1005
```

### 9.2 Record Key

```c
#define REC_SCHEMA_VERSION       0x0001
#define REC_MODE_STATE           0x0002
#define REC_TIME_CACHE           0x0003
#define REC_OFFLINE_POLICY       0x0004

#define REC_CALENDAR_META        0x0100
#define REC_CALENDAR_DAY_BASE    0x0110
#define REC_CALENDAR_TEXT_POOL   0x0120

#define REC_TEXT_BITMAP_META     0x0200
#define REC_TEXT_BITMAP_INDEX    0x0201
#define REC_TEXT_BITMAP_CHUNK_BASE 0x0210
```

### 9.3 mode_state

```c
#define EPD_MODE_STATE_MAGIC 0x4D535045u // 'EPSM'

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t current_mode;

    uint32_t mode_ui_id;
    uint32_t mode_ui_version;
    uint32_t mode_ui_crc32;

    uint32_t data_version;
    uint32_t glyph_cache_version;

    uint32_t last_sync_time;
    uint32_t valid_until;

    uint8_t  offline_update_enabled;
    uint8_t  need_sync;
    uint8_t  last_shutdown_clean;
    uint8_t  reserved;

    uint32_t crc32;
} epd_mode_state_t;
#pragma pack(pop)
```

### 9.4 calendar_year_data

```c
#define EPD_CALENDAR_MAGIC 0x4C414345u // 'ECAL'

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t year;

    uint32_t data_version;
    uint32_t valid_from;
    uint32_t valid_until;

    uint16_t day_count;      // 365/366
    uint16_t text_pool_count;

    uint32_t payload_len;
    uint32_t payload_crc32;
    uint32_t crc32;
} epd_calendar_year_header_t;

#pragma pack(pop)
```

单日数据：

```c
#pragma pack(push, 1)
typedef struct {
    uint16_t yday;              // 1~366
    uint8_t  month;
    uint8_t  day;
    uint8_t  weekday;

    uint8_t  lunar_month;
    uint8_t  lunar_day;

    uint8_t  flags;             // holiday/workday/today/solar_term
    uint8_t  holiday_text_id;
    uint8_t  solar_term_text_id;
    uint8_t  lunar_text_id;
    uint8_t  reserved;
} epd_calendar_day_t;
#pragma pack(pop)
```

flags：

```c
#define CAL_FLAG_HOLIDAY       (1u << 0)
#define CAL_FLAG_WORKDAY       (1u << 1)
#define CAL_FLAG_REST_DAY      (1u << 2)
#define CAL_FLAG_SOLAR_TERM    (1u << 3)
#define CAL_FLAG_SPECIAL_DAY   (1u << 4)
```

### 9.5 text_bitmap_cache

```c
#define EPD_TEXT_BITMAP_MAGIC 0x54425845u // 'EXBT'

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t item_count;

    uint32_t glyph_cache_version;
    uint32_t payload_len;
    uint32_t payload_crc32;
    uint32_t crc32;
} epd_text_bitmap_cache_header_t;

typedef struct {
    uint16_t text_id;
    uint16_t width;
    uint16_t height;
    uint16_t stride;

    uint8_t  format;       // raw_1bpp / rle_1bpp
    uint8_t  plane;        // black / red

    uint32_t data_offset;
    uint32_t data_len;
    uint32_t data_crc32;
} epd_text_bitmap_item_t;
#pragma pack(pop)
```

---

## 10. 固定区域设计

第一版推荐把固定区域表固化在 MCU，以降低协议和 FDS 复杂度。

### 10.1 区域类型

```c
typedef enum {
    REGION_DATE_TEXT     = 1,
    REGION_WEEK_TEXT     = 2,
    REGION_TIME_TEXT     = 3,
    REGION_BATTERY       = 4,
    REGION_HOLIDAY       = 5,
    REGION_SOLAR_TERM    = 6,
    REGION_WORK_REST     = 7,
} epd_region_type_t;
```

### 10.2 区域结构

```c
typedef struct {
    uint8_t  region_type;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t  plane;
    uint8_t  clear_color;
    uint8_t  font_id;
    uint8_t  refresh_mode;
} epd_region_t;
```

### 10.3 日历模式区域示例

```c
static const epd_region_t calendar_regions[] = {
    { REGION_DATE_TEXT,  40,  20, 220, 60, 0, EPD_COLOR_WHITE, FONT_DIGIT_BIG,   EPD_REFRESH_PARTIAL },
    { REGION_WEEK_TEXT,  40,  90, 160, 40, 0, EPD_COLOR_WHITE, FONT_TEXT_SMALL,  EPD_REFRESH_PARTIAL },
    { REGION_TIME_TEXT, 620,  20, 140, 50, 0, EPD_COLOR_WHITE, FONT_DIGIT_BIG,   EPD_REFRESH_PARTIAL },
    { REGION_BATTERY,   720, 440,  60, 24, 0, EPD_COLOR_WHITE, FONT_ICON_SMALL,  EPD_REFRESH_PARTIAL },
    { REGION_HOLIDAY,    40, 160, 200, 40, 1, EPD_COLOR_WHITE, FONT_BITMAP_TEXT, EPD_REFRESH_PARTIAL },
    { REGION_SOLAR_TERM, 40, 210, 200, 40, 1, EPD_COLOR_WHITE, FONT_BITMAP_TEXT, EPD_REFRESH_PARTIAL },
};
```

---

## 11. 状态机设计

### 11.1 主状态

```text
BOOT
  ↓
INIT
  ↓
CHECK_RESET_REASON
  ├─ 异常/上电/WDT → NEED_SYNC
  └─ 正常唤醒 → CHECK_FDS

CHECK_FDS
  ├─ 数据有效 → IDLE / OFFLINE_UPDATE
  └─ 数据无效 → NEED_SYNC

IDLE
  ├─ BLE连接 → CONNECTED
  ├─ RTC到期 → OFFLINE_UPDATE
  └─ 低电量 → LOW_BATTERY

CONNECTED
  ├─ DISPLAY_STREAM → RECEIVING_DISPLAY
  ├─ DATA_OBJECT → RECEIVING_OBJECT
  ├─ SET_MODE → UPDATE_MODE
  └─ ENTER_DFU → DFU_PREPARE

OFFLINE_UPDATE
  ├─ 成功 → SLEEP
  └─ 失败 → NEED_SYNC

NEED_SYNC
  显示兜底页，广播 need_sync

LOW_BATTERY
  显示低电量页，限制刷新
```

### 11.2 DISPLAY_STREAM 状态

```text
IDLE
  ↓ BEGIN_DISPLAY_STREAM
DISPLAY_PREPARE
  ↓
RECEIVING_DISPLAY
  ↓ chunk crc ok
WRITE_EPD_RAM
  ↓ END_DISPLAY_STREAM
VERIFY_FRAME
  ├─ OK → REFRESH_EPD → DISPLAY_DONE
  └─ FAIL → ABORT_DISPLAY
```

### 11.3 DATA_OBJECT 状态

```text
IDLE
  ↓ BEGIN_OBJECT
OBJECT_PREPARE
  ↓
RECEIVING_OBJECT
  ↓
VERIFY_OBJECT
  ↓
SAVE_FDS
  ↓
UPDATE_META
  ↓
SAVE_DONE
```

---

## 12. 典型流程

### 12.1 首次同步日历模式

```text
Web 扫描设备
  ↓
连接 NRF_EPD
  ↓
GET_INFO
  ↓
GET_MODE_STATE
  ↓
发现 need_sync = 1
  ↓
Web 生成日历固定 UI
  ↓
Web 生成一年 calendar_year_data
  ↓
Web 生成 text_bitmap_cache
  ↓
Web 下发 DISPLAY_STREAM
  ↓
MCU 直接写 EPD RAM 并刷新
  ↓
Web 下发 CALENDAR_YEAR_DATA
  ↓
MCU 保存 FDS
  ↓
Web 下发 TEXT_BITMAP_CACHE
  ↓
MCU 保存 FDS
  ↓
Web 下发 SET_MODE CALENDAR
  ↓
MCU 保存 mode_state
  ↓
同步完成
```

### 12.2 正常离线跨天更新

```text
RTC 00:00 唤醒
  ↓
检查 reset_reason = 正常唤醒
  ↓
读取 mode_state
  ↓
current_mode = CALENDAR
  ↓
读取 calendar_year_data
  ↓
找到今天 day record
  ↓
清除固定区域
  ↓
绘制日期数字/星期
  ↓
根据 holiday_text_id 贴节日 bitmap
  ↓
根据 solar_term_text_id 贴节气 bitmap
  ↓
局部刷新
  ↓
保存 last_update_time
  ↓
进入 sleep
```

### 12.3 异常重启

```text
MCU 上电或 WDT 重启
  ↓
读取 reset_reason
  ↓
不信任屏幕背景
  ↓
显示 Need Sync
  ↓
mode_state.need_sync = 1
  ↓
BLE 广播/状态中提示 need_sync
```

### 12.4 只同步节假日数据

```text
Web 连接
  ↓
GET_MODE_STATE
  ↓
发现 data_version 旧
  ↓
只下发 CALENDAR_YEAR_DATA
  ↓
MCU 保存 FDS
  ↓
如果当前屏幕不需要变化，不刷新
  ↓
如果当前日期相关显示变化，执行 RENDER_DYNAMIC
```

### 12.5 DFU 升级

```text
Web 打开 dfu.html
  ↓
应用态连接设备
  ↓
ENTER_DFU
  ↓
MCU 停止接收 display/data
  ↓
等待 FDS 操作完成
  ↓
断开 BLE
  ↓
进入 Bootloader
  ↓
Web 连接 DfuTarg
  ↓
发送 OTA zip
  ↓
升级完成
  ↓
应用启动
  ↓
显示 Need Sync 或等待重新同步
```

---

## 13. 错误码设计

```c
#define EPD_STATUS_OK                    0x00
#define EPD_STATUS_BUSY                  0x01
#define EPD_STATUS_READY                 0x02
#define EPD_STATUS_RENDERING             0x03
#define EPD_STATUS_RENDER_DONE           0x04
#define EPD_STATUS_NEED_SYNC             0x05

#define EPD_ERR_CHUNK_CRC                0x10
#define EPD_ERR_BLOCK_CRC                0x11
#define EPD_ERR_FRAME_CRC                0x12
#define EPD_ERR_OBJECT_CRC               0x13
#define EPD_ERR_INVALID_MAGIC            0x14
#define EPD_ERR_UNSUPPORTED_VERSION      0x15
#define EPD_ERR_UNSUPPORTED_FORMAT       0x16
#define EPD_ERR_SIZE_TOO_LARGE           0x17
#define EPD_ERR_FDS_FULL                 0x18
#define EPD_ERR_LOW_BATTERY              0x19
#define EPD_ERR_INVALID_STATE            0x1A
#define EPD_ERR_NEED_SYNC                0x1B
```

状态通知：

```c
typedef struct {
    uint8_t  status;
    uint8_t  error;
    uint8_t  current_mode;
    uint8_t  need_sync;

    uint32_t transfer_id;
    uint16_t current_seq;
    uint16_t total_seq;

    uint32_t mode_ui_version;
    uint32_t data_version;
    uint32_t glyph_cache_version;

    uint16_t battery_mv;
} epd_status_notify_t;
```

---

## 14. 分阶段实现计划

## 阶段 0：基线确认与体积预算

目标：确认当前工程可编译、DFU 可用、主线硬件配置明确。

### MCU 侧任务

1. 确认 Keil 应用工程可编译。
2. 确认 DFU Bootloader 工程可编译。
3. 确认现有 Buttonless DFU 流程可用。
4. 记录当前 App Flash/RAM 占用。
5. 记录当前 SoftDevice、Bootloader、Application、FDS 的布局。
6. 确认当前默认屏幕为 UC8179 800x480 三色。

### Web 侧任务

1. 确认 `html/index.html` 可连接设备。
2. 确认现有图片传输可用。
3. 确认 `html/dfu.html` 可进入 DFU。

### 验收标准

```text
应用可编译
设备可广播
网页可连接
图片可显示
DFU 可升级
记录当前体积基线
```

---

## 阶段 1：Need Sync 兜底与状态模型

目标：先建立可靠启动策略，不再假设断电后屏幕内容可信。

### MCU 侧任务

1. 新增 `EPD_MODE_NEED_SYNC`。
2. 内置 Need Sync 兜底画面。
3. 新增 `epd_mode_state_t`。
4. 新增 FDS 保存/读取 mode_state。
5. 启动时读取 reset_reason。
6. 上电/WDT/异常复位显示 Need Sync。
7. 正常睡眠唤醒才允许离线更新。
8. BLE GET_INFO 返回 need_sync。

### Web 侧任务

1. GET_INFO 后显示设备是否 need_sync。
2. 如果 need_sync，提示用户执行完整同步。
3. 显示 current_mode、data_version、glyph_cache_version。

### 验收标准

```text
异常重启后设备显示 Need Sync
网页能读取 need_sync
mode_state 能写入和读取
不影响 DFU
```

---

## 阶段 2：DISPLAY_STREAM 直接显示，不保存 UI

目标：实现完整 UI 在线下发、MCU 直接显示、不写 FDS。

### MCU 侧任务

1. 新增 DISPLAY_STREAM 命令。
2. 支持 `BEGIN_DISPLAY_STREAM`。
3. 支持 `WRITE_DISPLAY_CHUNK`。
4. 支持 `END_DISPLAY_STREAM`。
5. 校验 chunk CRC16。
6. 校验 block CRC32。
7. 校验 frame CRC32。
8. 接收时直接写 EPD RAM。
9. END 校验通过后刷新。
10. 失败时不刷新。
11. 更新 mode_state 中的 mode_ui_version/ui_crc。

### Web 侧任务

1. Canvas 生成 800x480 画面。
2. 生成 black plane / red plane。
3. 编码 raw_1bpp。
4. 生成 display_stream_header。
5. 按 chunk 分包。
6. 计算 CRC16/CRC32。
7. 显示传输进度。
8. 支持失败重发当前 chunk。

### 验收标准

```text
网页下发完整 UI
MCU 不保存 UI 到 FDS
屏幕显示成功
CRC 错误时拒绝刷新
断开重连后能查询上次 UI 版本
DFU 不受影响
```

---

## 阶段 3：FDS 保存时间与模式数据

目标：建立小数据持久化能力。

### MCU 侧任务

1. 实现 `EPD_OBJ_TIME_CACHE`。
2. 实现 `EPD_OBJ_MODE_STATE`。
3. 保存 timezone、unix_time、synced_at、valid_until。
4. 保存 current_mode、last_sync_time、need_sync。
5. 提供 GET_MODE_STATE。
6. 提供 SET_MODE。

### Web 侧任务

1. 连接后同步时间。
2. 切换模式后发送 SET_MODE。
3. 根据 GET_MODE_STATE 判断是否需要重新同步。

### 验收标准

```text
时间数据可保存
模式状态可保存
断电后可以读取状态
异常重启仍显示 Need Sync
正常唤醒可读取模式
```

---

## 阶段 4：一年日历数据缓存

目标：日历模式离线运行的数据基础。

### MCU 侧任务

1. 实现 `EPD_OBJ_CALENDAR_YEAR_DATA`。
2. 实现 calendar_year_header 校验。
3. 实现 day table 保存。
4. 实现根据日期查找 day record。
5. 实现 valid_until 判断。
6. 数据过期时设置 need_sync。
7. 不实现农历/节气计算，只读取 Web 下发结果。

### Web 侧任务

1. 生成一年日历数据。
2. 生成 holiday/workday flags。
3. 生成 solar_term_text_id。
4. 生成 holiday_text_id。
5. 生成 data_version。
6. 下发到 MCU。

### 验收标准

```text
一年日历数据可保存
MCU可按日期查找到当天记录
跨 valid_until 后 need_sync
数据 CRC 错误时拒绝保存
```

---

## 阶段 5：中文词 Text Bitmap Cache

目标：让 MCU 不内置中文字库，也能离线显示少量中文结果。

### MCU 侧任务

1. 实现 `EPD_OBJ_TEXT_BITMAP_CACHE`。
2. 保存 text bitmap header。
3. 保存 text bitmap index。
4. 保存 bitmap data chunk。
5. 支持通过 text_id 查找 bitmap。
6. 支持 raw_1bpp 贴图。
7. 校验 bitmap data CRC。

### Web 侧任务

1. 收集本年度需要离线显示的中文词。
2. 将词渲染成 bitmap。
3. 生成 text_id 映射。
4. 生成 text bitmap cache。
5. 下发到 MCU。

### 第一版推荐词库

```text
元旦
春节
清明
劳动节
端午
中秋
国庆
除夕
小寒
大寒
立春
雨水
惊蛰
春分
谷雨
立夏
小满
芒种
夏至
小暑
大暑
立秋
处暑
白露
秋分
寒露
霜降
立冬
小雪
大雪
冬至
休
班
假
调休
```

### 验收标准

```text
MCU 不内置中文字库
Web 下发中文词 bitmap
MCU 可根据 text_id 显示中文词
CRC 错误时拒绝显示
```

---

## 阶段 6：日历模式离线局部更新

目标：正常休眠唤醒后，MCU 能按固定区域更新日历模式动态内容。

### MCU 侧任务

1. 固化 calendar_regions。
2. 实现日期数字绘制。
3. 实现星期绘制。
4. 实现节日 bitmap 贴图。
5. 实现节气 bitmap 贴图。
6. 实现休/班标记贴图。
7. 支持局部刷新。
8. 如果局刷不稳定，先使用小区域全刷或全屏刷新作为兜底。
9. 更新 last_update_time。

### Web 侧任务

1. 日历模式 UI 背景按固定区域设计。
2. 确保动态区域背景为可清除的纯色区域。
3. 首次同步时下发完整 UI。
4. 下发一年日历数据和 text bitmap cache。

### 验收标准

```text
正常 RTC 唤醒后更新日期区域
可以显示节日/节气/休班 bitmap
异常重启不离线更新，显示 Need Sync
数据过期显示 Need Sync
```

---

## 阶段 7：时钟模式离线更新

目标：让时钟模式可独立离线运行。

### MCU 侧任务

1. 固化 clock_regions。
2. 内置数字大字体。
3. 每分钟 RTC 唤醒。
4. 更新 HH:MM 区域。
5. 可选更新日期区域。
6. 低电量时降低刷新频率。

### Web 侧任务

1. 下发时钟模式固定 UI。
2. 同步 time_cache。
3. 设置 offline_policy：每分钟或每 5 分钟更新。
4. SET_MODE CLOCK。

### 验收标准

```text
时钟模式断开网页后仍能更新时间
不需要中文字库
低电量时停止或降低刷新
```

---

## 阶段 8：同步优化与版本管理

目标：避免每次连接都全量同步。

### MCU 侧任务

1. GET_MODE_STATE 返回 mode_ui_version。
2. 返回 data_version。
3. 返回 glyph_cache_version。
4. 返回 valid_until。
5. 返回 need_sync。
6. 支持只更新 calendar data。
7. 支持只更新 text bitmap cache。

### Web 侧任务

1. 比较本地 UI/data/glyph 版本。
2. need_sync=1 时全量同步。
3. data_version 旧时只同步日历数据。
4. glyph_cache_version 旧时只同步 bitmap cache。
5. UI 版本旧时下发 DISPLAY_STREAM。

### 验收标准

```text
无变化时不重复下发
只变节假日数据时不下发完整 UI
只变字体缓存时不下发完整 UI
need_sync 时完整同步
```

---

## 阶段 9：DFU 稳定性验证

目标：确保架构重构后 OTA 不被破坏。

### MCU 侧任务

1. ENTER_DFU 前停止 display stream。
2. 停止 FDS 新写入。
3. 等待 FDS 队列完成。
4. 保存 mode_state。
5. 进入 Bootloader。
6. 升级后默认 Need Sync 或根据 schema 判断是否保留数据。

### Web 侧任务

1. 保持独立 DFU 页面。
2. 升级前提示用户数据可能需要重新同步。
3. 升级后重新连接主页面。
4. 自动检测 need_sync。

### 验收标准

```text
Buttonless DFU 可进入
DfuTarg 可识别
OTA zip 可升级
升级后设备可启动
升级后可重新同步显示
```

---

## 阶段 10：可选增强

这些不是第一版必须功能。

### 可选功能

1. RLE_1BPP 压缩。
2. Text bitmap cache 压缩。
3. 多模式固定区域表由 Web 下发并保存 FDS。
4. 缺块 bitmap 重传。
5. 更细粒度局部刷新。
6. 多年份日历缓存。
7. glyph 级中文缓存，而不是词 bitmap。
8. 图片模式元数据管理。
9. BLE 传输速度优化。
10. 更完整的错误恢复策略。

---

## 15. 文件改造建议

### 15.1 MCU 侧新增/调整文件

```text
application/app_mode.c
application/app_mode.h
application/app_display_stream.c
application/app_display_stream.h
application/app_data_object.c
application/app_data_object.h
application/app_offline_calendar.c
application/app_offline_calendar.h
application/app_text_bitmap.c
application/app_text_bitmap.h
application/app_dfu.c
application/app_dfu.h

storage/epd_fds.c
storage/epd_fds.h
storage/epd_records.h

protocol/epd_protocol.h
protocol/epd_objects.h
protocol/epd_status.h

renderer/epd_region.c
renderer/epd_region.h
renderer/epd_font_digit.c
renderer/epd_font_digit.h
renderer/epd_need_sync_page.c
renderer/epd_need_sync_page.h
```

### 15.2 Web 侧新增/调整文件

```text
html/js/device-info.js
html/js/ble-transfer.js
html/js/display-stream.js
html/js/calendar-year-builder.js
html/js/text-bitmap-cache.js
html/js/epd-plane-encoder.js
html/js/mode-sync.js
html/js/crc.js
```

### 15.3 文档建议

```text
docs/display-hardware-decoupled-design.md
docs/ble-display-stream.md
docs/fds-data-layout.md
docs/calendar-offline-mode.md
docs/text-bitmap-cache.md
docs/dfu-flow.md
```

---

## 16. 验收总清单

最终方案完成后，需要满足：

```text
1. nRF52811 应用固件可编译。
2. BLE 广播和连接正常。
3. Web 可读取设备信息和 need_sync。
4. 异常重启显示 Need Sync。
5. DISPLAY_STREAM 可显示完整 UI。
6. 完整 UI 不写入 FDS。
7. mode_state 可保存和读取。
8. time_cache 可保存和读取。
9. calendar_year_data 可保存和读取。
10. text_bitmap_cache 可保存和读取。
11. 正常 RTC 唤醒可执行离线局部更新。
12. 日历模式可显示节日/节气/休班 bitmap。
13. 数据过期后 need_sync。
14. 低电量保护有效。
15. Buttonless DFU 可用。
16. OTA 升级后可重新同步。
17. App Flash/RAM 不明显恶化，最好较当前 GUI 方案下降。
```

---

## 17. 最终定案

本项目后续主线建议定为：

```text
在线 UI 流式显示 + FDS 离线小数据缓存 + 固定区域局部更新 + Need Sync 兜底 + Buttonless DFU 保留
```

具体含义：

```text
网页端：
  负责完整 GUI、日历、节假日、农历、节气、字体渲染、图片处理、数据打包和同步。

MCU 端：
  负责 BLE 接收、CRC 校验、直接显示临时 UI、保存小型离线数据、固定区域更新、低功耗和 DFU。

FDS：
  只保存 mode_state、time_cache、calendar_year_data、text_bitmap_cache、offline_policy 等小数据。

EPD：
  保持显示，不要求 MCU 断电后恢复旧 UI；异常重启统一显示 Need Sync。
```

这个方案能最大程度降低 nRF52811 的 Flash/FDS 压力，同时保留离线运行能力和 OTA 升级能力。
