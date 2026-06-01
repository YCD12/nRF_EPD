# nRF_EPD 项目完整方案说明书

> 适用范围：nRF52811 + UC8179 + 800x480 黑白红三色墨水屏项目。  
> 目标：在保留 BLE、FDS、小状态存储、EPD 驱动、低功耗和 DFU 升级能力的前提下，将 GUI、字体、日历、节假日、图片合成等高层应用能力迁移到 Web / Mobile 端，使 nRF52811 固件成为低功耗 BLE 墨水屏显示终端。

---

## 1. 方案结论

当前工程已经实现了 EPD 显示、GUI 绘制、字体、农历/日历、BLE 服务、FDS、DFU 相关能力，但在 nRF52811 上继续沿用 MCU 端 hard-coded GUI 的方式风险较高。当前 App Flash 统计约 64,027 bytes，RAM 统计约 14,308 bytes，其中字体、GUI、农历、Adafruit_GFX、printf 相关模块合计约 32 KB，是当前固件体积的主要来源。

最终建议采用以下架构：

```text
Web / Mobile：完整应用层
  - 日历 / 时钟 / 节假日 / 农历 / 天气 / 图片处理
  - 旧 hard-coded UI 坐标模板化
  - 字体栅格化
  - 生成 800x480 black/red plane 或局部 bitmap block
  - BLE 分包下发
  - DFU 触发和升级入口

nRF52811 MCU：低功耗 BLE 显示终端
  - BLE 连接和 GATT 服务
  - 可靠传输协议
  - FDS 小状态存储
  - UC8179 EPD 驱动
  - 极简 Render Engine
  - Fallback Screen
  - Power / Battery / Watchdog / Diagnostics
  - Buttonless DFU 入口
```

最终核心原则：

```text
1. MCU 不再实现完整 GUI。
2. MCU 不再保存完整画面。
3. MCU 不再内置完整中文字体。
4. MCU 不再计算农历、节假日、月历布局。
5. 大 bitmap 默认流式写 EPD，不进 FDS。
6. FDS 只保存配置、状态、元数据和小型 recipe。
7. 断电/重启/DFU 后不恢复旧画面，统一显示带蓝牙名称的同步提示页。
8. 所有 Web 端新 UI 输出必须与旧 hard-coded UI 像素坐标一致。
9. DFU 可升级能力优先级高于 GUI 功能。
10. App Flash 目标控制在 36~40 KB，超过 48 KB 必须停止新增功能并裁剪。
```

---

## 2. 项目背景和约束

### 2.1 硬件平台

当前硬件目标：

```text
MCU：nRF52811
屏幕驱动：UC8179
屏幕规格：800x480 黑白红三色墨水屏
通信：BLE
开发方向：Web Bluetooth / Mobile BLE 控制 + MCU 端显示执行
升级方式：Secure DFU / Buttonless DFU
```

nRF52811 属于低资源 BLE SoC，Flash 和 RAM 非常有限。项目必须假定 MCU 端资源紧张，任何新增功能都要经过 Flash / RAM / DFU 占用评估。

### 2.2 当前实现状态

从当前工程占用表看，项目已经包含以下能力或模块：

```text
EPD 显示相关：uc8179.o、epd_driver.o、epd_service.o
GUI 相关：gui.o、fonts.o、u8g2_font.o、adafruit_gfx.o
日历/农历相关：lunar.o
BLE 相关：ble_advdata.o、ble_advertising.o、ble_conn_params.o、nrf_ble_gatt.o、nrf_sdh.o、nrf_sdh_ble.o
DFU 相关：ble_dfu.o、ble_dfu_unbonded.o、nrf_dfu_svci.o
FDS/Flash Storage：fds.o、nrf_fstorage_sd.o、nrf_fstorage.o
系统外设：nrfx_spi.o、nrfx_gpiote.o、app_timer2.o、nrf_pwr_mgmt.o、drv_rtc.o、nrfx_wdt.o
```

当前不是从 0 开始，而是在已有 BLE + EPD + GUI + DFU 雏形的基础上，重新划分职责并裁剪 MCU 端功能。

---

## 3. 当前工程资源占用分析

### 3.1 总体占用

根据当前上传的分析表：

```text
当前 App Flash 汇总：约 64,027 bytes
当前 App RAM 汇总：约 14,308 bytes
```

这里的 Flash 是对象文件统计口径，不等同于芯片全部 Flash 可用空间。实际设备 Flash 还要分给：

```text
MBR
SoftDevice S112
Application
FDS / App Data
Bootloader
Bootloader Settings Page
MBR Params Page，可选
DFU 临时空间 / Bank
```

所以 64 KB App 对 nRF52811 来说已经偏大，后续继续添加完整协议、状态管理、传输校验、fallback、低功耗和诊断模块时，必须先裁剪 GUI、字体和日历相关代码。

### 3.2 Flash 占用 Top 模块

| 模块 | Flash | RAM | 当前判断 |
|---|---:|---:|---|
| fonts.o | 9,821 B | 0 B | 第一优先级裁剪，字体迁移到 Web |
| gui.o | 9,014 B | 12 B | GUI 逻辑迁移到 Web 模板 |
| uc8179.o | 4,886 B | 96 B | 保留，但避免继续扩大 |
| adafruit_gfx.o | 3,772 B | 4 B | 建议移除，改极简绘制/流式写屏 |
| fds.o | 3,614 B | 284 B | 保留，用于小状态 |
| epd_service.o | 3,414 B | 8 B | 保留并重构为 BLE/协议入口 |
| mc_w.l | 3,076 B | 8 B | 字体/库相关，裁剪 |
| printfa.o | 2,340 B | 0 B | 建议关闭 printf/float printf |
| lunar.o | 2,110 B | 164 B | 农历/节假日迁移到 Web |
| epd_driver.o | 1,494 B | 48 B | 保留 |
| main.o | 1,394 B | 2,696 B | RAM 需重点检查，全局 buffer 要收敛 |

### 3.3 可裁剪空间估算

按模块分类估算：

```text
字体/字库：
  fonts.o       9,821 B
  u8g2_font.o     830 B
  mc_w.l        3,076 B
  mf_w.l        1,188 B
  合计：约 14,915 B

GUI / 日历农历：
  gui.o         9,014 B
  lunar.o       2,110 B
  合计：约 11,124 B

GFX 绘图库：
  adafruit_gfx.o 3,772 B

printf / 格式化：
  printfa.o     2,340 B

合计可裁剪：约 32,151 B
```

当前 App 约 64 KB，理论裁剪后：

```text
64 KB - 32 KB ≈ 32 KB
```

后续新增协议、传输、状态、fallback、diagnostics 等模块预计增加 4~8 KB，因此最终 App Flash 目标应控制在：

```text
目标：36~40 KB
警戒线：40~48 KB
危险线：>48 KB
```

### 3.4 RAM 占用问题

RAM Top：

```text
arm_startup_nrf52811.o：10,240 B，主要是 Stack
main.o：2,696 B，其中 ZI_data 2,644 B
fds.o：284 B
nrf_fstorage_sd.o：184 B
lunar.o：164 B
```

RAM 优化方向：

```text
1. 检查 main.c 是否存在大数组、图像 buffer、临时 buffer。
2. 禁止完整 framebuffer。
3. 大对象按 chunk / line / block 流式处理。
4. Stack 从 10 KB 是否可降到 6 KB，需要通过 stack watermark 实测。
5. 删除 lunar.o 后可释放部分 RAM。
```

---

## 4. 最终系统架构

### 4.1 架构分层

```text
Web / Mobile / Browser
  ├─ Mode Template Manager
  ├─ Legacy Pixel Compatibility Layer
  ├─ Calendar / Clock / Holiday Generator
  ├─ Font Rasterizer
  ├─ Bitmap Composer
  ├─ Layout / Recipe Compiler
  ├─ BLE Transfer Client
  ├─ Device Status Monitor
  └─ DFU Client

BLE GATT Layer
  ├─ EPD Control Characteristic
  ├─ EPD Data Characteristic
  ├─ EPD Status Notify Characteristic
  └─ Buttonless DFU Service

nRF52811 Firmware
  ├─ boot_manager
  ├─ ble_manager
  ├─ protocol_parser
  ├─ transfer_manager
  ├─ storage_manager
  ├─ render_engine
  ├─ epd_uc8179_driver
  ├─ fallback_screen
  ├─ mode_manager
  ├─ dfu_manager
  ├─ power_manager
  ├─ battery_manager
  ├─ watchdog_manager
  └─ diagnostics_manager

UC8179 / EPD Panel
  └─ 800x480 B/W/R physical display
```

### 4.2 端到端数据流

```text
Web 端选择模式
  ↓
Web 按旧 hard-coded 坐标生成 UI
  ↓
Web 渲染中文、数字、节假日、农历、天气等文本为 bitmap
  ↓
Web 生成 black plane / red plane / block / small recipe
  ↓
Web 通过 BLE 分包发送 object header + chunks
  ↓
MCU 校验 object header、chunk、CRC、尺寸、坐标和 store_policy
  ↓
大对象：流式写入 EPD，不写入 FDS
小对象：写入 FDS 双副本或 active record
  ↓
MCU 调用 render_engine / uc8179 driver 刷屏
  ↓
MCU 更新 last_sync_time、active_mode、last_object_id 等小状态
```

---

## 5. Web / Mobile 端设计

### 5.1 Web 端职责

Web / Mobile 是完整应用层，负责所有复杂逻辑：

```text
1. GUI 设计和模板管理。
2. 旧 hard-coded UI 坐标迁移和 pixel lock。
3. 日历、时钟、星期、节假日、农历、调休数据生成。
4. 字体渲染，尤其中文字体、大号数字、小号节假日文字。
5. 图片素材处理、缩放、裁剪、二值化、三色 plane 合成。
6. 生成 800x480 black/red plane。
7. 生成局部 bitmap block。
8. 生成小型 recipe。
9. 生成传输 manifest 和 CRC。
10. BLE 分包发送和重传。
11. 读取 MCU 状态、错误码、电量、固件版本。
12. 触发 Buttonless DFU。
```

### 5.2 旧 hard-coded 坐标兼容层

所有模式 UI 必须与旧 hard-coded 版本像素一致。Web 端需要维护模板文件：

```text
calendar.template.json
clock.template.json
clock_calendar.template.json
fallback.template.json
image_display.template.json
```

每个模板至少包含：

```json
{
  "mode_id": 1,
  "mode_name": "calendar",
  "canvas": { "w": 800, "h": 480 },
  "coordinate": {
    "origin": "top_left",
    "rotation": 0
  },
  "compatibility": {
    "legacy": "hardcoded_v1",
    "pixel_locked": true
  },
  "elements": []
}
```

强制规则：

```text
1. 不允许使用响应式布局改变最终输出。
2. 不允许浏览器 DPI 影响输出像素。
3. 不允许 CSS 字体差异影响最终 MCU 显示。
4. 最终输出必须是 800x480 物理像素图。
5. 日期格子、线条、标题栏、时间区域等固定元素必须 0 diff。
```

### 5.3 Web 端输出对象

Web 编译后输出：

```text
full_frame_black.bin
full_frame_red.bin
block_black_x_y_w_h.bin
block_red_x_y_w_h.bin
small_recipe.bin，可选
object_header.bin
transfer_manifest.json
```

推荐默认输出：

```text
整屏刷新：black plane + red plane
局部刷新：block + 坐标 + plane
小状态：mode_state / time_state / config
```

---

## 6. MCU 端总体设计

### 6.1 MCU 端定位

MCU 固件不是 GUI 系统，而是显示终端。它只做以下事情：

```text
1. BLE 连接。
2. GATT 服务。
3. 接收 Web 下发对象。
4. 校验对象完整性。
5. 大 bitmap 流式写 EPD。
6. 小状态写 FDS。
7. 显示兜底页。
8. 管理低功耗和电池状态。
9. 支持 Buttonless DFU。
10. 暴露状态和错误码给 Web。
```

禁止在 MCU 端新增：

```text
完整 GUI 框架
完整中文字库
完整日历算法
节假日/调休算法
农历算法
JSON parser
PNG/JPEG/BMP 解码库
完整 framebuffer
大压缩库
模板引擎
```

### 6.2 推荐目录结构

```text
application/
  main.c
  app_config.h
  app_version.h

ble/
  ble_epd_service.c
  ble_epd_service.h
  ble_status_service.c
  ble_status_service.h
  ble_dfu_entry.c
  ble_dfu_entry.h

protocol/
  epd_protocol.h
  epd_protocol.c
  epd_object.h
  epd_error.h
  epd_crc.c
  epd_crc.h

transfer/
  transfer_manager.c
  transfer_manager.h

storage/
  storage_manager.c
  storage_manager.h
  storage_records.h

render/
  render_engine.c
  render_engine.h
  fallback_screen.c
  fallback_screen.h

drivers/
  epd_uc8179.c
  epd_uc8179.h
  epd_spi.c
  epd_spi.h
  battery_adc.c
  battery_adc.h

system/
  power_manager.c
  power_manager.h
  watchdog_manager.c
  watchdog_manager.h
  diagnostics_manager.c
  diagnostics_manager.h
```

---

## 7. Flash / DFU / FDS 设计

### 7.1 Flash 布局原则

nRF52811 内部 Flash 需要同时容纳：

```text
MBR
SoftDevice S112
Application App
FDS / App Data
Bootloader
Bootloader Settings Page
MBR Params Page，可选
DFU 临时空间 / Bank
```

设计原则：

```text
1. 先保证 SoftDevice + Bootloader + DFU + FDS，再决定 App 功能。
2. App size 不是越接近可用空间越好，而是要留出 DFU 余量。
3. FDS 每增加 1 page，DFU 可用空间减少 4 KB 级别。
4. Bootloader 稳定后尽量冻结，不频繁升级 SoftDevice/Bootloader。
5. 默认只做 Application DFU，不默认做 SoftDevice + Bootloader 联合升级。
```

### 7.2 App Flash 门禁

```text
目标值：≤ 40 KB
警戒线：40~48 KB
危险线：> 48 KB
```

超过 40 KB 必须 review：

```text
是否引入字体
是否引入 GUI
是否引入 JSON parser
是否引入图片解码
是否引入 printf / float printf
是否引入大数组 / 大查找表
是否打开过多 log / assert
```

超过 48 KB：

```text
停止新增功能。
先裁剪 MCU 端功能。
不能通过压缩 FDS 保护区或 DFU 空间来换功能。
```

### 7.3 FDS 定位

FDS 只用于保存小型、低频、重启后必须保留的数据。

FDS 保存：

```text
schema_version
device_name
active_mode
last_sync_time
last_success_object_id
battery_state
fallback_config
small_recipe_meta
transfer_resume_meta
diagnostic counters
```

FDS 不保存：

```text
完整 framebuffer
完整 black plane
完整 red plane
完整 800x480 三色画面
大图片
完整中文字体
整年日历 bitmap
完整 JSON
完整节假日数据库
```

默认建议：

```text
FDS_VIRTUAL_PAGES = 2
FDS 总空间 = 8 KB
NRF_DFU_APP_DATA_AREA_SIZE = 8192 bytes
```

如果 FDS 扩展到 3 pages，则必须同步调整：

```text
FDS_VIRTUAL_PAGES = 3
NRF_DFU_APP_DATA_AREA_SIZE = 12288 bytes
```

并重新验证 OTA DFU。

### 7.4 DFU 策略

推荐：

```text
DFU 类型：Single-bank Application DFU 优先
DFU 入口：Buttonless DFU Service + Web 控制命令
DFU 前：停止显示传输、等待 FDS 操作完成、停止 EPD refresh、检查电池电压
DFU 后：不恢复旧画面，显示兜底页，等待 Web 重新同步
```

DFU 前流程：

```text
CMD_ENTER_DFU
  ↓
检查 battery_ok
  ↓
abort 当前 transfer
  ↓
等待 EPD 非 busy 或超时
  ↓
等待 FDS pending 操作完成
  ↓
保存 entering_dfu 状态，可选
  ↓
断开业务连接
  ↓
进入 bootloader
```

---

## 8. BLE 协议设计

### 8.1 GATT 服务

推荐服务：

```text
EPD Control Service
EPD Data Service
EPD Status Notify Service
Buttonless DFU Service
```

Characteristic：

```text
EPD_CMD_CHAR       Web -> MCU：控制命令
EPD_DATA_CHAR      Web -> MCU：数据 chunk
EPD_STATUS_CHAR    MCU -> Web：状态通知
EPD_INFO_CHAR      Web 读取设备信息
```

### 8.2 对象头

```c
typedef struct {
    uint32_t magic;          // 'EPDO'
    uint16_t version;
    uint16_t header_len;
    uint32_t object_id;
    uint8_t  object_type;
    uint8_t  store_policy;
    uint8_t  color_plane;
    uint8_t  flags;
    uint16_t screen_w;
    uint16_t screen_h;
    uint32_t total_len;
    uint32_t crc32;
    uint32_t meta_crc32;
} epd_object_header_t;
```

object_type：

```c
#define OBJ_FRAME_FULL          0x01
#define OBJ_FRAME_BLOCK         0x02
#define OBJ_RECIPE_SMALL        0x03
#define OBJ_MODE_STATE          0x04
#define OBJ_DEVICE_CONFIG       0x05
#define OBJ_TIME_STATE          0x06
#define OBJ_FALLBACK_CONFIG     0x07
#define OBJ_ENTER_DFU           0x08
```

store_policy：

```c
#define STORE_NONE              0x00  // 只显示，不保存
#define STORE_STATE_ONLY        0x01  // 只保存状态 / 元数据
#define STORE_SMALL_FDS         0x02  // 小对象写 FDS
#define STORE_EXTERNAL_FLASH    0x03  // 预留外部 Flash
```

默认策略：

```text
OBJ_FRAME_FULL      -> STORE_NONE 或 STORE_STATE_ONLY
OBJ_FRAME_BLOCK     -> STORE_NONE 或 STORE_STATE_ONLY
OBJ_RECIPE_SMALL    -> STORE_SMALL_FDS
OBJ_MODE_STATE      -> STORE_STATE_ONLY
OBJ_DEVICE_CONFIG   -> STORE_SMALL_FDS
OBJ_TIME_STATE      -> STORE_STATE_ONLY
```

### 8.3 命令

```c
#define CMD_QUERY_INFO          0x01
#define CMD_BEGIN_OBJECT        0x02
#define CMD_WRITE_CHUNK         0x03
#define CMD_END_OBJECT          0x04
#define CMD_ABORT_OBJECT        0x05
#define CMD_APPLY_OBJECT        0x06
#define CMD_REFRESH             0x07
#define CMD_CLEAR               0x08
#define CMD_SHOW_FALLBACK       0x09
#define CMD_ENTER_DFU           0x0A
#define CMD_QUERY_STATUS        0x0B
```

chunk 格式：

```c
typedef struct {
    uint32_t object_id;
    uint32_t offset;
    uint16_t seq;
    uint16_t len;
    uint32_t chunk_crc32;
    uint8_t  payload[];
} epd_chunk_t;
```

---

## 9. Transfer Manager 设计

### 9.1 职责

```text
1. 创建对象接收上下文。
2. 校验 object header。
3. 管理 seq、offset、chunk_len。
4. 校验 chunk_crc32。
5. 统计接收进度。
6. 对大对象流式写入 EPD 或 render buffer。
7. 对小对象进入 staging buffer。
8. END_OBJECT 时做整体验证。
9. 验证通过后通知 render_engine 或 storage_manager。
```

### 9.2 状态机

```text
IDLE
  ↓ CMD_BEGIN_OBJECT
RECEIVING
  ↓ chunks received
VERIFYING
  ↓ CRC OK
READY_TO_APPLY
  ↓ CMD_APPLY_OBJECT
APPLYING
  ↓ success
DONE
```

异常状态：

```text
ABORTED
CRC_ERROR
TIMEOUT
LOW_BATTERY_BLOCKED
EPD_BUSY_TIMEOUT
UNSUPPORTED_OBJECT
```

### 9.3 大对象处理

`OBJ_FRAME_FULL` / `OBJ_FRAME_BLOCK`：

```text
1. 不进入 FDS。
2. 不进入完整 RAM framebuffer。
3. 通过 chunk / line / block 流式写入 EPD RAM。
4. END_OBJECT 前不 refresh。
5. CRC 失败则丢弃，不更新 last_success_object_id。
```

### 9.4 小对象处理

`OBJ_DEVICE_CONFIG` / `OBJ_RECIPE_SMALL`：

```text
1. 进入小 staging buffer。
2. 完整 CRC 通过后写入 FDS。
3. 写入成功后更新 active meta。
4. 写入失败保持旧记录。
```

---

## 10. Render Engine 设计

### 10.1 定位

Render Engine 是极简显示执行器，不是 GUI 框架。

支持：

```text
clear
draw_bitmap_block
draw_full_plane
draw_line_basic，可选
draw_rect_basic，可选
refresh_full
refresh_partial，可选
sleep_panel
```

不支持：

```text
复杂布局系统
Flex/Grid/Constraint Layout
中文字体渲染
文本自动换行
日历计算
图片缩放/旋转
复杂压缩格式解码
```

### 10.2 边界检查

每个显示对象必须检查：

```text
x/y 是否越界
w/h 是否为 0
x+w 是否超过 800
y+h 是否超过 480
plane 是否支持
bitmap len 是否匹配区域大小
EPD 是否 busy
电池电压是否允许刷新
```

越界对象默认拒绝，不自动裁剪。原因是自动裁剪会导致 Web 预览和 MCU 实际显示不一致。

---

## 11. UC8179 Driver 设计

### 11.1 职责

```text
GPIO 初始化
SPI 初始化
RESET / DC / CS / BUSY 控制
UC8179 初始化序列
写 command
写 data
写 black plane RAM
写 red plane RAM
refresh
sleep
busy wait timeout
```

### 11.2 不负责

```text
BLE
FDS
GUI
日历
字体
Web 协议
模式业务逻辑
```

### 11.3 坐标转换

如果 UC8179 RAM 写入方向和视觉坐标不一致，坐标转换只能放在一个地方：

```text
epd_uc8179_driver
或 panel_mapper
```

禁止 Web、renderer、driver 各自转换一次。

### 11.4 BUSY 超时

EPD BUSY 超时处理：

```text
1. 停止当前写屏。
2. 记录 ERR_EPD_BUSY_TIMEOUT。
3. 通知 Web 状态。
4. 必要时等待下一次同步或复位。
5. 复位后显示 fallback screen。
```

---

## 12. Fallback Screen 设计

### 12.1 触发条件

以下情况显示兜底页：

```text
上电冷启动
断电重启
DFU 后首次启动
FDS schema 不兼容
没有有效 active_mode
上次传输未完成
上次显示对象 CRC 错误
Web 显式发送 SHOW_FALLBACK
用户按键重置显示状态
Watchdog 复位后
```

### 12.2 显示内容

最低要求：

```text
BLE name
同步提示
```

推荐内容：

```text
Device: NRF_EPD_xxxx
Status: Sync Required
Open Web Tool and reconnect
FW: x.y.z
Battery: Low / OK
```

如果必须中文，建议只内置固定小 bitmap，例如：

```text
请重新同步
```

不要内置完整中文字库。

### 12.3 资源约束

Fallback Screen 必须极小：

```text
Flash：0.5~1.5 KB
RAM：不使用完整 framebuffer
绘制方式：ASCII 小字体 + 简单线条 + 少量固定 bitmap
```

---

## 13. Mode Manager 设计

### 13.1 支持模式

```text
calendar
clock
clock_calendar
fallback
image_display
```

### 13.2 MCU 端保存内容

MCU 只保存：

```text
active_mode_id
last_sync_time
last_object_id
mode_schema_version
```

MCU 不保存：

```text
模式完整 UI
模式完整数据
日历完整数据
节假日完整数据
整屏 bitmap
```

### 13.3 模式切换

模式切换流程：

```text
Web 选择模式
  ↓
Web 生成该模式对应 bitmap / recipe
  ↓
BLE 下发
  ↓
MCU 校验并显示
  ↓
MCU 仅记录 active_mode_id 和 last_sync_time
```

如果模式切换中断：

```text
不更新 active_mode。
不更新 last_success_object_id。
下次启动显示 fallback screen。
```

---

## 14. Storage Manager / FDS 记录设计

### 14.1 FDS 文件分类

```c
#define FDS_FILE_SYSTEM          0x1000
#define FDS_FILE_DEVICE          0x1001
#define FDS_FILE_MODE            0x1002
#define FDS_FILE_TRANSFER        0x1003
#define FDS_FILE_FALLBACK        0x1004
#define FDS_FILE_DIAGNOSTIC      0x1005
#define FDS_FILE_RECIPE_SMALL    0x1006
```

### 14.2 FDS 记录

```c
#define REC_SCHEMA_VERSION       0x0001
#define REC_DEVICE_CONFIG        0x0002
#define REC_DEVICE_NAME          0x0003
#define REC_ACTIVE_MODE          0x0010
#define REC_LAST_SYNC            0x0011
#define REC_TRANSFER_META        0x0020
#define REC_TRANSFER_STATE       0x0021
#define REC_FALLBACK_CONFIG      0x0030
#define REC_DIAG_COUNTERS        0x0040
#define REC_SMALL_RECIPE_META    0x0050
#define REC_SMALL_RECIPE_DATA    0x0051
```

### 14.3 写入策略

```text
写新记录
  ↓
校验 CRC
  ↓
切换 active 标记
  ↓
延迟 GC
```

断电边界：

```text
写入中断：旧状态仍有效。
新状态 CRC 失败：忽略新状态。
active_mode 丢失：显示 fallback。
schema 不兼容：显示 fallback，等待 Web 重同步。
```

---

## 15. Power / Battery / Watchdog 设计

### 15.1 Power Manager

职责：

```text
BLE idle 后进入低功耗
EPD sleep
系统 sleep
RTC 唤醒，可选
按键唤醒，可选
```

### 15.2 Battery Manager

职责：

```text
ADC 采样电池电压
电压滤波
低电阈值判断
刷新前电量检查
DFU 前电量检查
状态上报给 Web
```

低电策略：

```text
刷新前低电：拒绝刷新，返回 ERR_LOW_BATTERY。
刷新中低电：尽量完成当前刷新，不再开始新任务。
DFU 前低电：拒绝进入 DFU 或要求用户处理电量。
```

### 15.3 Watchdog Manager

职责：

```text
BLE 长传输期间喂狗
EPD busy wait 期间喂狗
FDS pending wait 期间喂狗
异常卡死后复位
```

复位后：

```text
显示 fallback screen，不恢复旧画面。
```

---

## 16. Diagnostics / Status 设计

### 16.1 Web 可读取设备信息

```text
firmware_version
protocol_version
device_name
model_id
panel_width
panel_height
color_capability
dfu_available
fds_status
battery_mv
last_error
last_sync_time
active_mode
boot_count
```

### 16.2 错误码

```c
#define ERR_OK                     0x0000
#define ERR_UNSUPPORTED_CMD        0x0001
#define ERR_UNSUPPORTED_OBJECT     0x0002
#define ERR_CRC_FAIL               0x0003
#define ERR_SEQ_FAIL               0x0004
#define ERR_OFFSET_FAIL            0x0005
#define ERR_OBJECT_TOO_LARGE       0x0006
#define ERR_LOW_BATTERY            0x0007
#define ERR_EPD_BUSY_TIMEOUT       0x0008
#define ERR_FDS_WRITE_FAIL         0x0009
#define ERR_DFU_BLOCKED            0x000A
#define ERR_SCHEMA_MISMATCH        0x000B
```

### 16.3 诊断计数

FDS 可保存少量计数：

```text
boot_count
dfu_count
crc_error_count
epd_timeout_count
low_battery_block_count
fds_error_count
```

---

## 17. 当前工程模块改造规划

### 17.1 删除 / 迁移到 Web

| 模块 | 当前占用 | 处理方式 |
|---|---:|---|
| fonts.o | 9,821 B | 中文/数字字体迁移到 Web 栅格化 |
| u8g2_font.o | 830 B | 移除，MCU 不做复杂字体 |
| mc_w.l / mf_w.l | 4,264 B | 裁剪字体/库依赖 |
| gui.o | 9,014 B | 坐标提取为 Web template，MCU 删除 GUI 页面逻辑 |
| lunar.o | 2,110 B | 农历/节假日/调休迁移到 Web |
| adafruit_gfx.o | 3,772 B | 替换为极简 block/plane 写屏 |
| printfa.o | 2,340 B | 关闭 printf/float printf，错误码替代字符串日志 |

### 17.2 保留但瘦身

| 模块 | 当前占用 | 处理方式 |
|---|---:|---|
| uc8179.o | 4,886 B | 保留必要 LUT/初始化，删除未用屏幕配置/测试表 |
| epd_driver.o | 1,494 B | 保留 block/plane/refresh/sleep，不加入 GUI |
| epd_service.o | 3,414 B | 重构为 BLE 服务 + 协议入口，不包含 GUI |
| main.o | Flash 1,394 B / RAM 2,696 B | 移除全局大 buffer，职责下沉到 manager |

### 17.3 保留

```text
fds.o
nrf_fstorage_sd.o
nrf_fstorage.o
ble_dfu.o
ble_dfu_unbonded.o
nrf_dfu_svci.o
nrf_sdh.o
nrf_sdh_ble.o
nrf_ble_gatt.o
ble_advdata.o
ble_advertising.o
ble_conn_params.o
app_timer2.o
nrfx_spi.o
nrfx_gpiote.o
nrf_pwr_mgmt.o
nrfx_wdt.o
drv_rtc.o
```

---

## 18. 目标模块预算

| 模块 | 目标 Flash | 说明 |
|---|---:|---|
| BLE 基础 + EPD Service | 6~8 KB | 保留，控制 GATT 复杂度 |
| DFU App Support | <1 KB | 保留入口，不扩大 |
| FDS / Storage | 5~6 KB | 当前可接受，FDS 默认 2 pages |
| UC8179 + EPD Driver | 6~7 KB | 当前约 6.6 KB，不继续扩大 |
| Protocol + Transfer | 3~5 KB | 新增，必须极简 |
| Fallback Screen | 0.5~1.5 KB | 小字体/固定 bitmap |
| Power / Battery / WDT / Diag | 2~3 KB | 控制功能范围 |
| main / glue | 1~2 KB | 避免业务堆积 |
| C runtime | 1~2 KB | 去掉 printfa / float 后降低 |

目标总量：

```text
App Flash：36~40 KB
App RAM：10~12 KB，含合理 stack
FDS：默认 8 KB
```

---

## 19. 实际场景处理

### 19.1 断电重启

```text
不恢复旧画面。
显示 fallback screen。
提示用户连接 Web 重新同步。
```

### 19.2 BLE 传输中断

```text
丢弃未完成对象。
不更新 active_mode。
不更新 last_success_object_id。
Web 重连后重新传输。
```

### 19.3 Web 下发错误坐标

```text
MCU 检查越界。
拒绝对象。
不上屏。
不自动裁剪。
返回错误码。
```

### 19.4 Web 下发错误屏幕尺寸

```text
如果不是 800x480，拒绝对象。
```

### 19.5 DFU 后首次启动

```text
检查 FDS schema。
不恢复旧画面。
显示 fallback screen。
等待 Web 重同步。
```

### 19.6 低电量刷新

```text
刷新前检测低电则拒绝刷新。
返回 ERR_LOW_BATTERY。
Web 提示用户处理电量。
```

### 19.7 EPD BUSY 卡死

```text
超时退出。
记录 ERR_EPD_BUSY_TIMEOUT。
允许 Web 查询错误。
复位后显示 fallback。
```

### 19.8 schema 升级

```text
兼容则迁移小状态。
不兼容则清除运行状态，显示 fallback。
不尝试恢复旧画面。
```

---

## 20. 测试和验收标准

### 20.1 资源验收

```text
App Flash ≤ 40 KB 为目标。
40~48 KB 必须 size review。
>48 KB 禁止继续新增功能。
RAM 不允许出现完整 framebuffer。
main.o ZI_data 需要压缩到合理范围。
Stack 需要通过 watermark 实测。
```

### 20.2 DFU 验收

```text
DFU zip 可生成。
实机 OTA 可完成。
DFU 后能正常启动。
DFU 后显示 fallback screen。
FDS 小状态不被错误覆盖。
NRF_DFU_APP_DATA_AREA_SIZE 与 FDS_VIRTUAL_PAGES 匹配。
```

### 20.3 显示验收

```text
Web 输出 800x480 black/red plane。
固定 UI 元素与旧 hard-coded 坐标 0 diff。
日期格子、分割线、标题栏、时钟区域位置一致。
MCU 不做坐标修正。
EPD 刷新后画面与 Web 预览一致。
```

### 20.4 传输验收

```text
chunk 丢失可检测。
seq 错误可检测。
offset 错误可检测。
CRC 错误可检测。
断连后不写 active 状态。
重复 object_id 行为明确。
```

### 20.5 异常验收

```text
断电重启显示 fallback。
FDS schema 错误显示 fallback。
EPD busy 超时有错误码。
低电禁止刷新。
Watchdog 复位后显示 fallback。
```

---

## 21. 后续 AI Coding 规则

后续所有 AI coding 必须遵守：

```text
1. 不在 MCU 端新增复杂 GUI 框架。
2. 不在 MCU 端新增完整中文字库。
3. 不在 MCU 端新增完整日历/节假日/农历算法。
4. 不把完整 800x480 bitmap 写入 FDS。
5. 不在 RAM 中创建完整 framebuffer。
6. 不破坏 Buttonless DFU。
7. 不随意扩大 FDS page 数。
8. 不改变旧 hard-coded UI 的像素坐标。
9. 不让 MCU 做自适应布局。
10. 大显示对象默认流式显示，小状态才写 FDS。
11. 任意错误状态优先 fallback 或等待 Web 重同步。
12. 每次改动后检查 app size / RAM / DFU / FDS。
```

推荐任务输入格式：

```text
请基于 nRF_EPD 完整方案说明书修改当前工程。

目标模块：<module_name>
具体需求：<feature>

必须遵守：
- nRF52811 资源限制
- FDS 不保存完整画面
- 断电后显示蓝牙名称和重新同步提示
- 所有模式 UI 像素坐标必须与旧 hard-coded 版本一致
- 不破坏 Buttonless DFU
- 不引入大字体 / 大 GUI / 大 JSON parser

输出：
- 修改文件列表
- 核心代码
- size 风险
- 测试方法
```

---

## 22. 最终结论

当前工程的占用数据证明，新架构不是理论优化，而是针对当前问题的直接修正：

```text
当前 App 约 64 KB，偏大。
最大占用集中在字体、GUI、农历、GFX、printf。
这些模块正好都应该迁移到 Web 端或裁剪。
裁剪后 App 有机会降到 36~40 KB。
这更适合 nRF52811 + BLE + FDS + DFU 的组合。
```

最终方案应收敛为：

```text
Web / Mobile：完整应用层。
nRF52811：低功耗 BLE 显示终端。
FDS：小状态持久化。
EPD Driver：硬件显示执行。
DFU：最高优先级维护能力。
Fallback Screen：所有异常和重启后的统一入口。
```

最重要的工程决策：

```text
1. 断电后不恢复原画面，统一显示包含蓝牙名称的同步提示页。
2. 所有模式的新 Web 下发布局，必须与旧 MCU hard-coded 坐标像素一致。
3. MCU 端只保留 BLE、协议、FDS 小状态、EPD 驱动、fallback、DFU、低功耗和诊断。
4. GUI、字体、日历、节假日、农历、图片合成全部迁移到 Web / Mobile 端。
5. DFU / Flash / RAM 预算作为硬性门禁，优先级高于所有新增显示功能。
```
