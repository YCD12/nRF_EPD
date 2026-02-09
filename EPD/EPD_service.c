/* Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

/**
 * @file EPD_service.c
 * @brief EPD BLE服务实现文件
 *
 * 本文件实现了墨水屏(EPD)的BLE服务,主要功能包括:
 *
 * ============================================================================
 * 1. 命令队列机制 (Command Queue Mechanism)
 * ============================================================================
 *    - 使用环形缓冲区(circular buffer)管理命令队列
 *    - 支持最多8个命令同时排队 (EPD_CMD_QUEUE_SIZE)
 *    - 通过app_scheduler异步顺序执行命令
 *    - 紧急命令(复位、睡眠、擦除配置)绕过队列立即执行
 *
 *    工作流程:
 *    (1) BLE写入特性 -> on_write() -> epd_service_on_write()
 *    (2) 紧急命令直接执行,普通命令入队
 *    (3) 如果队列未在处理,通过app_scheduler启动处理
 *    (4) epd_process_next_command()从队列取出一个命令执行
 *    (5) 执行完毕后,如果队列还有命令,继续调度下一个
 *    (6) 队列为空时标记is_processing=false
 *
 * ============================================================================
 * 2. 刷新策略 (全刷/局刷智能切换)
 * ============================================================================
 *    局刷使用条件:
 *    - 显示模式为MODE_CLOCK或MODE_CLOCK_CALENDAR
 *    - 驱动芯片为UC8179(支持局刷)
 *    - 驱动提供了refresh_partial函数
 *
 *    全刷触发条件:
 *    - 首次刷新 (last_full_clear == 0)
 *    - 强制全刷标志 (force_full_refresh)
 *    - 达到全刷间隔(默认每小时)且非夜间静默时段
 *
 *    局刷优势:
 *    - 刷新速度快(约1秒 vs 全刷的10-15秒)
 *    - 功耗低
 *    - 闪烁少
 *
 *    局刷劣势:
 *    - 长期使用会积累残影
 *    - 需要定期全刷来清除残影
 *
 * ============================================================================
 * 3. 夜间静默逻辑 (Quiet Hours)
 * ============================================================================
 *    时间范围: 01:00 - 05:59
 *    目的: 减少夜间闪烁和功耗
 *    效果: 跳过整点全刷,仅使用局刷
 *    注意: 首次全刷和强制全刷不受夜间静默影响
 *
 * ============================================================================
 * 4. 显示模式管理
 * ============================================================================
 *    - MODE_PICTURE (0): 图片模式,用于显示自定义图片
 *    - MODE_CALENDAR (1): 日历模式,每天00:00更新
 *    - MODE_CLOCK_CALENDAR (2): 时钟+日历模式,每分钟更新时间区域
 *    - MODE_CLOCK (3): 时钟模式,每分钟更新
 *
 * ============================================================================
 * 5. BLE服务管理
 * ============================================================================
 *    - 处理连接/断开事件
 *    - 处理特性写入(命令接收)
 *    - 发送通知给对端
 */

#include "EPD_service.h"

#include <string.h>

#include "app_scheduler.h"
#include "ble_srv_common.h"
#include "main.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_pwr_mgmt.h"
#include "sdk_macros.h"
#include "Lunar.h"

// ============================================================================
// CRC16计算和传输响应函数
// CRC16 Calculation and Transfer Response Functions
// ============================================================================

/**@brief 计算CRC16-CCITT校验值
 *        Calculate CRC16-CCITT checksum
 *
 * @details 使用多项式0x8408 (反转的0x1021)
 *          用于验证BLE传输数据的完整性
 *          Uses polynomial 0x8408 (reversed 0x1021)
 *          Used to verify BLE transfer data integrity
 *
 * @param[in] data  数据指针 / Pointer to data
 * @param[in] len   数据长度 / Data length
 *
 * @return CRC16校验值 / CRC16 checksum value
 */
static uint16_t crc16_compute(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return crc;
}

/**@brief 发送块响应(ACK/NACK)到APP
 *        Send block response (ACK/NACK) to APP
 *
 * @details 响应格式: [0xA0, block_id_L, block_id_H, status]
 *          status: 0x00 = 成功(ACK), 0x01 = 失败(NACK,需重传)
 *          Response format: [0xA0, block_id_L, block_id_H, status]
 *          status: 0x00 = success(ACK), 0x01 = failure(NACK, need retransmit)
 *
 * @param[in] p_epd     EPD服务结构指针 / Pointer to EPD service structure
 * @param[in] block_id  块ID / Block ID
 * @param[in] status    状态码(0x00=成功, 0x01=CRC错误) / Status (0x00=OK, 0x01=CRC error)
 */
static void send_block_response(ble_epd_t* p_epd, uint16_t block_id, uint8_t status) {
    uint8_t rsp[] = {EPD_RSP_BLOCK_ACK, block_id & 0xFF, block_id >> 8, status};
    ble_epd_string_send(p_epd, rsp, sizeof(rsp));
    NRF_LOG_DEBUG("[EPD]: Block %d response: %s\n", block_id, status == 0 ? "ACK" : "NACK");
}

/**@brief 发送传输状态响应到APP
 *        Send transfer status response to APP
 *
 * @details 响应格式: [0xA1, total_L, total_H, received_L, received_H, session_id, active, bitmap...]
 *          用于断点续传时查询已接收的块
 *          Response format: [0xA1, total_L, total_H, received_L, received_H, session_id, active, bitmap...]
 *          Used to query received blocks for resume upload
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD service structure
 */
static void send_transfer_status(ble_epd_t* p_epd) {
    // 响应格式: [0xA1, total_L, total_H, received_L, received_H, session_id, active, bitmap...]
    uint8_t rsp[7 + EPD_BLOCK_BITMAP_SIZE];
    rsp[0] = EPD_RSP_STATUS;
    rsp[1] = p_epd->transfer_ctx.total_blocks & 0xFF;
    rsp[2] = p_epd->transfer_ctx.total_blocks >> 8;
    rsp[3] = p_epd->transfer_ctx.received_blocks & 0xFF;
    rsp[4] = p_epd->transfer_ctx.received_blocks >> 8;
    rsp[5] = p_epd->transfer_ctx.session_id;
    rsp[6] = p_epd->transfer_ctx.transfer_active ? 1 : 0;
    
    // 只发送必要的位图长度
    uint16_t bitmap_len = (p_epd->transfer_ctx.total_blocks + 7) / 8;
    if (bitmap_len > EPD_BLOCK_BITMAP_SIZE) bitmap_len = EPD_BLOCK_BITMAP_SIZE;
    memcpy(&rsp[7], p_epd->transfer_ctx.block_bitmap, bitmap_len);
    
    ble_epd_string_send(p_epd, rsp, 7 + bitmap_len);
    NRF_LOG_INFO("[EPD]: Transfer status sent: total=%d, received=%d, session=%d, active=%d\n",
                 p_epd->transfer_ctx.total_blocks, p_epd->transfer_ctx.received_blocks,
                 p_epd->transfer_ctx.session_id, p_epd->transfer_ctx.transfer_active);
}

// nRF52811默认引脚配置
// Default pin configuration for nRF52811
#define EPD_CFG_52811 {0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02, 0x07, 0xFF, 0x12, 0x07}


// ============================================================================
// 局部刷新偏移上下文结构
// Context for partial write with offset (used for time-only refresh)
// ============================================================================
// 在MODE_CLOCK_CALENDAR模式下,只需要刷新时间显示区域,
// 该结构用于指定刷新的偏移位置,将局部坐标转换为屏幕绝对坐标
typedef struct {
    epd_model_t* epd;       // EPD模型指针 / Pointer to EPD model
    uint16_t offset_x;      // X轴偏移量(相对于屏幕左上角) / X offset from screen origin
    uint16_t offset_y;      // Y轴偏移量(相对于屏幕左上角) / Y offset from screen origin
} partial_write_ctx_t;

/**@brief 带偏移的局部写入回调包装函数
 *        Callback wrapper that adds offset to write_image_partial coordinates
 *
 * @details 用途: 在绘制GUI时,将局部坐标转换为屏幕绝对坐标。
 *          当MODE_CLOCK_CALENDAR模式下只刷新时间区域时,
 *          DrawGUI_ClockOnly输出的坐标是相对于时间区域原点的,
 *          此函数将其转换为相对于屏幕左上角的绝对坐标。
 *
 * @param[in] ctx    上下文指针,包含EPD和偏移信息 / Context with EPD and offset
 * @param[in] black  黑色图像数据 / Black image data
 * @param[in] color  彩色图像数据 / Color image data
 * @param[in] x      相对X坐标 / Relative X coordinate
 * @param[in] y      相对Y坐标 / Relative Y coordinate
 * @param[in] w      宽度 / Width
 * @param[in] h      高度 / Height
 */
static void write_image_partial_with_offset(void* ctx, uint8_t* black, uint8_t* color,
                                            uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    partial_write_ctx_t* context = (partial_write_ctx_t*)ctx;
    // 将局部坐标加上偏移,转换为屏幕绝对坐标
    context->epd->drv->write_image_partial(context->epd, black, color,
        x + context->offset_x, y + context->offset_y, w, h);
}

// ============================================================================
// 命令队列管理函数
// Command Queue Management Functions
// ============================================================================
//
// 命令队列的设计目的:
// 1. EPD操作需要较长时间(如刷新需要几秒),期间可能收到新命令
// 2. BLE可能快速发送多个命令,需要缓冲并顺序执行
// 3. 防止命令并发执行导致EPD驱动冲突
//
// 实现原理:
// - 使用环形缓冲区(circular buffer)
// - head指向下一个要取出的命令
// - tail指向下一个要插入命令的位置
// - count记录当前队列中的命令数量
// - is_processing标志防止重复启动处理流程
//
// ============================================================================

/**@brief 初始化命令队列
 *        Initialize command queue
 *
 * @details 清除所有队列项并将头/尾/计数重置为零。
 *          Clears all queue items and resets head/tail/count to zero.
 *
 * @param[in] queue  命令队列结构指针 / Pointer to command queue structure
 */
static void epd_cmd_queue_init(epd_cmd_queue_t* queue) {
    memset(queue, 0, sizeof(epd_cmd_queue_t));
    queue->head = 0;        // 队列头索引 / Queue head index
    queue->tail = 0;        // 队列尾索引 / Queue tail index
    queue->count = 0;       // 当前队列项数 / Current item count
    queue->is_processing = false;  // 未在处理 / Not processing
    NRF_LOG_INFO("[EPD]: Command queue initialized\n");
}

/**@brief 将命令推入队列
 *        Push command to queue
 *
 * @details 将命令添加到队列尾部。如果队列已满,则丢弃命令。
 *          Adds a command to the tail of the queue. If queue is full, command is dropped.
 *
 *          队列满的处理:
 *          - 记录警告日志,显示被丢弃的命令ID
 *          - 返回false表示入队失败
 *          - 调用者需要检查返回值
 *
 * @param[in] queue   命令队列结构指针 / Pointer to command queue structure
 * @param[in] p_data  命令数据指针 / Pointer to command data
 * @param[in] length  命令数据长度 / Length of command data
 *
 * @return true 如果命令成功入队 / if command was queued successfully
 * @return false 如果队列已满 / if queue is full
 */
static bool epd_cmd_queue_push(epd_cmd_queue_t* queue, uint8_t* p_data, uint16_t length) {
    // 检查队列是否已满
    if (queue->count >= EPD_CMD_QUEUE_SIZE) {
        NRF_LOG_WARNING("[EPD]: Command queue full (size=%d), dropping command 0x%02x\n",
                        EPD_CMD_QUEUE_SIZE, p_data[0]);
        return false;
    }

    // 检查数据长度是否超限
    if (length > EPD_CMD_MAX_DATA_LEN) {
        NRF_LOG_WARNING("[EPD]: Command data too long (%d > %d), truncating\n",
                        length, EPD_CMD_MAX_DATA_LEN);
        length = EPD_CMD_MAX_DATA_LEN;
    }

    // 将命令复制到队列尾部
    epd_cmd_queue_item_t* item = &queue->items[queue->tail];
    item->cmd = p_data[0];
    item->length = length;
    memcpy(item->data, p_data, length);

    // 更新队列尾指针和计数(环形缓冲区)
    // 使用模运算实现环形缓冲区
    queue->tail = (queue->tail + 1) % EPD_CMD_QUEUE_SIZE;
    queue->count++;

    NRF_LOG_DEBUG("[EPD]: Command 0x%02x queued at index %d (count=%d)\n",
                  item->cmd, (queue->tail - 1 + EPD_CMD_QUEUE_SIZE) % EPD_CMD_QUEUE_SIZE, queue->count);
    return true;
}

/**@brief 从队列中弹出命令
 *        Pop command from queue
 *
 * @details 从队列头部移除并返回命令。
 *          Removes and returns the command from the head of the queue.
 *
 * @param[in]  queue  命令队列结构指针 / Pointer to command queue structure
 * @param[out] item   存储弹出命令项的指针 / Pointer to store popped command item
 *
 * @return true 如果成功弹出命令 / if command was popped successfully
 * @return false 如果队列为空 / if queue is empty
 */
static bool epd_cmd_queue_pop(epd_cmd_queue_t* queue, epd_cmd_queue_item_t* item) {
    if (queue->count == 0) {
        return false;  // 队列为空 / Queue is empty
    }

    // 从队列头部复制命令
    memcpy(item, &queue->items[queue->head], sizeof(epd_cmd_queue_item_t));
    // 更新队列头指针和计数(环形缓冲区)
    queue->head = (queue->head + 1) % EPD_CMD_QUEUE_SIZE;
    queue->count--;

    NRF_LOG_DEBUG("[EPD]: Command 0x%02x popped from index %d (remaining=%d)\n",
                  item->cmd, (queue->head - 1 + EPD_CMD_QUEUE_SIZE) % EPD_CMD_QUEUE_SIZE, queue->count);
    return true;
}

/**@brief 检查命令是否紧急(需要立即执行)
 *        Check if command is urgent (needs immediate execution)
 *
 * @details 紧急命令绕过队列立即执行。
 *          这些命令是系统关键命令,不应延迟:
 *          - EPD_CMD_SYS_RESET (0x91): 系统复位
 *          - EPD_CMD_SYS_SLEEP (0x92): 进入睡眠模式
 *          - EPD_CMD_CFG_ERASE (0x99): 擦除配置并复位
 *
 *          Urgent commands bypass the queue and are executed immediately.
 *          These include system-critical commands that should not be delayed.
 *
 * @param[in] cmd  命令ID / Command ID
 *
 * @return true 如果命令紧急 / if command is urgent
 * @return false 否则 / otherwise
 */
static bool is_urgent_command(uint8_t cmd) {
    // These commands need immediate execution, bypass queue
    // 这些命令需要立即执行,绕过队列
    return (cmd == EPD_CMD_SYS_RESET ||
            cmd == EPD_CMD_SYS_SLEEP ||
            cmd == EPD_CMD_CFG_ERASE ||
            // CRC传输相关命令需要立即执行以确保ACK及时响应
            // CRC transfer commands need immediate execution for timely ACK response
            cmd == EPD_CMD_WRITE_BLOCK ||
            cmd == EPD_CMD_QUERY_STATUS ||
            cmd == EPD_CMD_RESET_TRANSFER);
}

// 异步命令处理的前向声明
// Forward declaration for async command processing
static void epd_process_next_command(void* p_event_data, uint16_t event_size);

// ============================================================================
// 命令队列管理函数结束
// End of Command Queue Management Functions
// ============================================================================

/**@brief GUI更新函数(由app_scheduler调度)
 *        GUI update function scheduled by app_scheduler
 *
 * @details 此函数处理完整的GUI更新过程,包括:
 *          1. 全刷/局刷决策
 *          2. 电压读取(缓存30分钟)
 *          3. GUI渲染
 *          4. 显示刷新
 *
 *          This function handles the complete GUI update process including:
 *          1. Full refresh vs partial refresh decision
 *          2. Voltage reading (cached for 30 minutes)
 *          3. GUI rendering
 *          4. Display refresh
 *
 * @param[in] p_event_data  epd_gui_update_event_t结构指针
 * @param[in] event_size    事件数据大小
 */
static void epd_gui_update(void* p_event_data, uint16_t event_size) {
    epd_gui_update_event_t* event = (epd_gui_update_event_t*)p_event_data;
    ble_epd_t* p_epd = event->p_epd;

    // Set refreshing flag to prevent BLE disconnect from interrupting refresh
    // 设置刷新标志,防止BLE断开中断刷新过程
    // 当BLE断开时,on_disconnect会检查此标志,如果正在刷新则跳过sleep操作
    p_epd->is_refreshing = true;

    app_feed_wdt();  // Feed watchdog at the beginning. 开始时喂狗
    EPD_GPIO_Init();  // 初始化GPIO / Initialize GPIO
    epd_model_t* epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    EPD_LED_OFF();  // Ensure LED is OFF after driver init, will be turned ON only for full refresh
                    // 确保驱动初始化后LED关闭,只在全刷时才打开

    // ========================================================================
    // 电压读取缓存策略
    // Voltage Reading Cache Strategy
    // ========================================================================
    // 每30分钟读取一次电压以节省电量
    // Read voltage only every 30 minutes to save power
    static uint32_t last_voltage_read = 0;
    static uint16_t cached_voltage_mv = 3300;  // Default: 3.3V = 3300mV. 默认值: 3.3V
    if (last_voltage_read == 0 || (event->timestamp - last_voltage_read) >= 1800) {
        cached_voltage_mv = EPD_ReadVoltageAndCache();
        last_voltage_read = event->timestamp;
    }

    // 准备GUI数据
    // Prepare GUI data
    gui_data_t data = {
        .mode = (display_mode_t)p_epd->config.display_mode,
        .color = epd->color,
        .width = epd->width,
        .height = epd->height,
        .timestamp = event->timestamp,
        .week_start = p_epd->config.week_start,
        .temperature = epd->drv->read_temp(epd),
        .voltage_mv = cached_voltage_mv,
    };

    // 获取BLE设备名称(SSID)
    // Get BLE device name (SSID)
    uint16_t dev_name_len = sizeof(data.ssid);
    uint32_t err_code = sd_ble_gap_device_name_get((uint8_t*)data.ssid, &dev_name_len);
    if (err_code == NRF_SUCCESS && dev_name_len > 0) data.ssid[dev_name_len] = '\0';

    // DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
    // epd->drv->refresh(epd);

    // ========================================================================
    // 刷新策略: 全刷/局刷智能切换
    // Refresh Strategy: Smart Full/Partial Refresh Switching
    // ========================================================================
    //
    // 局刷使用条件:
    // 1. 显示模式为MODE_CLOCK或MODE_CLOCK_CALENDAR
    // 2. 驱动芯片为UC8179(SSD1683也支持但代码中只检查UC8179)
    // 3. 驱动提供了refresh_partial函数
    //
    // 局刷优势:
    // - 刷新速度快(约1秒 vs 全刷的10-15秒)
    // - 功耗低
    // - 闪烁少
    //
    // 局刷劣势:
    // - 长期使用会积累残影
    // - 需要定期全刷来清除残影
    //
    // ========================================================================
    // Use partial refresh for supported ICs in clock mode or clock+calendar mode (UC8179, SSD1683)
    // 在时钟模式或时钟+日历模式下,对支持的芯片使用局部刷新
    bool use_partial = ((data.mode == MODE_CLOCK || data.mode == MODE_CLOCK_CALENDAR) &&
                       epd->drv->ic == EPD_DRIVER_IC_UC8179 &&
                       epd->drv->refresh_partial != NULL);

    if (use_partial) {
        NRF_LOG_INFO("[EPD]: === Using PARTIAL refresh for clock mode ===\n");

        // ====================================================================
        // 全刷触发条件判断
        // Full Refresh Trigger Conditions
        // ====================================================================
        //
        // 1. 首次刷新 (last_full_clear == 0)
        //    - 确保初始显示正确
        //
        // 2. 强制全刷标志 (p_epd->force_full_refresh)
        //    - 模式切换时设置,清除残影
        //
        // 3. 达到全刷间隔且非夜间静默时段
        //    - 定期全刷保证显示质量
        //    - 夜间静默时跳过
        //
        // 全刷间隔配置:
        // - full_refresh_minutes = 0 或 0xFF: 默认每小时(3600秒)
        // - 其他值: full_refresh_minutes * 60秒
        //
        // ====================================================================
        // First time or at configured interval: use full refresh to ensure display is correct
        // 首次或按配置间隔: 使用全刷确保显示正确
        static uint32_t last_full_clear = 0;
        // Get refresh interval in seconds (0 means hourly = 60 minutes)
        // 获取刷新间隔(秒),0表示每小时=60分钟
        uint32_t refresh_interval = (p_epd->config.full_refresh_minutes == 0 || p_epd->config.full_refresh_minutes == 0xFF)
                                    ? 3600  // Default: hourly. 默认: 每小时
                                    : (uint32_t)p_epd->config.full_refresh_minutes * 60;
        // Force full refresh if mode was just switched, first time, or at interval boundary.
        // 如果模式刚切换、首次或到达间隔边界,则强制全刷
        bool at_refresh_boundary = (event->timestamp % refresh_interval == 0);

        // ====================================================================
        // 夜间静默逻辑 (Quiet Hours Logic)
        // ====================================================================
        // 时间范围: 01:00 - 05:59
        // 目的: 减少夜间闪烁和功耗,让用户睡眠时不被打扰
        // 效果: 跳过整点全刷,仅使用局刷
        // 注意: 首次全刷和强制全刷不受夜间静默影响(仍然执行)
        // ====================================================================
        // Quiet hours: 01:00~05:59 使用纯局刷,不做整点全刷以省电和减少闪烁。
        // 只在周期性整点全刷时跳过,首次全刷和强制全刷仍然允许。
        // Quiet hours: 01:00-05:59 use pure partial refresh, skip hourly full refresh
        // to save power and reduce flicker. Only skip periodic full refresh,
        // first-time and forced full refresh are still allowed.
        bool skip_full_in_quiet_hours = false;
        if (at_refresh_boundary) {
            tm_t tm_now = {0};
            transformTime(event->timestamp, &tm_now);
            if (tm_now.tm_hour >= 1 && tm_now.tm_hour <= 5) {  // 01:00-05:59
                skip_full_in_quiet_hours = true;
                NRF_LOG_DEBUG("[EPD]: Quiet hours (01:00-05:59), skipping periodic full refresh\n");
            }
        }

        // 综合判断是否需要全刷
        // Comprehensive decision on whether full refresh is needed
        bool need_full_refresh = p_epd->force_full_refresh ||
                                 (last_full_clear == 0) ||
                                 (at_refresh_boundary && !skip_full_in_quiet_hours);

        if (need_full_refresh) {
            // ================================================================
            // 执行全刷流程
            // Execute Full Refresh Flow
            // ================================================================
            NRF_LOG_INFO("[EPD]: *** Performing FULL refresh (mode switch or periodic) ***\n");

            // LED ON during full refresh
            // 全刷期间打开LED指示
            EPD_LED_ON();

            // Standard full refresh flow to ensure display is correct
            // 标准全刷流程,确保显示正确
            app_feed_wdt();  // Feed watchdog before drawing GUI. 绘制GUI前喂狗
            DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
            app_feed_wdt();  // Feed watchdog after drawing GUI. 绘制GUI后喂狗
            epd->drv->refresh(epd);
            app_feed_wdt();  // Feed watchdog after refresh. 刷新后喂狗

            // LED OFF after full refresh completed
            // 全刷完成后关闭LED
            EPD_LED_OFF();

            last_full_clear = event->timestamp;  // 记录全刷时间 / Record full refresh time
            p_epd->force_full_refresh = false;  // Clear the force flag after full refresh. 清除强制全刷标志
            NRF_LOG_INFO("[EPD]: Full refresh completed\n");
        } else {
            // ================================================================
            // 执行局刷流程
            // Execute Partial Refresh Flow
            // ================================================================
            NRF_LOG_INFO("[EPD]: Using partial refresh (%d min since last full, interval=%d min)\n",
                        (int)(event->timestamp - last_full_clear) / 60,
                        (int)(refresh_interval / 60));

            // Initialize for partial mode (includes reset)
            // 初始化局刷模式(包含复位)
            if (epd->drv->init_partial) {
                epd->drv->init_partial(epd);
            } else {
                NRF_LOG_ERROR("[EPD]: init_partial not available!\n");
                goto fallback_full_refresh;  // 回退到全刷 / Fallback to full refresh
            }
            app_feed_wdt();  // Feed watchdog after init_partial. 局刷初始化后喂狗

            // ================================================================
            // 局刷区域计算 (Partial Refresh Area Calculation)
            // ================================================================
            // For MODE_CLOCK_CALENDAR, only refresh the time display area (minimal partial refresh).
            // The time refresh area is further reduced dynamically to only the minute-ones digit
            // when possible (e.g. 00->01, 01->02, ...). For MODE_CLOCK, refresh the full screen.
            //
            // 对于MODE_CLOCK_CALENDAR模式,只刷新时间显示区域(最小化局刷)。
            // 时间刷新区域可以进一步动态减少到只刷新分钟个位数
            // (例如 00->01, 01->02, ...)。对于MODE_CLOCK模式,刷新整个屏幕。
            // ================================================================
            uint16_t partial_x = 0, partial_y = 0, partial_w = epd->width, partial_h = epd->height;
            if (data.mode == MODE_CLOCK_CALENDAR) {
                // Get the (possibly reduced) time-only refresh area based on current timestamp.
                // 根据当前时间戳获取(可能缩小的)仅时间刷新区域
                GetTimeRefreshArea(data.timestamp, &partial_x, &partial_y, &partial_w, &partial_h);
            }

            // Clear the partial refresh area to white first
            // 首先将局刷区域清为白色
            if (epd->drv->clear_partial) {
                // For MODE_CLOCK_CALENDAR, we skip clear_partial to avoid affecting calendar area
                // 对于MODE_CLOCK_CALENDAR模式,跳过clear_partial以避免影响日历区域
                if (data.mode != MODE_CLOCK_CALENDAR) {
                    epd->drv->clear_partial(epd, false);  // Don't refresh, just clear RAM. 不刷新,只清RAM
                }
            } else {
                NRF_LOG_ERROR("[EPD]: clear_partial not available!\n");
                goto fallback_full_refresh;
            }
            app_feed_wdt();  // Feed watchdog after clear_partial. 清除局刷区域后喂狗

            // Write black data using partial image write
            // 使用局部图像写入写入黑色数据
            NRF_LOG_INFO("[EPD]: Drawing GUI for partial refresh (%d,%d,%d,%d)\n", partial_x, partial_y, partial_w, partial_h);
            if (data.mode == MODE_CLOCK_CALENDAR) {
                // ============================================================
                // MODE_CLOCK_CALENDAR模式的最小化刷新
                // Minimal refresh for MODE_CLOCK_CALENDAR mode
                // ============================================================
                // Draw only the time portion for minimal partial refresh.
                // The local GFX buffer origin corresponds to the TIME_REFRESH origin,
                // and GetTimeRefreshArea returns a sub-window inside this region.
                //
                // 只绘制时间部分进行最小化局刷。
                // 本地GFX缓冲区原点对应TIME_REFRESH原点,
                // GetTimeRefreshArea返回该区域内的子窗口。
                // ============================================================
                uint16_t time_base_x = 0, time_base_y = 0;
                GetTimeRefreshOrigin(&time_base_x, &time_base_y);
                partial_write_ctx_t write_ctx = {
                    .epd = epd,
                    .offset_x = time_base_x,
                    .offset_y = time_base_y
                };
                DrawGUI_ClockOnly(&data, (buffer_callback)write_image_partial_with_offset, &write_ctx);
            } else {
                // MODE_CLOCK模式: 刷新整个屏幕
                // MODE_CLOCK mode: refresh full screen
                DrawGUI(&data, (buffer_callback)epd->drv->write_image_partial, epd);
            }
            app_feed_wdt();  // Feed watchdog after drawing GUI. 绘制GUI后喂狗

            // Load external LUT and perform partial refresh
            // 加载外部LUT并执行局刷
            NRF_LOG_INFO("[EPD]: Performing partial refresh (%d,%d,%d,%d)\n", partial_x, partial_y, partial_w, partial_h);
            if (epd->drv->refresh_partial) {
                epd->drv->refresh_partial(epd, partial_x, partial_y, partial_w, partial_h);
            } else {
                NRF_LOG_ERROR("[EPD]: refresh_partial not available!\n");
            }
            app_feed_wdt();  // Feed watchdog after refresh_partial. 局刷后喂狗
        }

        NRF_LOG_INFO("[EPD]: === Partial refresh sequence complete ===\n");
    } else {
fallback_full_refresh:
        // ====================================================================
        // 其他模式使用全刷
        // Use normal full refresh for other modes or displays
        // ====================================================================
        // 适用于:
        // - MODE_PICTURE: 图片模式
        // - MODE_CALENDAR: 日历模式
        // - 不支持局刷的驱动芯片
        // ====================================================================
        NRF_LOG_INFO("[EPD]: === Using FULL refresh ===\n");

        // LED ON during full refresh
        // 全刷期间打开LED指示
        EPD_LED_ON();

        app_feed_wdt();  // Feed watchdog before drawing GUI. 绘制GUI前喂狗
        DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
        app_feed_wdt();  // Feed watchdog after drawing GUI. 绘制GUI后喂狗
        epd->drv->refresh(epd);
        app_feed_wdt();  // Feed watchdog after refresh. 刷新后喂狗

        // LED OFF after full refresh completed
        // 全刷完成后关闭LED
        EPD_LED_OFF();

        NRF_LOG_INFO("[EPD]: === Full refresh complete ===\n");
    }

    // ========================================================================
    // 刷新后处理
    // Post-refresh handling
    // ========================================================================
    // If BLE disconnected during refresh, put EPD to sleep now
    // (on_disconnect skipped this to avoid interrupting refresh)
    // 如果刷新期间BLE断开,现在将EPD置于睡眠状态
    // (on_disconnect跳过了此操作以避免中断刷新)
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) {
        NRF_LOG_INFO("[EPD]: Refresh complete, BLE disconnected - putting EPD to sleep\n");
        epd->drv->sleep(epd);
        nrf_delay_ms(200);  // for sleep. 等待睡眠生效
    }

    EPD_GPIO_Uninit();  // 释放GPIO / Release GPIO

    // Clear refreshing flag after refresh is complete
    // 刷新完成后清除刷新标志
    p_epd->is_refreshing = false;

    app_feed_wdt();  // 最后喂狗 / Final watchdog feed
}

/**@brief BLE连接事件处理函数
 *        Function for handling the @ref BLE_GAP_EVT_CONNECTED event from the S110 SoftDevice.
 *
 * @details 处理BLE连接事件:
 *          1. 保存连接句柄
 *          2. 初始化GPIO
 *          3. 闪烁LED一次表示连接成功
 *
 * @param[in] p_epd     EPD服务结构 / EPD Service structure
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针 / Pointer to the event received from BLE stack
 */
static void on_connect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    p_epd->conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
    EPD_GPIO_Init();
    // Blink LED once on BLE connect
    // BLE连接时闪烁LED一次
    EPD_LED_BLINK();
}

/**@brief BLE断开事件处理函数
 *        Function for handling the @ref BLE_GAP_EVT_DISCONNECTED event from the S110 SoftDevice.
 *
 * @details 处理BLE断开事件:
 *          1. 重置连接句柄
 *          2. 如果正在刷新,跳过sleep和GPIO_Uninit(由epd_gui_update处理)
 *          3. 否则将EPD置于睡眠状态并释放GPIO
 *
 * @param[in] p_epd     EPD服务结构 / EPD Service structure
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针 / Pointer to the event received from BLE stack
 */
static void on_disconnect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    UNUSED_PARAMETER(p_ble_evt);
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;

    // If screen refresh is in progress, skip sleep/GPIO_Uninit to avoid interrupting refresh
    // The epd_gui_update function will handle GPIO_Uninit after refresh completes
    // 如果屏幕刷新正在进行,跳过sleep/GPIO_Uninit以避免中断刷新
    // epd_gui_update函数将在刷新完成后处理GPIO_Uninit
    if (p_epd->is_refreshing) {
        NRF_LOG_INFO("[EPD]: BLE disconnected during refresh, skipping sleep/GPIO_Uninit\n");
        app_feed_wdt();
        return;
    }

    // Blink LED once on BLE disconnect
    // BLE断开时闪烁LED一次
    EPD_LED_BLINK();

    // Only put EPD to sleep if it has been initialized
    // 只有在EPD已初始化时才将其置于睡眠状态
    if (p_epd->epd != NULL) {
        p_epd->epd->drv->sleep(p_epd->epd);
        nrf_delay_ms(200);  // for sleep. 等待睡眠生效
        EPD_GPIO_Uninit();
    }
    app_feed_wdt();  // Feed watchdog after disconnect operations. 断开操作后喂狗
}

/**@brief 更新显示模式(内部函数)
 *        Update display mode (internal function)
 *
 * @details 如果模式发生变化:
 *          1. 更新配置中的显示模式
 *          2. 写入配置到Flash
 *          3. 设置强制全刷标志,防止模式切换时残影
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 * @param[in] mode   新的显示模式 / New display mode
 */
static void epd_update_display_mode(ble_epd_t* p_epd, display_mode_t mode) {
    if (p_epd->config.display_mode != mode) {
        p_epd->config.display_mode = mode;
        epd_config_write(&p_epd->config);

        // When mode changes, force full refresh on next GUI update
        // This prevents residual data when switching between modes
        // 模式变化时,下次GUI更新时强制全刷
        // 这防止了模式切换时的残影
        p_epd->force_full_refresh = true;
    }
}

/**@brief 更新显示模式并可选触发GUI更新
 *        Update display mode and optionally trigger GUI update
 *
 * @details 更新显示模式,并可选择自动触发GUI更新。
 *          当切换到时钟或时钟+日历模式时,可以立即触发全刷以清除残影。
 *
 * @param[in] p_epd        EPD服务结构指针 / Pointer to EPD Service structure
 * @param[in] mode         新的显示模式 / New display mode
 * @param[in] auto_refresh 如果为true,切换到时钟模式时自动触发GUI更新 /
 *                         if true, automatically trigger GUI update when switching to clock mode
 */
static void epd_update_display_mode_with_refresh(ble_epd_t* p_epd, display_mode_t mode, bool auto_refresh) {
    display_mode_t old_mode = (display_mode_t)p_epd->config.display_mode;

    epd_update_display_mode(p_epd, mode);

    // When switching TO clock or clock_calendar mode, optionally trigger a full refresh immediately
    // to clear any residual image data from the previous mode
    // 当切换到时钟或时钟+日历模式时,可选择立即触发全刷以清除之前模式的残影
    if (auto_refresh && (mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR) && old_mode != mode) {
        NRF_LOG_INFO("[EPD]: Switching to %s mode, triggering full refresh to clear residual data\n",
                    mode == MODE_CLOCK ? "CLOCK" : "CLOCK_CALENDAR");
        ble_epd_on_timer(p_epd, timestamp(), true);
    }
}

/**@brief 从时钟模式切换到图片模式
 *        Switch from clock/clock_calendar mode to picture mode and ensure display is ready
 *
 * @details 当从时钟模式切换到图片模式时,需要重新初始化显示屏。
 *          这是因为时钟模式使用局刷,而图片模式需要全刷。
 *          局刷状态会干扰直接写入RAM的操作。
 *
 *          When switching from clock mode to picture mode, the display needs to be reinitialized.
 *          This is because clock mode uses partial refresh, while picture mode needs full refresh.
 *          The partial mode state interferes with direct write_ram operations.
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
static void epd_switch_from_clock_mode(ble_epd_t* p_epd) {
    if (p_epd->epd == NULL) return;  // EPD not initialized yet. EPD尚未初始化

    if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
        // Reinitialize the display to ensure it's in normal full-refresh mode
        // This prevents partial mode state from interfering with direct write operations
        // 重新初始化显示屏以确保处于正常全刷模式
        // 这防止局刷状态干扰直接写入操作
        if (p_epd->epd->drv->init) {
            p_epd->epd->drv->init(p_epd->epd);
            nrf_delay_ms(10);  // Give display time to reinitialize. 给显示屏时间重新初始化
        }
    }
}

/**@brief 发送当前时间到BLE对端
 *        Send current time to BLE peer
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
static void epd_send_time(ble_epd_t* p_epd) {
    char buf[20] = {0};
    snprintf(buf, 20, "t=%" PRIu32, timestamp());
    ble_epd_string_send(p_epd, (uint8_t*)buf, strlen(buf));
}

/**@brief 发送MTU大小到BLE对端
 *        Send MTU size to BLE peer
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
static void epd_send_mtu(ble_epd_t* p_epd) {
    char buf[10] = {0};
    snprintf(buf, sizeof(buf), "mtu=%d", p_epd->max_data_len);
    ble_epd_string_send(p_epd, (uint8_t*)buf, strlen(buf));
}

/**@brief 执行单个命令(由队列处理器调用)
 *        Execute a single command (called by queue processor)
 *
 * @details 此函数是所有EPD命令的核心处理函数。
 *          它根据命令ID执行相应的操作,包括:
 *          - 引脚配置 (EPD_CMD_SET_PINS)
 *          - 初始化 (EPD_CMD_INIT)
 *          - 清屏 (EPD_CMD_CLEAR)
 *          - 发送命令/数据 (EPD_CMD_SEND_COMMAND/DATA)
 *          - 刷新 (EPD_CMD_REFRESH)
 *          - 睡眠 (EPD_CMD_SLEEP)
 *          - 设置时间 (EPD_CMD_SET_TIME)
 *          - 设置模式 (EPD_CMD_SET_MODE)
 *          - 写入图像 (EPD_CMD_WRITE_IMAGE)
 *          - 系统控制 (EPD_CMD_SYS_RESET/SLEEP/CFG_ERASE)
 *
 * @param[in] p_epd   EPD服务结构指针 / Pointer to EPD service structure
 * @param[in] p_data  命令数据指针 / Pointer to command data
 * @param[in] length  命令数据长度 / Length of command data
 */
static void epd_execute_command(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    NRF_LOG_DEBUG("[EPD]: Executing command 0x%02x, LEN=%d\n", p_data[0], length);
    NRF_LOG_HEXDUMP_DEBUG(p_data, length);
    if (p_data == NULL || length <= 0) return;

    // Feed watchdog before command execution
    // 命令执行前喂狗
    app_feed_wdt();

    switch (p_data[0]) {
        // ====================================================================
        // 引脚配置命令 (0x00)
        // Pin Configuration Command
        // ====================================================================
        case EPD_CMD_SET_PINS:
            if (length < 8) return;
            // 设置各个引脚 / Set each pin
            p_epd->config.mosi_pin = p_data[1];   // MOSI引脚
            p_epd->config.sclk_pin = p_data[2];   // SCLK引脚
            p_epd->config.cs_pin = p_data[3];     // CS片选引脚
            p_epd->config.dc_pin = p_data[4];     // DC数据/命令引脚
            p_epd->config.rst_pin = p_data[5];    // RST复位引脚
            p_epd->config.busy_pin = p_data[6];   // BUSY忙信号引脚
            p_epd->config.bs_pin = p_data[7];     // BS引脚
            if (length > 8) p_epd->config.en_pin = p_data[8];  // EN使能引脚(可选)
            epd_config_write(&p_epd->config);

            // 重新初始化GPIO / Reinitialize GPIO
            EPD_GPIO_Uninit();
            EPD_GPIO_Load(&p_epd->config);
            EPD_GPIO_Init();
            app_feed_wdt();  // Feed watchdog after GPIO operations. GPIO操作后喂狗
            break;

        // ====================================================================
        // EPD初始化命令 (0x01)
        // EPD Initialization Command
        // ====================================================================
        case EPD_CMD_INIT:
            // 初始化EPD驱动,可指定型号或使用配置中的型号
            // Initialize EPD driver, can specify model or use config model
            p_epd->epd = epd_init((epd_model_id_t)(length > 1 ? p_data[1] : p_epd->config.model_id));
            if (p_epd->epd->id != p_epd->config.model_id) {
                p_epd->config.model_id = p_epd->epd->id;
                epd_config_write(&p_epd->config);
            }
            // 发送MTU和时间信息给对端 / Send MTU and time info to peer
            epd_send_mtu(p_epd);
            epd_send_time(p_epd);
            app_feed_wdt();  // Feed watchdog after EPD init. EPD初始化后喂狗
            break;

        // ====================================================================
        // 清屏命令 (0x02)
        // Clear Screen Command
        // ====================================================================
        case EPD_CMD_CLEAR:
            if (p_epd->epd == NULL) return;  // EPD not initialized. EPD未初始化
            // 如果在时钟模式,需要先切换到图片模式
            // If in clock mode, need to switch to picture mode first
            if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("[EPD]: Switching from clock mode to PICTURE mode for clear\n");
                epd_switch_from_clock_mode(p_epd);
            }
            epd_update_display_mode(p_epd, MODE_PICTURE);
            p_epd->epd->drv->clear(p_epd->epd, length > 1 ? p_data[1] : true);
            app_feed_wdt();  // Feed watchdog after clear. 清屏后喂狗
            break;

        // ====================================================================
        // 发送命令到EPD (0x03)
        // Send Command to EPD
        // ====================================================================
        case EPD_CMD_SEND_COMMAND:
            if (length < 2) return;
            // In clock mode, switch to picture mode for direct command operation
            // 在时钟模式下,切换到图片模式进行直接命令操作
            if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("[EPD]: Switching from clock mode to PICTURE mode for send_command\n");
                epd_switch_from_clock_mode(p_epd);
                epd_update_display_mode(p_epd, MODE_PICTURE);
            }
            EPD_WriteCmd(p_data[1]);
            app_feed_wdt();  // Feed watchdog after send command. 发送命令后喂狗
            break;

        // ====================================================================
        // 发送数据到EPD (0x04)
        // Send Data to EPD
        // ====================================================================
        case EPD_CMD_SEND_DATA:
            // In clock mode, switch to picture mode for direct data operation
            // 在时钟模式下,切换到图片模式进行直接数据操作
            if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("[EPD]: Switching from clock mode to PICTURE mode for send_data\n");
                epd_switch_from_clock_mode(p_epd);
                epd_update_display_mode(p_epd, MODE_PICTURE);
            }
            EPD_WriteData(&p_data[1], length - 1);
            app_feed_wdt();  // Feed watchdog after send data. 发送数据后喂狗
            break;

        // ====================================================================
        // 刷新显示命令 (0x05)
        // Refresh Display Command
        // ====================================================================
        case EPD_CMD_REFRESH:
            if (p_epd->epd == NULL) return;  // EPD not initialized. EPD未初始化
            epd_update_display_mode(p_epd, MODE_PICTURE);
            p_epd->epd->drv->refresh(p_epd->epd);
            app_feed_wdt();  // Feed watchdog after refresh. 刷新后喂狗
            break;

        // ====================================================================
        // EPD睡眠命令 (0x06)
        // EPD Sleep Command
        // ====================================================================
        case EPD_CMD_SLEEP:
            if (p_epd->epd == NULL) return;  // EPD not initialized. EPD未初始化
            p_epd->epd->drv->sleep(p_epd->epd);
            app_feed_wdt();  // Feed watchdog after sleep. 睡眠后喂狗
            break;

        // ====================================================================
        // 设置时间命令 (0x20)
        // Set Time Command
        // ====================================================================
        case EPD_CMD_SET_TIME: {
            if (length < 5) return;

            NRF_LOG_DEBUG("time: %02x %02x %02x %02x\n", p_data[1], p_data[2], p_data[3], p_data[4]);
            if (length > 5) NRF_LOG_DEBUG("timezone: %d\n", (int8_t)p_data[5]);

            // 解析时间戳 (大端序)
            // Parse timestamp (big-endian)
            uint32_t timestamp = (p_data[1] << 24) | (p_data[2] << 16) | (p_data[3] << 8) | p_data[4];
            // 添加时区偏移(默认+8小时,即北京时间)
            // Add timezone offset (default +8 hours, i.e. Beijing time)
            timestamp += (length > 5 ? (int8_t)p_data[5] : 8) * 60 * 60;
            set_timestamp(timestamp);

            // Only change mode if explicitly specified in command, otherwise keep current mode
            // 只有命令中明确指定时才更改模式,否则保持当前模式
            if (length > 6) {
                // Note: auto_refresh=false because ble_epd_on_timer is called below
                // 注意: auto_refresh=false因为下面会调用ble_epd_on_timer
                epd_update_display_mode_with_refresh(p_epd, (display_mode_t)p_data[6], false);
            }
            // Always force full refresh when receiving SET_TIME command
            // This ensures display is fully updated with new time/mode
            // 收到SET_TIME命令时总是强制全刷
            // 这确保显示完全更新为新的时间/模式
            p_epd->force_full_refresh = true;
            ble_epd_on_timer(p_epd, timestamp, true);
            app_feed_wdt();  // Feed watchdog after set time. 设置时间后喂狗
        } break;

        // ====================================================================
        // 设置周起始日命令 (0x21)
        // Set Week Start Day Command
        // ====================================================================
        case EPD_CMD_SET_WEEK_START:
            if (length < 2) return;
            // 0: Sunday, 1: Monday, ... 6: Saturday
            // 0: 周日, 1: 周一, ... 6: 周六
            if (p_data[1] < 7 && p_data[1] != p_epd->config.week_start) {
                p_epd->config.week_start = p_data[1];
                epd_config_write(&p_epd->config);
            }
            app_feed_wdt();  // Feed watchdog after set week start. 设置周起始日后喂狗
            break;

        // ====================================================================
        // 设置显示模式命令 (0x22)
        // Set Display Mode Command
        // ====================================================================
        // 模式切换处理流程:
        // Mode switching process:
        // 1. 检查新模式是否有效
        // 2. 如果从时钟模式切换出去,重新初始化显示屏
        // 3. 设置强制全刷标志
        // 4. 更新模式并触发GUI更新
        // ====================================================================
        case EPD_CMD_SET_MODE:
            if (length < 2) return;
            if (p_data[1] <= MODE_CLOCK) {  // MODE_CLOCK is now max value (3)
                display_mode_t new_mode = (display_mode_t)p_data[1];
                NRF_LOG_INFO("[EPD]: CMD_SET_MODE: switching to mode %d\n", new_mode);

                // If switching from clock modes, reinitialize display
                // 如果从时钟模式切换,重新初始化显示屏
                if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                    if (new_mode != MODE_CLOCK && new_mode != MODE_CLOCK_CALENDAR) {
                        epd_switch_from_clock_mode(p_epd);
                    }
                }

                // Update mode and force full refresh
                // 更新模式并强制全刷
                p_epd->force_full_refresh = true;
                epd_update_display_mode(p_epd, new_mode);

                // Trigger immediate GUI update with current time
                // 使用当前时间立即触发GUI更新
                ble_epd_on_timer(p_epd, timestamp(), true);
            }
            app_feed_wdt();  // Feed watchdog after set mode. 设置模式后喂狗
            break;

        // ====================================================================
        // 写入图像命令 (0x30)
        // Write Image Command
        // ====================================================================
        // MSB=0000: ram begin, LSB=1111: black
        // 用于将图像数据写入EPD RAM
        case EPD_CMD_WRITE_IMAGE:
            if (length < 3) return;
            if (p_epd->epd == NULL) return;  // EPD not initialized. EPD未初始化
            // In clock mode, switch to picture mode and reinitialize for normal write_ram operation
            // Clock mode uses partial refresh which is incompatible with direct write_ram
            // 在时钟模式下,切换到图片模式并重新初始化以进行正常的write_ram操作
            // 时钟模式使用局刷,与直接write_ram不兼容
            if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("[EPD]: Switching from clock mode to PICTURE mode for write_image\n");
                epd_switch_from_clock_mode(p_epd);
            }
            epd_update_display_mode(p_epd, MODE_PICTURE);  // Officially update mode. 正式更新模式
            p_epd->epd->drv->write_ram(p_epd->epd, p_data[1], &p_data[2], length - 2);
            app_feed_wdt();  // Feed watchdog after write image. 写入图像后喂狗
            break;

        // ====================================================================
        // 带CRC校验的分块写入命令 (0x31)
        // Write Block with CRC16 Checksum Command
        // ====================================================================
        // 数据格式: [cmd(1)][block_id(2)][total_blocks(2)][cfg(1)][payload(N)][crc16(2)]
        // Data format: [cmd(1)][block_id(2)][total_blocks(2)][cfg(1)][payload(N)][crc16(2)]
        // cfg字节: 低4位=图层(0x0F=黑白层, 0x00=颜色层), 高4位=是否首块(0x00=首块, 0xF0=续块)
        // cfg byte: low nibble=layer(0x0F=BW, 0x00=color), high nibble=first flag(0x00=first, 0xF0=continue)
        case EPD_CMD_WRITE_BLOCK:
            if (length < 8) return;  // 最小长度: cmd(1) + block_id(2) + total(2) + cfg(1) + crc(2)
            if (p_epd->epd == NULL) return;
            {
                // 解析数据包
                uint16_t block_id = p_data[1] | (p_data[2] << 8);
                uint16_t total_blocks = p_data[3] | (p_data[4] << 8);
                uint8_t cfg = p_data[5];  // 图层+首块标志
                uint16_t payload_len = length - 8;  // 减去协议头尾
                uint8_t* payload = &p_data[6];
                uint16_t recv_crc = p_data[length - 2] | (p_data[length - 1] << 8);
                
                // 计算CRC并验证 (只校验payload部分)
                uint16_t calc_crc = crc16_compute(payload, payload_len);
                
                if (calc_crc == recv_crc) {
                    // CRC正确
                    
                    // 1. 检查block_id合法性
                    if (block_id >= EPD_MAX_BLOCKS || block_id >= total_blocks) {
                        NRF_LOG_WARNING("[EPD]: Block %d invalid (max=%d, total=%d)\n",
                                       block_id, EPD_MAX_BLOCKS, total_blocks);
                        send_block_response(p_epd, block_id, 0x02);  // NACK - invalid block ID
                        break;
                    }
                    
                    // 2. 检查total_blocks合法性
                    if (total_blocks == 0 || total_blocks > EPD_MAX_BLOCKS) {
                        NRF_LOG_WARNING("[EPD]: Invalid total_blocks=%d\n", total_blocks);
                        send_block_response(p_epd, block_id, 0x02);  // NACK - invalid total
                        break;
                    }
                    
                    // 初始化或更新传输上下文
                    if (block_id == 0 || !p_epd->transfer_ctx.transfer_active) {
                        // 新传输或第一个块
                        p_epd->transfer_ctx.total_blocks = total_blocks;
                        p_epd->transfer_ctx.received_blocks = 0;
                        memset(p_epd->transfer_ctx.block_bitmap, 0, EPD_BLOCK_BITMAP_SIZE);
                        p_epd->transfer_ctx.transfer_active = true;
                        
                        // 如果在时钟模式，需先切换到图片模式
                        if (p_epd->config.display_mode == MODE_CLOCK || 
                            p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                            NRF_LOG_INFO("[EPD]: Switching from clock mode to PICTURE mode for write_block\n");
                            epd_switch_from_clock_mode(p_epd);
                        }
                        epd_update_display_mode(p_epd, MODE_PICTURE);
                    } else if (p_epd->transfer_ctx.total_blocks != total_blocks) {
                        // 3. 检测total_blocks在传输中途变化(可能是APP端bug或新传输)
                        NRF_LOG_WARNING("[EPD]: total_blocks changed mid-transfer: %d -> %d, resetting\n",
                                       p_epd->transfer_ctx.total_blocks, total_blocks);
                        // 重置传输上下文
                        p_epd->transfer_ctx.total_blocks = total_blocks;
                        p_epd->transfer_ctx.received_blocks = 0;
                        memset(p_epd->transfer_ctx.block_bitmap, 0, EPD_BLOCK_BITMAP_SIZE);
                    }
                    
                    // 检查是否重复块(断点续传时可能重发)
                    uint16_t byte_idx = block_id / 8;
                    uint8_t bit_idx = block_id % 8;
                    if (byte_idx < EPD_BLOCK_BITMAP_SIZE && 
                        !(p_epd->transfer_ctx.block_bitmap[byte_idx] & (1 << bit_idx))) {
                        // 新块: 写入EPD RAM
                        // 使用APP传入的cfg参数，包含图层信息和首块标志
                        p_epd->epd->drv->write_ram(p_epd->epd, cfg, payload, payload_len);
                        
                        // 标记块已接收
                        p_epd->transfer_ctx.block_bitmap[byte_idx] |= (1 << bit_idx);
                        p_epd->transfer_ctx.received_blocks++;
                        
                        NRF_LOG_DEBUG("[EPD]: Block %d/%d received, cfg=0x%02X, CRC OK\n", 
                                     block_id, total_blocks, cfg);
                    } else {
                        NRF_LOG_DEBUG("[EPD]: Block %d already received (duplicate)\n", block_id);
                    }
                    
                    // 发送ACK
                    send_block_response(p_epd, block_id, 0x00);
                } else {
                    // CRC错误，发送NACK请求重传
                    NRF_LOG_WARNING("[EPD]: Block %d CRC mismatch! recv=0x%04X, calc=0x%04X\n",
                                   block_id, recv_crc, calc_crc);
                    send_block_response(p_epd, block_id, 0x01);
                }
            }
            app_feed_wdt();
            break;

        // ====================================================================
        // 查询传输状态命令 (0x32) - 用于断点续传
        // Query Transfer Status Command - for Resume Upload
        // ====================================================================
        case EPD_CMD_QUERY_STATUS:
            send_transfer_status(p_epd);
            break;

        // ====================================================================
        // 重置传输状态命令 (0x33)
        // Reset Transfer State Command
        // ====================================================================
        case EPD_CMD_RESET_TRANSFER:
            memset(&p_epd->transfer_ctx, 0, sizeof(image_transfer_ctx_t));
            if (length > 1) {
                p_epd->transfer_ctx.session_id = p_data[1];
            }
            p_epd->transfer_ctx.transfer_active = false;
            NRF_LOG_INFO("[EPD]: Transfer state reset, session_id=%d\n", p_epd->transfer_ctx.session_id);
            break;

        // ====================================================================
        // 设置配置命令 (0x90)
        // Set Configuration Command
        // ====================================================================
        case EPD_CMD_SET_CONFIG:
            if (length < 2) return;
            {
                display_mode_t old_mode = (display_mode_t)p_epd->config.display_mode;
                memcpy(&p_epd->config, &p_data[1], (length - 1 > EPD_CONFIG_SIZE) ? EPD_CONFIG_SIZE : length - 1);
                epd_config_write(&p_epd->config);

                // If display mode changed, update LED control and handle mode transitions
                // 如果显示模式改变,更新LED控制并处理模式转换
                if (p_epd->config.display_mode != old_mode) {
                    NRF_LOG_INFO("[EPD]: display_mode changed via SET_CONFIG: %d -> %d\n", old_mode, p_epd->config.display_mode);

                    // If switched to CLOCK or CLOCK_CALENDAR mode, force full refresh to clear residual data
                    // 如果切换到时钟或时钟+日历模式,强制全刷以清除残影
                    if ((p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) &&
                        old_mode != p_epd->config.display_mode) {
                        p_epd->force_full_refresh = true;
                        ble_epd_on_timer(p_epd, timestamp(), true);
                    }
                }
            }
            app_feed_wdt();  // Feed watchdog after set config. 设置配置后喂狗
            break;

        // ====================================================================
        // 系统睡眠命令 (0x92) - 紧急命令
        // System Sleep Command - Urgent Command
        // ====================================================================
        case EPD_CMD_SYS_SLEEP:
            sleep_mode_enter();
            app_feed_wdt();  // Feed watchdog after sleep (may not execute). 睡眠后喂狗(可能不会执行)
            break;

        // ====================================================================
        // 系统复位命令 (0x91) - 紧急命令
        // System Reset Command - Urgent Command
        // ====================================================================
        case EPD_CMD_SYS_RESET:
            nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_RESET);
            break;

        // ====================================================================
        // 擦除配置命令 (0x99) - 紧急命令
        // Erase Configuration Command - Urgent Command
        // ====================================================================
        case EPD_CMD_CFG_ERASE:
            epd_config_clear(&p_epd->config);
            nrf_delay_ms(100);  // required. 必需的延时
            NVIC_SystemReset();
            break;

        default:
            break;
    }

    // Feed watchdog after command execution
    // 命令执行后喂狗
    app_feed_wdt();
    NRF_LOG_DEBUG("[EPD]: Command 0x%02x execution completed\n", p_data[0]);
}

/**@brief 处理队列中的下一个命令(由app_scheduler调度)
 *        Process next command from queue (scheduled by app_scheduler)
 *
 * @details 此函数是命令队列处理的核心:
 *          1. 从队列中弹出一个命令
 *          2. 执行该命令
 *          3. 如果队列还有命令,调度下一次处理
 *          4. 队列为空时标记处理完成
 *
 *          This function is the core of command queue processing:
 *          1. Pop one command from queue
 *          2. Execute the command
 *          3. If queue has more commands, schedule next processing
 *          4. Mark processing complete when queue is empty
 *
 * @param[in] p_event_data  EPD服务结构指针的指针(复制的数据)
 * @param[in] event_size    事件数据大小
 */
static void epd_process_next_command(void* p_event_data, uint16_t event_size) {
    // Extract the pointer from the copied data
    // 从复制的数据中提取指针
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    epd_cmd_queue_item_t cmd_item;

    // Mark as processing
    // 标记正在处理
    p_epd->cmd_queue.is_processing = true;

    // Pop one command from queue
    // 从队列中弹出一个命令
    if (epd_cmd_queue_pop(&p_epd->cmd_queue, &cmd_item)) {
        NRF_LOG_INFO("[EPD]: Processing queued command 0x%02x (remaining=%d)\n",
                     cmd_item.cmd, p_epd->cmd_queue.count);

        // Execute the command
        // 执行命令
        epd_execute_command(p_epd, cmd_item.data, cmd_item.length);

        // Feed watchdog between commands
        // 命令间喂狗
        app_feed_wdt();

        // If queue still has commands, schedule next one
        // 如果队列还有命令,调度下一个
        if (p_epd->cmd_queue.count > 0) {
            NRF_LOG_DEBUG("[EPD]: Scheduling next command (queue count=%d)\n", p_epd->cmd_queue.count);
            app_sched_event_put(&p_epd, sizeof(ble_epd_t*), epd_process_next_command);
        } else {
            // Queue is empty, mark processing complete
            // 队列为空,标记处理完成
            p_epd->cmd_queue.is_processing = false;
            NRF_LOG_INFO("[EPD]: Command queue empty, all commands processed\n");
        }
    } else {
        // Queue is empty
        // 队列为空
        p_epd->cmd_queue.is_processing = false;
        NRF_LOG_DEBUG("[EPD]: Queue empty in process_next_command\n");
    }
}

/**@brief 服务写入处理包装函数(将命令入队或执行)
 *        Service write handler wrapper (queues or executes commands)
 *
 * @details 此函数是BLE写入的入口点:
 *          1. 紧急命令绕过队列立即执行
 *          2. 普通命令添加到队列
 *          3. 如果队列未在处理,启动队列处理
 *
 *          This function is the entry point for BLE writes:
 *          1. Urgent commands bypass queue and execute immediately
 *          2. Normal commands are added to queue
 *          3. If queue is not processing, start queue processing
 *
 * @param[in] p_epd   EPD服务结构指针 / Pointer to EPD service structure
 * @param[in] p_data  命令数据指针 / Pointer to command data
 * @param[in] length  命令数据长度 / Length of command data
 */
static void epd_service_on_write(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    NRF_LOG_DEBUG("[EPD]: on_write received command 0x%02x, LEN=%d\n",
                  p_data ? p_data[0] : 0, length);
    if (p_data == NULL || length <= 0) return;

    // Urgent commands bypass queue and execute immediately
    // 紧急命令绕过队列立即执行
    if (is_urgent_command(p_data[0])) {
        NRF_LOG_INFO("[EPD]: Urgent command 0x%02x, executing immediately (bypassing queue)\n", p_data[0]);
        epd_execute_command(p_epd, p_data, length);
        return;
    }

    // Normal commands: add to queue
    // 普通命令:添加到队列
    if (epd_cmd_queue_push(&p_epd->cmd_queue, p_data, length)) {
        NRF_LOG_DEBUG("[EPD]: Command 0x%02x added to queue (count=%d, processing=%d)\n",
                      p_data[0], p_epd->cmd_queue.count, p_epd->cmd_queue.is_processing);

        // If no command is currently being processed, start processing
        // 如果当前没有命令正在处理,开始处理
        if (!p_epd->cmd_queue.is_processing) {
            NRF_LOG_DEBUG("[EPD]: Starting command queue processing\n");
            app_sched_event_put(&p_epd, sizeof(ble_epd_t*), epd_process_next_command);
        }
    } else {
        NRF_LOG_ERROR("[EPD]: Failed to queue command 0x%02x (queue full)\n", p_data[0]);
    }
}

/**@brief BLE GATTS写入事件处理函数
 *        Function for handling the @ref BLE_GATTS_EVT_WRITE event from the S110 SoftDevice.
 *
 * @details 处理两种类型的写入:
 *          1. CCCD写入:启用/禁用通知,启用时发送配置
 *          2. 特性值写入:调用命令处理函数
 *
 *          Handles two types of writes:
 *          1. CCCD write: enable/disable notifications, send config when enabled
 *          2. Characteristic value write: call command handler
 *
 * @param[in] p_epd     EPD服务结构 / EPD Service structure
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针 / Pointer to the event received from BLE stack
 */
static void on_write(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    ble_gatts_evt_write_t* p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;

    // 检查是否是CCCD写入(通知启用/禁用)
    // Check if this is a CCCD write (notification enable/disable)
    if ((p_evt_write->handle == p_epd->char_handles.cccd_handle) && (p_evt_write->len == 2)) {
        if (ble_srv_is_notification_enabled(p_evt_write->data)) {
            // 通知已启用,发送配置给对端
            // Notification enabled, send config to peer
            NRF_LOG_DEBUG("notification enabled\n");
            p_epd->is_notification_enabled = true;
            static uint16_t length = sizeof(epd_config_t);
            NRF_LOG_DEBUG("send epd config\n");
            uint32_t err_code = ble_epd_string_send(p_epd, (uint8_t*)&p_epd->config, length);
            if (err_code != NRF_ERROR_INVALID_STATE) APP_ERROR_CHECK(err_code);
        } else {
            p_epd->is_notification_enabled = false;
        }
    } else if (p_evt_write->handle == p_epd->char_handles.value_handle) {
        // 特性值写入,处理命令
        // Characteristic value write, handle command
        epd_service_on_write(p_epd, p_evt_write->data, p_evt_write->len);
    } else {
        // Do Nothing. This event is not relevant for this service.
        // 不做任何事。此事件与此服务无关。
    }
}

/**@brief nRF52811的BLE事件处理函数
 *        BLE event handler for nRF52811
 *
 * @param[in] p_ble_evt  BLE事件指针 / Pointer to BLE event
 * @param[in] p_context  上下文指针(EPD服务结构) / Context pointer (EPD service structure)
 */
void ble_epd_evt_handler(ble_evt_t const* p_ble_evt, void* p_context) {
    if (p_context == NULL || p_ble_evt == NULL) return;

    ble_epd_t* p_epd = (ble_epd_t*)p_context;
    ble_epd_on_ble_evt(p_epd, (ble_evt_t*)p_ble_evt);
}

/**@brief BLE事件处理函数
 *        BLE event handler
 *
 * @details 分发BLE事件到相应的处理函数:
 *          - BLE_GAP_EVT_CONNECTED: 连接事件
 *          - BLE_GAP_EVT_DISCONNECTED: 断开事件
 *          - BLE_GATTS_EVT_WRITE: 写入事件
 *
 * @param[in] p_epd     EPD服务结构 / EPD Service structure
 * @param[in] p_ble_evt BLE事件指针 / Pointer to BLE event
 */
void ble_epd_on_ble_evt(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    if ((p_epd == NULL) || (p_ble_evt == NULL)) {
        return;
    }

    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            on_connect(p_epd, p_ble_evt);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            on_disconnect(p_epd, p_ble_evt);
            break;

        case BLE_GATTS_EVT_WRITE:
            on_write(p_epd, p_ble_evt);
            break;

        default:
            // No implementation needed.
            // 无需实现。
            break;
    }
}

/**@brief 初始化BLE服务(内部函数)
 *        Initialize BLE service (internal function)
 *
 * @details 初始化BLE服务,包括:
 *          1. 注册自定义UUID
 *          2. 添加主服务
 *          3. 添加EPD特性(可写、可通知)
 *          4. 添加应用版本特性(只读)
 *
 *          Initialize BLE service, including:
 *          1. Register custom UUID
 *          2. Add primary service
 *          3. Add EPD characteristic (writable, notifiable)
 *          4. Add app version characteristic (read-only)
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
static uint32_t epd_service_init(ble_epd_t* p_epd) {
    ble_uuid_t ble_uuid = {0};
    ble_uuid128_t base_uuid = BLE_UUID_EPD_SVC_BASE;
    ble_add_char_params_t add_char_params;
    uint8_t app_version = APP_VERSION;

    // 注册自定义UUID / Register custom UUID
    VERIFY_SUCCESS(sd_ble_uuid_vs_add(&base_uuid, &ble_uuid.type));

    // 添加主服务 / Add primary service
    ble_uuid.type = ble_uuid.type;
    ble_uuid.uuid = BLE_UUID_EPD_SVC;
    VERIFY_SUCCESS(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid, &p_epd->service_handle));

    // 添加EPD特性(可写、可通知)
    // Add EPD characteristic (writable, notifiable)
    memset(&add_char_params, 0, sizeof(add_char_params));
    add_char_params.uuid = BLE_UUID_EPD_CHAR;
    add_char_params.uuid_type = ble_uuid.type;
    add_char_params.max_len = BLE_EPD_MAX_DATA_LEN;
    add_char_params.init_len = sizeof(uint8_t);
    add_char_params.is_var_len = true;
    add_char_params.char_props.notify = 1;        // 支持通知 / Support notification
    add_char_params.char_props.write = 1;         // 支持写入(带响应) / Support write with response
    add_char_params.char_props.write_wo_resp = 1; // 支持写入(无响应) / Support write without response
    add_char_params.read_access = SEC_OPEN;
    add_char_params.write_access = SEC_OPEN;
    add_char_params.cccd_write_access = SEC_OPEN;

    VERIFY_SUCCESS(characteristic_add(p_epd->service_handle, &add_char_params, &p_epd->char_handles));

    // 添加应用版本特性(只读)
    // Add app version characteristic (read-only)
    memset(&add_char_params, 0, sizeof(add_char_params));
    add_char_params.uuid = BLE_UUID_APP_VER;
    add_char_params.uuid_type = ble_uuid.type;
    add_char_params.max_len = sizeof(uint8_t);
    add_char_params.init_len = sizeof(uint8_t);
    add_char_params.p_init_value = &app_version;
    add_char_params.char_props.read = 1;
    add_char_params.read_access = SEC_OPEN;

    return characteristic_add(p_epd->service_handle, &add_char_params, &p_epd->app_ver_handles);
}

/**@brief 准备睡眠模式
 *        Prepare for sleep mode
 *
 * @details 进入睡眠模式前的准备:
 *          1. 关闭LED
 *          2. 配置唤醒引脚
 *
 *          Preparation before entering sleep mode:
 *          1. Turn off LED
 *          2. Configure wakeup pin
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
void ble_epd_sleep_prepare(ble_epd_t* p_epd) {
    // Turn off led. 关闭LED
    EPD_LED_OFF();
    // Prepare wakeup pin. 准备唤醒引脚
    if (p_epd->config.wakeup_pin != 0xFF) {
        nrf_gpio_cfg_sense_input(p_epd->config.wakeup_pin, NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_HIGH);
    }
}

/**@brief 初始化EPD服务
 *        Initialize EPD Service
 *
 * @details 初始化EPD服务的完整流程:
 *          1. 初始化服务结构
 *          2. 初始化命令队列
 *          3. 加载或创建默认配置
 *          4. 初始化GPIO和EPD驱动
 *          5. 添加BLE服务
 *
 *          Complete initialization process:
 *          1. Initialize service structure
 *          2. Initialize command queue
 *          3. Load or create default config
 *          4. Initialize GPIO and EPD driver
 *          5. Add BLE service
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
uint32_t ble_epd_init(ble_epd_t* p_epd) {
    if (p_epd == NULL) return NRF_ERROR_NULL;

    // Initialize the service structure.
    // 初始化服务结构
    p_epd->max_data_len = BLE_EPD_MAX_DATA_LEN;
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    p_epd->is_notification_enabled = false;
    p_epd->force_full_refresh = false;  // Initialize force refresh flag. 初始化强制刷新标志
    p_epd->is_refreshing = false;       // Initialize refreshing flag. 初始化刷新中标志

    // Initialize command queue
    // 初始化命令队列
    epd_cmd_queue_init(&p_epd->cmd_queue);

    // 初始化并读取配置
    // Initialize and read config
    epd_config_init(&p_epd->config);
    epd_config_read(&p_epd->config);

    // write default config
    // 如果配置为空,写入默认配置
    if (epd_config_empty(&p_epd->config)) {
        uint8_t cfg[] = EPD_CFG_52811;
        memcpy(&p_epd->config, cfg, sizeof(cfg));
        // 设置默认值 / Set default values
        if (p_epd->config.display_mode == 0xFF) p_epd->config.display_mode = MODE_CLOCK_CALENDAR;
        if (p_epd->config.week_start == 0xFF) p_epd->config.week_start = 0;
        if (p_epd->config.full_refresh_minutes == 0xFF) p_epd->config.full_refresh_minutes = 0;  // 0 = hourly (60 min)
        epd_config_write(&p_epd->config);
    }

    // load config
    // 加载配置
    EPD_GPIO_Load(&p_epd->config);

    // Initialize EPD driver based on config
    // 根据配置初始化EPD驱动
    EPD_GPIO_Init();
    p_epd->epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    EPD_GPIO_Uninit();

    // blink LED on start
    // 启动时闪烁LED
    EPD_LED_BLINK();

    // Add the service.
    // 添加BLE服务
    return epd_service_init(p_epd);
}

/**@brief 通过BLE发送字符串
 *        Send a string via BLE
 *
 * @details 将字符串作为通知发送给BLE对端。
 *          Sends the input string as a notification to the BLE peer.
 *
 * @param[in] p_epd     EPD服务结构指针 / Pointer to EPD Service structure
 * @param[in] p_string  要发送的字符串 / String to be sent
 * @param[in] length    字符串长度 / Length of the string
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length) {
    // 检查连接状态和通知是否启用
    // Check connection state and if notification is enabled
    if ((p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) || (!p_epd->is_notification_enabled))
        return NRF_ERROR_INVALID_STATE;
    if (length > p_epd->max_data_len) return NRF_ERROR_INVALID_PARAM;

    ble_gatts_hvx_params_t hvx_params;

    memset(&hvx_params, 0, sizeof(hvx_params));

    hvx_params.handle = p_epd->char_handles.value_handle;
    hvx_params.p_data = p_string;
    hvx_params.p_len = &length;
    hvx_params.type = BLE_GATT_HVX_NOTIFICATION;

    return sd_ble_gatts_hvx(p_epd->conn_handle, &hvx_params);
}

/**@brief 定时器事件处理函数,触发GUI更新
 *        Timer event handler, triggers GUI update
 *
 * @details 定期调用以根据当前模式更新显示:
 *          - MODE_CALENDAR: 每天00:00:00更新 (timestamp % 86400 == 0)
 *          - MODE_CLOCK: 每分钟更新 (timestamp % 60 == 0)
 *          - MODE_CLOCK_CALENDAR: 每分钟更新 (timestamp % 60 == 0)
 *          - force_update为true时强制更新
 *
 *          Called periodically to update the display based on current mode:
 *          - MODE_CALENDAR: Updates daily at 00:00:00
 *          - MODE_CLOCK: Updates every minute
 *          - MODE_CLOCK_CALENDAR: Updates every minute
 *          - Forces update when force_update is true
 *
 * @param[in] p_epd         EPD服务结构指针 / Pointer to EPD Service structure
 * @param[in] timestamp     当前Unix时间戳 / Current Unix timestamp
 * @param[in] force_update  如果为true强制更新 / If true, force update regardless of mode
 */
void ble_epd_on_timer(ble_epd_t* p_epd, uint32_t timestamp, bool force_update) {
    // Update calendar on 00:00:00, clock/clock_calendar on every minute
    // 日历在00:00:00更新,时钟/时钟+日历每分钟更新
    if (force_update || (p_epd->config.display_mode == MODE_CALENDAR && timestamp % 86400 == 0) ||
        (p_epd->config.display_mode == MODE_CLOCK && timestamp % 60 == 0) ||
        (p_epd->config.display_mode == MODE_CLOCK_CALENDAR && timestamp % 60 == 0)) {
        // 通过app_scheduler调度GUI更新
        // Schedule GUI update via app_scheduler
        epd_gui_update_event_t event = {p_epd, timestamp};
        app_sched_event_put(&event, sizeof(epd_gui_update_event_t), epd_gui_update);
    }
}
