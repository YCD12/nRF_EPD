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
 *    - 首次刷新 (p_epd->last_full_clear == 0)
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

#include <stdbool.h>
#include <string.h>

#include "app_scheduler.h"
#include "app_timer.h"
#include "ble_srv_common.h"
#include "main.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_pwr_mgmt.h"
#include "sdk_macros.h"
#include "Lunar.h"
#include "epd_ext_resources.h"
#include "epd_clock_diff.h"
#include "ui_runtime.h"


// ============================================================================
// CRC16计算和传输响应函数
// CRC16 Calculation and Transfer Response Functions
// ============================================================================

#define LOW_BATTERY_POLL_INTERVAL_S 10  // Low-battery mode voltage polling interval in seconds | 低电量模式电压轮询周期(秒)

// 资源更新会话空闲超时(秒)。收到擦除/写入/提交命令后进入会话并暂停周期刷新，
// 超过该时长未收到新的资源命令(视为上传中断)则自动恢复周期刷新。
// Idle timeout (s) for a resource update session. Periodic refresh is paused while a
// session is active; if no new resource command arrives within this window (upload aborted),
// the session auto-expires and periodic refresh resumes.
#define EPD_RES_SESSION_TIMEOUT_S 15U

// 刷新消抖保护窗口(秒)。收到时间同步后，若上一次刷新距离当前周期 Tick 不足该时间，
// 则跳过本次周期刷新，防止校时后连续双重刷新(PRD §1.3 / US 3.2)。
#define EPD_REFRESH_DEBOUNCE_S 45U

// SET_TIME 频率限制(秒)。同一时间窗口内仅允许一次 SET_TIME 触发屏幕更新(PRD §4.3)。
#define EPD_SET_TIME_RATE_LIMIT_S 10U

#include "UC8179.h"



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

static uint32_t epd_parse_u24_le(const uint8_t* data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16);
}

static uint32_t epd_parse_u32_le(const uint8_t* data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void epd_write_u32_le(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)((value >> 8) & 0xFF);
    data[2] = (uint8_t)((value >> 16) & 0xFF);
    data[3] = (uint8_t)((value >> 24) & 0xFF);
}

static ret_code_t epd_resources_ensure_ready(void) {
    ret_code_t ret = epd_ext_resources_init();
    if (ret == NRF_ERROR_INVALID_DATA ||
        ret == NRF_ERROR_INVALID_LENGTH ||
        ret == NRF_ERROR_INVALID_ADDR ||
        ret == NRF_ERROR_NOT_FOUND) {
        return NRF_SUCCESS;
    }
    return ret;
}

static bool epd_resources_manifest_ready(void) {
    epd_ext_resource_status_t status;
    ret_code_t ret = epd_ext_resources_get_status(&status);

    return (ret == NRF_SUCCESS && status.flash_ready && status.manifest_valid);
}

static void send_resource_op_response(ble_epd_t* p_epd, uint8_t cmd, uint16_t seq, ret_code_t ret) {
    uint8_t rsp[8];
    rsp[0] = EPD_RSP_RESOURCE_OP;
    rsp[1] = cmd;
    rsp[2] = (uint8_t)(seq & 0xFF);
    rsp[3] = (uint8_t)(seq >> 8);
    epd_write_u32_le(&rsp[4], (uint32_t)ret);
    ble_epd_string_send(p_epd, rsp, sizeof(rsp));
}

static void send_resource_status(ble_epd_t* p_epd, ret_code_t init_ret) {
    epd_ext_resource_status_t status;
    ret_code_t ret = NRF_SUCCESS;
    uint8_t rsp[24];

    memset(&status, 0, sizeof(status));
    if (init_ret != NRF_ERROR_BUSY) {
        ret = epd_ext_resources_get_status(&status);
        if (ret != NRF_SUCCESS) {
            memset(&status, 0, sizeof(status));
        }
    }

    rsp[0] = EPD_RSP_RESOURCE_STATUS;
    rsp[1] = status.flash_ready ? 1 : 0;
    rsp[2] = status.manifest_valid ? 1 : 0;
    rsp[3] = (uint8_t)(status.resource_count & 0xFF);
    rsp[4] = (uint8_t)(status.resource_count >> 8);
    rsp[5] = status.jedec_id.manufacturer_id;
    rsp[6] = status.jedec_id.memory_type;
    rsp[7] = status.jedec_id.capacity;
    epd_write_u32_le(&rsp[8], status.manifest_crc32);
    epd_write_u32_le(&rsp[12], (ret == NRF_SUCCESS) ? (uint32_t)init_ret : (uint32_t)ret);
    rsp[16] = (uint8_t)((status.flash_total_bytes / 1024UL) & 0xFF);
    rsp[17] = (uint8_t)((status.flash_total_bytes / 1024UL) >> 8);
    rsp[18] = (uint8_t)((status.resource_free_bytes / 1024UL) & 0xFF);
    rsp[19] = (uint8_t)((status.resource_free_bytes / 1024UL) >> 8);
    epd_write_u32_le(&rsp[20], (uint32_t)GUI_GetUIBundleLastError());
    ble_epd_string_send(p_epd, rsp, sizeof(rsp));
}

static void send_resource_read_response(ble_epd_t* p_epd, uint16_t seq, uint32_t address,
                                        ret_code_t ret, const uint8_t* payload, uint8_t payload_len) {
    uint8_t rsp[BLE_EPD_MAX_DATA_LEN];
    uint16_t crc = 0;
    uint16_t rsp_len = 10;

    rsp[0] = EPD_RSP_RESOURCE_READ;
    rsp[1] = (uint8_t)(seq & 0xFF);
    rsp[2] = (uint8_t)(seq >> 8);
    rsp[3] = (uint8_t)ret;
    rsp[4] = (uint8_t)(address & 0xFF);
    rsp[5] = (uint8_t)((address >> 8) & 0xFF);
    rsp[6] = (uint8_t)((address >> 16) & 0xFF);
    rsp[7] = payload_len;
    if (ret == NRF_SUCCESS && payload_len > 0 && payload != NULL) {
        memcpy(&rsp[8], payload, payload_len);
        crc = crc16_compute(payload, payload_len);
    }
    rsp[8 + payload_len] = (uint8_t)(crc & 0xFF);
    rsp[9 + payload_len] = (uint8_t)(crc >> 8);
    rsp_len += payload_len;
    ble_epd_string_send(p_epd, rsp, rsp_len);
}

static bool is_resource_command(uint8_t cmd) {
    return (cmd == EPD_CMD_RES_ERASE ||
            cmd == EPD_CMD_RES_WRITE ||
            cmd == EPD_CMD_RES_COMMIT ||
            cmd == EPD_CMD_RES_STATUS ||
            cmd == EPD_CMD_RES_READ);
}

static void send_resource_busy_response(ble_epd_t* p_epd, const uint8_t* p_data, uint16_t length) {
    uint8_t cmd = (p_data != NULL && length > 0) ? p_data[0] : 0;

    switch (cmd) {
        case EPD_CMD_RES_WRITE: {
            uint16_t seq = (length >= 3) ? ((uint16_t)p_data[1] | ((uint16_t)p_data[2] << 8)) : 0;
            send_resource_op_response(p_epd, cmd, seq, NRF_ERROR_BUSY);
        } break;

        case EPD_CMD_RES_READ: {
            uint16_t seq = 0;
            uint32_t address = 0;
            if (length >= 7) {
                seq = (uint16_t)p_data[1] | ((uint16_t)p_data[2] << 8);
                address = epd_parse_u24_le(&p_data[3]);
            }
            send_resource_read_response(p_epd, seq, address, NRF_ERROR_BUSY, NULL, 0);
        } break;

        case EPD_CMD_RES_STATUS:
            send_resource_status(p_epd, NRF_ERROR_BUSY);
            break;

        case EPD_CMD_RES_ERASE:
        case EPD_CMD_RES_COMMIT:
            send_resource_op_response(p_epd, cmd, 0, NRF_ERROR_BUSY);
            break;

        default:
            break;
    }
}

// nRF52811默认引脚配置
// Default pin configuration for nRF52811
#define EPD_CFG_52811 {0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02, 0x07, 0xFF, 0x12, 0x07}




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

/**@brief 资源缺失提示屏——GFX 分页回调：把每页黑平面顺序流式写入已打开的 DTM1 数据流。
 *
 * @details 关键：**不使用 PTIN/局部窗口(PTL)寻址**。真机多轮验证表明，全刷 `write_image`
 *          走 PTIN+`_setPartialRamArea` 的逐页局部窗口寻址，在"空 Flash 上电"这种上下文里
 *          极不稳定(白屏/红黑白噪点/无反应)；而 `UC8179_Clear` 用的"DTM1/DTM2 顺序写整帧
 *          (不带 PTIN)"是可靠的(clear(true) 真机能出干净白屏)。
 *          本回调即复用该顺序写方式：调用前已 `EPD_WriteCmd(DTM1)`(复位 RAM 写指针到 0)，
 *          这里按页把黑平面数据连续写入，各页拼接正好铺满整帧(与 EPD_FillRAM 同构)。
 *          红平面(DTM2)由外层统一填 0xFF(无红)，故此处忽略 color。
 */
/* Returns true when the caller should skip all further rendering and jump to
 * finish_refresh — either because the critical-battery screen was already drawn
 * (avoid re-flashing every minute) or the voltage just crossed critical and we
 * need the caller to draw it then shut down. */
typedef struct {
    bool skip;           /* jump straight to finish_refresh           */
    bool low_battery;    /* show low-battery warning but keep running  */
    bool critical;       /* draw critical screen then power off        */
    uint16_t voltage_mv; /* sampled voltage for this refresh cycle     */
} battery_guard_result_t;

static battery_guard_result_t epd_battery_guard(ble_epd_t *p_epd) {
    static uint16_t cached_mv = 3300;
    static bool power_off_drawn = false;
    cached_mv = EPD_ReadVoltageAndCache();

    bool low      = (cached_mv <= LOW_BATTERY_THRESHOLD_MV);
    bool critical = (cached_mv <  CRITICAL_BATTERY_SHUTDOWN_MV);
    p_epd->low_battery_screen_active = low;

    battery_guard_result_t r = {false, low, critical, cached_mv};

    if (critical) {
        if (power_off_drawn) {
            NRF_LOG_WARNING("[EPD]: Critical battery screen already drawn, skipping redundant update\n");
            r.skip = true;
            return r;
        }
        power_off_drawn = true;
    } else {
        power_off_drawn = false;
    }

    if (low) {
        NRF_LOG_WARNING("[EPD]: Low battery detected (%d mV), showing warning screen\n", cached_mv);
    }
    return r;
}

/* Returns true when the caller should skip rendering because resources are
 * missing AND the hint screen was already drawn (avoids re-flashing every
 * minute on blank Flash). Also draws the missing-resource hint screen when
 * needed and forces the caller to jump to finish_refresh. */
typedef struct {
    bool missing;       /* resources are absent or invalid     */
    bool skip;          /* hint already drawn — skip this tick */
    bool draw_hint;     /* caller should draw the hint screen  */
} resource_guard_result_t;

static resource_guard_result_t epd_resource_guard(ble_epd_t *p_epd, epd_model_t *epd,
                                                   display_mode_t mode, bool low_battery) {
    static bool hint_drawn = false;
    resource_guard_result_t r = {false, false, false};

    ret_code_t wake_ret = epd_ext_resources_wake();
    if (wake_ret != NRF_SUCCESS) {
        NRF_LOG_WARNING("[EPD]: External resource flash wake failed before refresh: %lu",
                        (unsigned long)wake_ret);
    }

    r.missing = (wake_ret != NRF_SUCCESS) || !epd_resources_manifest_ready();
    bool mode_exists = GUI_ModeExists((uint8_t)mode, epd->width, epd->height);
    NRF_LOG_WARNING("[EPD]: Guard: wake=%lu, manifest=%d, low_bat=%d, mode=%d, exists=%d",
                    (unsigned long)wake_ret, (int)epd_resources_manifest_ready(), (int)low_battery,
                    (int)mode, (int)mode_exists);
    if (!r.missing && !low_battery &&
        (mode == MODE_CALENDAR || mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR) &&
        !mode_exists) {
        r.missing = true;
    }

    if (r.missing) {
        if (hint_drawn && !p_epd->force_full_refresh) {
            NRF_LOG_INFO("[EPD]: Resource hint already shown, skipping redundant refresh\n");
            r.skip = true;
        } else {
            hint_drawn = true;
            r.draw_hint = true;
            NRF_LOG_WARNING("[EPD]: External resources missing/invalid — drawing sync hint");
        }
    } else {
        hint_drawn = false;
    }
    return r;
}

static void epd_resmiss_seq_black_cb(void* user_data, uint8_t* black, uint8_t* color,
                                     uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    (void)user_data;
    (void)color;
    (void)x;
    (void)y;
    if (black == NULL) {
        return;
    }
    uint16_t wb = (uint16_t)((w + 7) / 8);
    uint32_t total = (uint32_t)wb * (uint32_t)h;
    uint32_t offset = 0;
    while (total > 0) {
        uint8_t chunk = (total > 128) ? 128 : (uint8_t)total;
        EPD_WriteData(black + offset, chunk);
        offset += chunk;
        total -= chunk;
        app_feed_wdt();
    }
}

/**@brief 绘制"资源缺失/失效"提示屏（诚实降级兜底，顺序写整帧）
 *
 * @details 外置 Flash 被清空(全 0xFF)或资源损坏时调用。与 `UC8179_Clear(refresh=true)`
 *          完全同构(该路径真机验证可靠)，仅把 DTM1 的常量填充换成提示屏黑平面的真实数据：
 *            1. `EPD_WriteCmd(DTM1)` 打开黑平面数据流(复位 RAM 写指针);
 *            2. `DrawResourceMissingScreen(seq_black_cb)` 逐页顺序流式写入黑平面(自包含
 *               内置位图字体，不读外置 Flash);
 *            3. `EPD_FillRAM(DTM2, 0xFF)` 红平面全填无红;
 *            4. `drv->refresh` 全刷显示(用 OTP 内置波形，不依赖外置 Flash)。
 *          全程不用 PTIN/PTL 局部窗口寻址，从根上规避 write_image 在本场景的不稳定。
 */
static void epd_draw_resource_missing_screen(epd_model_t* epd, gui_data_t* data) {
    if (epd == NULL || epd->drv == NULL || data == NULL) {
        return;
    }
    EPD_LED_OFF();

    uint32_t ram_bytes = (uint32_t)((epd->width + 7) / 8) * (uint32_t)epd->height;

    // DTM1(黑平面)：顺序流式写入提示屏
    EPD_WriteCmd(UC81xx_DTM1);
    DrawResourceMissingScreen(data, epd_resmiss_seq_black_cb, epd);
    app_feed_wdt();

    // DTM2(红平面)：全 0xFF = 无红
    EPD_FillRAM(UC81xx_DTM2, 0xFF, ram_bytes);
    app_feed_wdt();

    // 全刷显示(与 UC8179_Clear(refresh=true) 相同的收尾)
    if (epd->drv->refresh != NULL) {
        epd->drv->refresh(epd);
    }
    app_feed_wdt();
}

/**@brief GUI更新函数(由app_scheduler调度)
 *        GUI update function scheduled by app_scheduler
 *
 * @details 此函数处理完整的GUI更新过程,包括:
 *          1. 全刷/局刷决策
 *          2. 电池电压在整屏刷新前更新
 *          3. GUI渲染
 *          4. 显示刷新
 *
 *          This function handles the complete GUI update process including:
 *          1. Full refresh vs partial refresh decision
 *          2. Refresh battery voltage before full-screen redraws
 *          3. GUI rendering
 *          4. Display refresh
 *
 * @param[in] p_event_data  epd_gui_update_event_t结构指针
 * @param[in] event_size    事件数据大小
 */
static void epd_gui_update(void* p_event_data, uint16_t event_size) {
    epd_gui_update_event_t* event = (epd_gui_update_event_t*)p_event_data;
    ble_epd_t* p_epd = event->p_epd;
    uint16_t dev_name_len;
    uint32_t err_code;
    uint16_t refresh_voltage_mv = 3300;  /* updated by battery guard, readable at finish_refresh */
    bool critical_battery = false;
    bool low_battery = false;
    gui_data_t data;
    resource_guard_result_t res;

    // Set refreshing flag to prevent BLE disconnect from interrupting refresh
    // 设置刷新标志,防止BLE断开中断刷新过程
    // 当BLE断开时,on_disconnect会检查此标志,如果正在刷新则跳过sleep操作
    p_epd->is_refreshing = true;
    p_epd->last_refresh_start = event->timestamp;
    p_epd->last_refresh_start_ticks = app_timer_cnt_get();

    app_feed_wdt();  // Feed watchdog at the beginning. 开始时喂狗
    EPD_GPIO_Init();  // 初始化GPIO / Initialize GPIO
    UC8179_ClearBusyTimeout();  // Reset busy-timeout flag before each refresh cycle.
    epd_model_t* epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    EPD_LED_OFF();  // Keep LED off during all refresh operations.
                    // 所有刷新过程中保持LED熄灭。

    display_mode_t mode = (display_mode_t)p_epd->config.display_mode;
    bool use_partial = false;

    // ── Battery guard ────────────────────────────────────────────────────────
    {
        battery_guard_result_t bat = epd_battery_guard(p_epd);
        refresh_voltage_mv = bat.voltage_mv;
        critical_battery   = bat.critical;
        if (bat.skip) goto finish_refresh;
    }
    low_battery = (refresh_voltage_mv <= LOW_BATTERY_THRESHOLD_MV);

    // ── Prepare GUI data ─────────────────────────────────────────────────────
    data = (gui_data_t){
        .mode        = mode,
        .color       = epd->color,
        .width       = epd->width,
        .height      = epd->height,
        .timestamp   = event->timestamp,
        .week_start  = p_epd->config.week_start,
        .temperature = epd->drv->read_temp(epd),
        .voltage_mv  = refresh_voltage_mv,
    };
    // 获取BLE设备名称(SSID) / Get BLE device name
    dev_name_len = sizeof(data.ssid);
    err_code = sd_ble_gap_device_name_get((uint8_t*)data.ssid, &dev_name_len);
    if (err_code == NRF_SUCCESS && dev_name_len > 0) data.ssid[dev_name_len] = '\0';

    // ── Resource guard ───────────────────────────────────────────────────────
    res = epd_resource_guard(p_epd, epd, mode, low_battery);
    if (res.skip)       goto finish_refresh;
    if (res.draw_hint)  { epd_draw_resource_missing_screen(epd, &data); goto finish_refresh; }

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
    use_partial = ((mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR) &&
                   epd->drv->ic == EPD_DRIVER_IC_UC8179 &&
                   epd->drv->refresh_partial != NULL &&
                   !low_battery);

    if (use_partial) {
        NRF_LOG_INFO("[EPD]: === Using PARTIAL refresh for clock mode ===\n");

        // ====================================================================
        // 全刷触发条件判断
        // Full Refresh Trigger Conditions
        // ====================================================================
        //
        // 1. 首次刷新 (p_epd->last_full_clear == 0)
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
                                 (p_epd->last_full_clear == 0) ||
                                 (at_refresh_boundary && !skip_full_in_quiet_hours);

        if (need_full_refresh) {
            // ================================================================
            // 执行全刷流程
            // Execute Full Refresh Flow
            // ================================================================
            NRF_LOG_INFO("[EPD]: *** Performing FULL refresh (mode switch or periodic) ***\n");
            ble_log("[EPD]:full ts=%u %s", event->timestamp,
                    p_epd->force_full_refresh ? "force" : (p_epd->last_full_clear == 0 ? "first" : "period"));

            // Standard full refresh flow to ensure display is correct
            // 标准全刷流程,确保显示正确
            app_feed_wdt();  // Feed watchdog before drawing GUI. 绘制GUI前喂狗
            DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
            app_feed_wdt();  // Feed watchdog after drawing GUI. 绘制GUI后喂狗
            epd->drv->refresh(epd);
            app_feed_wdt();  // Feed watchdog after refresh. 刷新后喂狗
            if (UC8179_HadBusyTimeout()) {
                NRF_LOG_ERROR("[EPD]: BUSY timeout during full refresh (clock path), aborting\n");
                goto finish_refresh;
            }

            EPD_LED_OFF();
            p_epd->last_full_clear = event->timestamp;  // 记录全刷时间 / Record full refresh time
            if (data.mode == MODE_CLOCK_CALENDAR) {
                GUI_MarkTimeRefreshTimestamp(data.timestamp);
            }
            p_epd->force_full_refresh = false;  // Clear the force flag after full refresh. 清除强制全刷标志
            NRF_LOG_INFO("[EPD]: Full refresh completed\n");
            ble_log("[EPD]:full ok");

            // Seed the differential clock refresh state
            {
                tm_t tm_seed = {0};
                transformTime(event->timestamp, &tm_seed);
                epd_clock_diff_seed(tm_seed.tm_hour, tm_seed.tm_min);
            }
        } else {
            // ================================================================
            // 执行局刷流程
            // Execute Partial Refresh Flow
            // ================================================================
            NRF_LOG_INFO("[EPD]: Using partial refresh (%d min since last full, interval=%d min)\n",
                        (int)(event->timestamp - p_epd->last_full_clear) / 60,
                        (int)(refresh_interval / 60));

            // Initialize for partial mode (includes reset)
            // 初始化局刷模式(包含复位)
            if (epd->drv->init_partial) {
                UC8179_ClearBusyTimeout();
                epd->drv->init_partial(epd);
            } else {
                NRF_LOG_ERROR("[EPD]: init_partial not available!\n");
                goto fallback_full_refresh;  // 回退到全刷 / Fallback to full refresh
            }
            app_feed_wdt();  // Feed watchdog after init_partial. 局刷初始化后喂狗
            if (UC8179_HadBusyTimeout()) {
                NRF_LOG_ERROR("[EPD]: BUSY timeout during partial init, falling back to full refresh\n");
                goto fallback_full_refresh;
            }

            // Try true differential clock partial refresh first
            if (epd_clock_diff_try_partial(epd, data.timestamp, data.mode)) {
                NRF_LOG_INFO("[EPD]: Differential clock partial refresh successful\n");
                goto finish_refresh;
            }

            if (data.mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("[EPD]: Differential refresh failed/unseeded, falling back to full refresh\n");
                goto fallback_full_refresh;
            }

            // Fallback for MODE_CLOCK (full screen partial refresh)
            uint16_t partial_x = 0, partial_y = 0, partial_w = epd->width, partial_h = epd->height;
            UC8179_Set_Partial_Strong_BW_Allowed(true);

            // Clear the partial refresh area to white first
            if (epd->drv->clear_partial) {
                epd->drv->clear_partial(epd, false);  // Don't refresh, just clear RAM. 不刷新,只清RAM
            } else {
                NRF_LOG_ERROR("[EPD]: clear_partial not available!\n");
                goto fallback_full_refresh;
            }
            app_feed_wdt();  // Feed watchdog after clear_partial. 清除局刷区域后喂狗

            // Write black data using partial image write
            // 使用局部图像写入写入黑色数据
            NRF_LOG_INFO("[EPD]: Drawing GUI for partial refresh (%d,%d,%d,%d)\n", partial_x, partial_y, partial_w, partial_h);
            ble_log("[EPD]:part %d,%d,%d,%d", partial_x, partial_y, partial_w, partial_h);
            DrawGUI(&data, (buffer_callback)epd->drv->write_image_partial, epd);
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
            if (UC8179_HadBusyTimeout()) {
                NRF_LOG_ERROR("[EPD]: BUSY timeout during partial refresh (clock mode), aborting\n");
                goto finish_refresh;
            }

            // Seed the state since we just did a full window redraw
            {
                tm_t tm_seed = {0};
                transformTime(data.timestamp, &tm_seed);
                epd_clock_diff_seed(tm_seed.tm_hour, tm_seed.tm_min);
            }
        }

        NRF_LOG_INFO("[EPD]: === Partial refresh sequence complete ===\n");
        ble_log("[EPD]:part ok");
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
        ble_log("[EPD]:full");

        if (epd->drv->init) {
            epd->drv->init(epd);
        }

        app_feed_wdt();  // Feed watchdog before drawing GUI. 绘制GUI前喂狗
        DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
        app_feed_wdt();  // Feed watchdog after drawing GUI. 绘制GUI后喂狗
        epd->drv->refresh(epd);
        app_feed_wdt();  // Feed watchdog after refresh. 刷新后喂狗
        if (UC8179_HadBusyTimeout()) {
            NRF_LOG_ERROR("[EPD]: BUSY timeout during full refresh (fallback path), aborting\n");
            goto finish_refresh;
        }

        EPD_LED_OFF();
        p_epd->last_full_clear = event->timestamp;
        p_epd->force_full_refresh = false;
        NRF_LOG_INFO("[EPD]: === Full refresh complete ===\n");
        ble_log("[EPD]:full ok");

        // Seed the state after fallback full refresh
        {
            tm_t tm_seed = {0};
            transformTime(event->timestamp, &tm_seed);
            epd_clock_diff_seed(tm_seed.tm_hour, tm_seed.tm_min);
        }
    }

finish_refresh:
    app_feed_wdt();

    // ========================================================================
    // 刷新后处理
    // Post-refresh handling
    // ========================================================================
    // If BLE disconnected during refresh, put EPD to sleep now
    // (on_disconnect skipped this to avoid interrupting refresh)
    // 如果刷新期间BLE断开,现在将EPD置于睡眠状态
    // (on_disconnect跳过了此操作以避免中断刷新)
    bool ble_disconnected = (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID);
    if (ble_disconnected) {
        NRF_LOG_INFO("[EPD]: Refresh complete, BLE disconnected - putting EPD to sleep\n");
    }

    if (critical_battery) {
        NRF_LOG_ERROR("[EPD]: Critical battery warning displayed (%d mV), shutting down\n", refresh_voltage_mv);
    }

    if ((ble_disconnected || critical_battery) && epd->drv->sleep) {
        epd->drv->sleep(epd);
        nrf_delay_ms(200);  // for sleep. 等待睡眠生效
    }

    if (ble_disconnected || critical_battery) {
        ret_code_t flash_sleep_ret = epd_ext_resources_sleep();
        if (flash_sleep_ret != NRF_SUCCESS) {
            NRF_LOG_WARNING("[EPD]: External resource flash sleep failed: %lu",
                            (unsigned long)flash_sleep_ret);
        }
    }

    if (critical_battery) {
        sleep_mode_enter();
    }

    EPD_GPIO_Uninit();  // 释放GPIO / Release GPIO

    // Clear refreshing flag after refresh is complete
    // 刷新完成后清除刷新标志
    p_epd->is_refreshing = false;

#if NRF_LOG_ENABLED && NRF_LOG_DEFAULT_LEVEL >= 3
    /* PRD §US-3.1 AC2: Fine-grained latency diagnostics.
     * Use app_timer RTC ticks (32768 Hz, ~0.03 ms resolution) to measure
     * the physical refresh duration in milliseconds. */
    {
        uint32_t end_ticks = app_timer_cnt_get();
        uint32_t diff_ticks = app_timer_cnt_diff_compute(end_ticks, p_epd->last_refresh_start_ticks);
        uint32_t render_ms = (uint32_t)(((uint64_t)diff_ticks * 1000U) / APP_TIMER_CLOCK_FREQ);
        NRF_LOG_INFO("[EPD] Render duration: %u ms, ts_start=%u ts_end=%u",
                     render_ms, p_epd->last_refresh_start, timestamp());
    }
#endif

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

    // Blink LED once on BLE disconnect, even if the display is refreshing.
    // BLE断开时闪烁LED一次, 即使屏幕正在刷新。
    EPD_LED_BLINK();

    // If screen refresh is in progress, skip sleep/GPIO_Uninit to avoid interrupting refresh
    // The epd_gui_update function will handle GPIO_Uninit after refresh completes
    // 如果屏幕刷新正在进行,跳过sleep/GPIO_Uninit以避免中断刷新
    // epd_gui_update函数将在刷新完成后处理GPIO_Uninit
    if (p_epd->is_refreshing) {
        NRF_LOG_INFO("[EPD]: BLE disconnected during refresh, skipping sleep/GPIO_Uninit\n");
        app_feed_wdt();
        return;
    }

    // Only put EPD to sleep if it has been initialized
    // 只有在EPD已初始化时才将其置于睡眠状态
    if (p_epd->epd != NULL) {
        p_epd->epd->drv->sleep(p_epd->epd);
        nrf_delay_ms(200);  // for sleep. 等待睡眠生效
        EPD_GPIO_Uninit();
    }

    ret_code_t flash_sleep_ret = epd_ext_resources_sleep();
    if (flash_sleep_ret != NRF_SUCCESS) {
        NRF_LOG_WARNING("[EPD]: External resource flash sleep on disconnect failed: %lu",
                        (unsigned long)flash_sleep_ret);
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
        epd_clock_diff_invalidate();
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

static size_t epd_append_char(char* out, size_t out_size, size_t pos, char value) {
    if (out_size > 0 && pos < out_size - 1) {
        out[pos] = value;
    }
    return pos + 1;
}

static size_t epd_append_uint(char* out, size_t out_size, size_t pos, uint32_t value) {
    char digits[10];
    uint8_t len = 0;

    do {
        digits[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && len < sizeof(digits));

    while (len > 0) {
        pos = epd_append_char(out, out_size, pos, digits[--len]);
    }
    return pos;
}

static size_t epd_make_prefixed_uint(char* out, size_t out_size, const char* prefix, uint32_t value) {
    size_t pos = 0;

    while (*prefix != '\0') {
        pos = epd_append_char(out, out_size, pos, *prefix++);
    }
    pos = epd_append_uint(out, out_size, pos, value);

    if (out_size > 0) {
        out[(pos < out_size) ? pos : (out_size - 1)] = '\0';
    }
    return (pos < out_size) ? pos : (out_size - 1);
}

/**@brief 发送当前时间到BLE对端
 *        Send current time to BLE peer
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
static void epd_send_time(ble_epd_t* p_epd) {
    char buf[20] = {0};
    size_t len = epd_make_prefixed_uint(buf, sizeof(buf), "t=", timestamp());
    ble_epd_string_send(p_epd, (uint8_t*)buf, len);
}

/**@brief 发送当前电池电压到BLE对端
 *        Send current battery voltage to BLE peer
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD service structure
 */
static void epd_send_voltage(ble_epd_t* p_epd) {
    char buf[16] = {0};
    uint16_t voltage_mv = EPD_ReadVoltageAndCache();
    size_t len = epd_make_prefixed_uint(buf, sizeof(buf), "v=", voltage_mv);
    ble_epd_string_send(p_epd, (uint8_t*)buf, len);
}

/**@brief 发送MTU大小到BLE对端
 *        Send MTU size to BLE peer
 *
 * @param[in] p_epd  EPD服务结构指针 / Pointer to EPD Service structure
 */
static void epd_send_mtu(ble_epd_t* p_epd) {
    char buf[10] = {0};
    size_t len = epd_make_prefixed_uint(buf, sizeof(buf), "mtu=", p_epd->max_data_len);
    ble_epd_string_send(p_epd, (uint8_t*)buf, len);
}

/**@brief 解析SET_TIME/SYNC_TIME_SILENT中的时间戳
 *        Parse timestamp payload used by SET_TIME and SYNC_TIME_SILENT
 *
 * @details 载荷格式:
 *          - [1..4]: Unix时间戳(大端序, UTC秒)
 *          - [5]:    时区偏移(小时, int8_t, 可选, 默认+8)
 *          Payload format:
 *          - [1..4]: Unix timestamp (big-endian, UTC seconds)
 *          - [5]:    Timezone offset in hours (int8_t, optional, defaults to +8)
 *
 * @param[in] p_data  命令数据 / Command payload
 * @param[in] length  数据长度 / Payload length
 *
 * @return 本地时区修正后的时间戳 / Timestamp adjusted to local timezone
 */
static uint32_t epd_parse_time_payload(const uint8_t* p_data, uint16_t length) {
    uint32_t parsed_timestamp = ((uint32_t)p_data[1] << 24) |
                                ((uint32_t)p_data[2] << 16) |
                                ((uint32_t)p_data[3] << 8) |
                                (uint32_t)p_data[4];
    int8_t timezone_hours = (length > 5) ? (int8_t)p_data[5] : 8;
    return parsed_timestamp + (timezone_hours * 60 * 60);
}

static void cmd_set_pins(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 8) return;
    epd_config_t candidate = p_epd->config;
    candidate.mosi_pin = p_data[1];
    candidate.sclk_pin = p_data[2];
    candidate.cs_pin   = p_data[3];
    candidate.dc_pin   = p_data[4];
    candidate.rst_pin  = p_data[5];
    candidate.busy_pin = p_data[6];
    candidate.bs_pin   = p_data[7];
    if (length > 8) candidate.en_pin = p_data[8];
    if (EPD_GPIO_Config_Conflicts_With_Ext_Flash(&candidate)) {
        NRF_LOG_ERROR("[EPD]: SET_PINS rejected: P0.12-P0.15 reserved for external Flash");
        return;
    }
    p_epd->config = candidate;
    epd_config_write(&p_epd->config);
    EPD_GPIO_Uninit();
    EPD_GPIO_Load(&p_epd->config);
    EPD_GPIO_Init();
    app_feed_wdt();
}

static void cmd_init(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    p_epd->epd = epd_init((epd_model_id_t)(length > 1 ? p_data[1] : p_epd->config.model_id));
    if (p_epd->epd->id != p_epd->config.model_id) {
        p_epd->config.model_id = p_epd->epd->id;
        epd_config_write(&p_epd->config);
    }
    epd_send_mtu(p_epd);
    epd_send_time(p_epd);
    epd_send_voltage(p_epd);
    app_feed_wdt();
}

static void cmd_clear(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (p_epd->epd == NULL) return;
    if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
        NRF_LOG_INFO("Switch mode: clear\n");
        epd_switch_from_clock_mode(p_epd);
    }
    epd_update_display_mode(p_epd, MODE_PICTURE);
    p_epd->epd->drv->clear(p_epd->epd, length > 1 ? p_data[1] : true);
    app_feed_wdt();
}

static void cmd_send_command(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 2) return;
    if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
        NRF_LOG_INFO("Switch mode: cmd\n");
        epd_switch_from_clock_mode(p_epd);
        epd_update_display_mode(p_epd, MODE_PICTURE);
    }
    EPD_WriteCmd(p_data[1]);
    app_feed_wdt();
}

static void cmd_send_data(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
        NRF_LOG_INFO("Switch mode: data\n");
        epd_switch_from_clock_mode(p_epd);
        epd_update_display_mode(p_epd, MODE_PICTURE);
    }
    EPD_WriteData(&p_data[1], length - 1);
    app_feed_wdt();
}

static void cmd_refresh(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    if (p_epd->epd == NULL) return;
    epd_update_display_mode(p_epd, MODE_PICTURE);
    p_epd->epd->drv->refresh(p_epd->epd);
    app_feed_wdt();
}

static void cmd_sleep(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    if (p_epd->epd == NULL) return;
    p_epd->epd->drv->sleep(p_epd->epd);
    app_feed_wdt();
}

static void cmd_set_time(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 5) return;

    /* PRD §4.3: SET_TIME rate limit — reject if less than 10 s since last
     * accepted SET_TIME to prevent high-frequency refresh abuse. */
    uint32_t now = timestamp();
    if (p_epd->last_set_time_ts != 0 &&
        (now - p_epd->last_set_time_ts) < EPD_SET_TIME_RATE_LIMIT_S) {
        NRF_LOG_WARNING("[EPD]: SET_TIME rate-limited (last %u s ago)",
                        now - p_epd->last_set_time_ts);
        app_feed_wdt();
        return;
    }

    NRF_LOG_DEBUG("time: %02x %02x %02x %02x\n", p_data[1], p_data[2], p_data[3], p_data[4]);
    if (length > 5) {
        NRF_LOG_DEBUG("timezone: %d\n", (int8_t)p_data[5]);
    }
    uint32_t ts = epd_parse_time_payload(p_data, length);
    set_timestamp(ts);
    p_epd->last_set_time_ts = now;  // Record device-local time, not the (pre-compensated) payload ts.
    if (length > 6) {
        epd_update_display_mode_with_refresh(p_epd, (display_mode_t)p_data[6], false);
    }
    ble_epd_on_timer(p_epd, ts, true);
    app_feed_wdt();
}

static void cmd_sync_time_silent(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 5) return;
    uint32_t ts = epd_parse_time_payload(p_data, length);
    set_timestamp(ts);
    NRF_LOG_INFO("[EPD]: Time synced silently\n");
    epd_send_time(p_epd);
    app_feed_wdt();
}

static void cmd_set_week_start(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 2) return;
    if (p_data[1] < 7 && p_data[1] != p_epd->config.week_start) {
        p_epd->config.week_start = p_data[1];
        epd_config_write(&p_epd->config);
    }
    app_feed_wdt();
}

static void cmd_set_mode(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 2) return;
    if (p_data[1] == MODE_PICTURE ||
        (p_data[1] <= 15 &&
         p_epd->epd != NULL &&
         GUI_ModeExists(p_data[1], p_epd->epd->width, p_epd->epd->height))) {
        display_mode_t new_mode = (display_mode_t)p_data[1];
        NRF_LOG_INFO("[EPD]: CMD_SET_MODE: switching to mode %d\n", new_mode);
        if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
            if (new_mode != MODE_CLOCK && new_mode != MODE_CLOCK_CALENDAR) {
                epd_switch_from_clock_mode(p_epd);
            }
        }
        epd_update_display_mode(p_epd, new_mode);
        ble_epd_on_timer(p_epd, timestamp(), true);
    }
    app_feed_wdt();
}

static void cmd_write_image(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 3) return;
    if (p_epd->epd == NULL) return;
    if (p_epd->config.display_mode == MODE_CLOCK || p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
        NRF_LOG_INFO("Switch mode: img\n");
        epd_switch_from_clock_mode(p_epd);
    }
    epd_update_display_mode(p_epd, MODE_PICTURE);
    p_epd->epd->drv->write_ram(p_epd->epd, p_data[1], &p_data[2], length - 2);
    app_feed_wdt();
}

static void cmd_write_block(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 8) return;
    if (p_epd->epd == NULL) return;
    uint16_t block_id    = p_data[1] | (p_data[2] << 8);
    uint16_t total_blocks = p_data[3] | (p_data[4] << 8);
    uint8_t  cfg         = p_data[5];
    uint16_t payload_len = length - 8;
    uint8_t *payload     = &p_data[6];
    uint16_t recv_crc    = p_data[length - 2] | (p_data[length - 1] << 8);
    uint16_t calc_crc    = crc16_compute(payload, payload_len);

    if (calc_crc == recv_crc) {
        if (block_id >= EPD_MAX_BLOCKS || block_id >= total_blocks) {
            NRF_LOG_WARNING("[EPD]: Block %d invalid (max=%d, total=%d)\n",
                            block_id, EPD_MAX_BLOCKS, total_blocks);
            send_block_response(p_epd, block_id, 0x02);
            return;
        }
        if (total_blocks == 0 || total_blocks > EPD_MAX_BLOCKS) {
            NRF_LOG_WARNING("[EPD]: Invalid total_blocks=%d\n", total_blocks);
            send_block_response(p_epd, block_id, 0x02);
            return;
        }
        if (block_id == 0 || !p_epd->transfer_ctx.transfer_active) {
            p_epd->transfer_ctx.total_blocks    = total_blocks;
            p_epd->transfer_ctx.received_blocks = 0;
            memset(p_epd->transfer_ctx.block_bitmap, 0, EPD_BLOCK_BITMAP_SIZE);
            p_epd->transfer_ctx.transfer_active = true;
            if (p_epd->config.display_mode == MODE_CLOCK ||
                p_epd->config.display_mode == MODE_CLOCK_CALENDAR) {
                NRF_LOG_INFO("Switch mode: block\n");
                epd_switch_from_clock_mode(p_epd);
            }
            epd_update_display_mode(p_epd, MODE_PICTURE);
        } else if (p_epd->transfer_ctx.total_blocks != total_blocks) {
            NRF_LOG_WARNING("[EPD]: total_blocks changed mid-transfer: %d -> %d, resetting\n",
                            p_epd->transfer_ctx.total_blocks, total_blocks);
            p_epd->transfer_ctx.total_blocks    = total_blocks;
            p_epd->transfer_ctx.received_blocks = 0;
            memset(p_epd->transfer_ctx.block_bitmap, 0, EPD_BLOCK_BITMAP_SIZE);
        }
        uint16_t byte_idx = block_id / 8;
        uint8_t  bit_idx  = block_id % 8;
        if (byte_idx < EPD_BLOCK_BITMAP_SIZE &&
            !(p_epd->transfer_ctx.block_bitmap[byte_idx] & (1 << bit_idx))) {
            p_epd->epd->drv->write_ram(p_epd->epd, cfg, payload, payload_len);
            p_epd->transfer_ctx.block_bitmap[byte_idx] |= (1 << bit_idx);
            p_epd->transfer_ctx.received_blocks++;
            NRF_LOG_DEBUG("[EPD]: Block %d/%d received, cfg=0x%02X, CRC OK\n",
                          block_id, total_blocks, cfg);
        } else {
            NRF_LOG_DEBUG("[EPD]: Block %d already received (duplicate)\n", block_id);
        }
        send_block_response(p_epd, block_id, 0x00);
    } else {
        NRF_LOG_WARNING("[EPD]: Block %d CRC mismatch! recv=0x%04X, calc=0x%04X\n",
                        block_id, recv_crc, calc_crc);
        send_block_response(p_epd, block_id, 0x01);
    }
    app_feed_wdt();
}

static void cmd_query_status(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    send_transfer_status(p_epd);
}

static void cmd_reset_transfer(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    memset(&p_epd->transfer_ctx, 0, sizeof(image_transfer_ctx_t));
    if (length > 1) {
        p_epd->transfer_ctx.session_id = p_data[1];
    }
    p_epd->transfer_ctx.transfer_active = false;
    NRF_LOG_INFO("[EPD]: Transfer state reset, session_id=%d\n", p_epd->transfer_ctx.session_id);
}

static void cmd_res_erase(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 8) {
        send_resource_op_response(p_epd, EPD_CMD_RES_ERASE, 0, NRF_ERROR_INVALID_LENGTH);
        return;
    }
    uint32_t address   = epd_parse_u24_le(&p_data[1]);
    uint32_t erase_len = epd_parse_u32_le(&p_data[4]);
    ret_code_t ret = epd_resources_ensure_ready();
    if (ret == NRF_SUCCESS) {
        ret = epd_ext_resource_erase(address, erase_len);
    }
    if (ret == NRF_SUCCESS) {
        /* 擦除会使 manifest 失效，GUI_ReloadUICache 会同步重置农历库缓存并
         * 失效 UI/月历渲染缓存。重建失败时缓存被标记为无效，后续渲染回退到
         * 干净白屏而非乱码。 */
        if (p_epd->epd != NULL) {
            (void)GUI_ReloadUICache(p_epd->epd->width, p_epd->epd->height, (uint8_t)p_epd->epd->color);
        }
        p_epd->force_full_refresh = true;
    }
    send_resource_op_response(p_epd, EPD_CMD_RES_ERASE, 0, ret);
    app_feed_wdt();
}

static void cmd_res_write(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 8) {
        send_resource_op_response(p_epd, EPD_CMD_RES_WRITE, 0, NRF_ERROR_INVALID_LENGTH);
        return;
    }
    uint16_t seq        = (uint16_t)p_data[1] | ((uint16_t)p_data[2] << 8);
    uint32_t address    = epd_parse_u24_le(&p_data[3]);
    uint8_t *payload    = &p_data[6];
    uint16_t payload_len = length - 8;
    uint16_t recv_crc   = (uint16_t)p_data[length - 2] | ((uint16_t)p_data[length - 1] << 8);
    uint16_t calc_crc   = crc16_compute(payload, payload_len);
    if (calc_crc != recv_crc) {
        send_resource_op_response(p_epd, EPD_CMD_RES_WRITE, seq, NRF_ERROR_INVALID_DATA);
        return;
    }
    ret_code_t ret = epd_resources_ensure_ready();
    if (ret == NRF_SUCCESS) {
        ret = epd_ext_resource_write(address, payload, payload_len);
    }
    send_resource_op_response(p_epd, EPD_CMD_RES_WRITE, seq, ret);
    app_feed_wdt();
}

static void cmd_res_commit(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    ret_code_t ret = epd_resources_ensure_ready();
    if (ret == NRF_SUCCESS) {
        ret = epd_ext_resource_commit_staging();
    }
    bool commit_success = (ret == NRF_SUCCESS);
    if (commit_success) {
        /* 新资源已激活：GUI_ReloadUICache 会同步重置农历库缓存并重建
         * UI/月历渲染缓存，使更新后的数据库无需重启设备即可生效。 */
        if (p_epd->epd != NULL) {
            ret_code_t ui_ret = GUI_ReloadUICache(p_epd->epd->width, p_epd->epd->height, (uint8_t)p_epd->epd->color);
            if (ui_ret != NRF_SUCCESS) {
                NRF_LOG_WARNING("[EPD]: UI cache reload after resource commit failed: %lu",
                                (unsigned long)ui_ret);
            }
        }
        p_epd->force_full_refresh = true;
    }
    /* 提交结束资源会话(无论成功与否)，恢复周期刷新以立即呈现新资源；
     * 失败时 web 会从擦除重新开始，届时会再次进入会话。 */
    p_epd->res_session_active = false;
    send_resource_op_response(p_epd, EPD_CMD_RES_COMMIT, 0, ret);

    app_feed_wdt();
}

static void cmd_res_status(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    ret_code_t ret = epd_resources_ensure_ready();
    send_resource_status(p_epd, ret);
    app_feed_wdt();
}

static void cmd_res_read(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    uint8_t  buffer[BLE_EPD_MAX_DATA_LEN];
    uint16_t max_payload = p_epd->max_data_len > 10 ? (p_epd->max_data_len - 10) : 0;
    if (length < 7) {
        send_resource_read_response(p_epd, 0, 0, NRF_ERROR_INVALID_LENGTH, NULL, 0);
        return;
    }
    uint16_t seq     = (uint16_t)p_data[1] | ((uint16_t)p_data[2] << 8);
    uint32_t address = epd_parse_u24_le(&p_data[3]);
    uint8_t  read_len = p_data[6];
    if (read_len > max_payload || read_len > sizeof(buffer)) {
        send_resource_read_response(p_epd, seq, address, NRF_ERROR_INVALID_LENGTH, NULL, 0);
        return;
    }
    ret_code_t ret = epd_resources_ensure_ready();
    if (ret == NRF_SUCCESS) {
        ret = epd_ext_resource_read_raw(address, buffer, read_len);
    }
    send_resource_read_response(p_epd, seq, address, ret, buffer, (ret == NRF_SUCCESS) ? read_len : 0);
    app_feed_wdt();
}

static void cmd_set_config(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    if (length < 2) return;
    memcpy(&p_epd->config, &p_data[1], (length - 1 > EPD_CONFIG_SIZE) ? EPD_CONFIG_SIZE : length - 1);
    epd_config_write(&p_epd->config);
    
    // Always force full refresh and trigger update when config is updated,
    // so any changes (mode, week_start, etc.) are immediately visible.
    p_epd->force_full_refresh = true;
    ble_epd_on_timer(p_epd, timestamp(), true);
    
    app_feed_wdt();
}

static void cmd_sys_sleep(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_epd; (void)p_data; (void)length;
    sleep_mode_enter();
    app_feed_wdt();
}

static void cmd_sys_reset(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_epd; (void)p_data; (void)length;
    nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_RESET);
}

static void cmd_cfg_erase(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) {
    (void)p_data; (void)length;
    epd_config_clear(&p_epd->config);
    nrf_delay_ms(100);
    NVIC_SystemReset();
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
    if (p_data == NULL || length == 0) return;

    NRF_LOG_DEBUG("[EPD]: Executing command 0x%02x, LEN=%d\n", p_data[0], length);
    NRF_LOG_HEXDUMP_DEBUG(p_data, length);

    // Feed watchdog before command execution
    // 命令执行前喂狗
    app_feed_wdt();

    switch (p_data[0]) {
        case EPD_CMD_SET_PINS:        cmd_set_pins(p_epd, p_data, length);        break;
        case EPD_CMD_INIT:            cmd_init(p_epd, p_data, length);            break;
        case EPD_CMD_CLEAR:           cmd_clear(p_epd, p_data, length);           break;
        case EPD_CMD_SEND_COMMAND:    cmd_send_command(p_epd, p_data, length);    break;
        case EPD_CMD_SEND_DATA:       cmd_send_data(p_epd, p_data, length);       break;
        case EPD_CMD_REFRESH:         cmd_refresh(p_epd, p_data, length);         break;
        case EPD_CMD_SLEEP:           cmd_sleep(p_epd, p_data, length);           break;
        case EPD_CMD_SET_TIME:        cmd_set_time(p_epd, p_data, length);        break;
        case EPD_CMD_SYNC_TIME_SILENT: cmd_sync_time_silent(p_epd, p_data, length); break;
        case EPD_CMD_SET_WEEK_START:  cmd_set_week_start(p_epd, p_data, length);  break;
        case EPD_CMD_SET_MODE:        cmd_set_mode(p_epd, p_data, length);        break;
        case EPD_CMD_WRITE_IMAGE:     cmd_write_image(p_epd, p_data, length);     break;
        case EPD_CMD_WRITE_BLOCK:     cmd_write_block(p_epd, p_data, length);     break;
        case EPD_CMD_QUERY_STATUS:    cmd_query_status(p_epd, p_data, length);    break;
        case EPD_CMD_RESET_TRANSFER:  cmd_reset_transfer(p_epd, p_data, length);  break;
        case EPD_CMD_RES_ERASE:       cmd_res_erase(p_epd, p_data, length);       break;
        case EPD_CMD_RES_WRITE:       cmd_res_write(p_epd, p_data, length);       break;
        case EPD_CMD_RES_COMMIT:      cmd_res_commit(p_epd, p_data, length);      break;
        case EPD_CMD_RES_STATUS:      cmd_res_status(p_epd, p_data, length);      break;
        case EPD_CMD_RES_READ:        cmd_res_read(p_epd, p_data, length);        break;
        case EPD_CMD_SET_CONFIG:      cmd_set_config(p_epd, p_data, length);      break;
        case EPD_CMD_SYS_SLEEP:       cmd_sys_sleep(p_epd, p_data, length);       break;
        case EPD_CMD_SYS_RESET:       cmd_sys_reset(p_epd, p_data, length);       break;
        case EPD_CMD_CFG_ERASE:       cmd_cfg_erase(p_epd, p_data, length);       break;
        default: break;
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
    if (p_data == NULL || length == 0) return;

    if (p_data[0] != 0x31 && p_data[0] != 0x41) {
        ble_log("[CMD]:%02x/%d", p_data[0], length);
    }

    // Resource flash commands share SPI/power state with GUI resource loading and
    // must not run while the panel driver is in a refresh transaction.
    if (is_resource_command(p_data[0]) && p_epd->is_refreshing) {
        NRF_LOG_WARNING("[EPD]: Resource command 0x%02x rejected while refreshing", p_data[0]);
        send_resource_busy_response(p_epd, p_data, length);
        return;
    }

    // 资源变更命令(擦除/写入/提交)进入资源会话，暂停周期刷新，避免刷新占用SPI导致写入失败。
    // 每条命令续期空闲超时；提交完成后由执行器结束会话。查询类命令(STATUS/READ)只读，不进入会话。
    if (p_data[0] == EPD_CMD_RES_ERASE ||
        p_data[0] == EPD_CMD_RES_WRITE ||
        p_data[0] == EPD_CMD_RES_COMMIT) {
        p_epd->res_session_active = true;
        p_epd->res_session_deadline = timestamp() + EPD_RES_SESSION_TIMEOUT_S;
    }

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
            ble_log("[SYS]:mtu=%d rst=0x%X", p_epd->max_data_len, get_reset_reason());
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
    p_epd->last_full_clear    = 0;      // No full refresh yet. 尚未全刷
    p_epd->is_refreshing = false;       // Initialize refreshing flag. 初始化刷新中标志
    p_epd->low_battery_screen_active = false;  // Low-battery page inactive by default. 默认未进入低电量页
    p_epd->res_session_active = false;  // No resource update session at boot. 启动时无资源更新会话
    p_epd->res_session_deadline = 0;
    p_epd->last_refresh_start = 0;            // No refresh yet. 尚未刷新
    p_epd->last_refresh_start_ticks = 0;     // No refresh ticks yet. 尚未刷新
    p_epd->last_set_time_ts = 0;             // No SET_TIME yet. 尚未校时

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

    // Initialize EPD driver and external flash resources while power (EN pin) is active
    // 根据配置初始化EPD驱动，并在电源(EN引脚)有效时初始化外部Flash资源
    EPD_GPIO_Init();
    p_epd->epd = epd_init((epd_model_id_t)p_epd->config.model_id);

    ret_code_t resource_ret = epd_ext_resources_init();
    if (resource_ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("[EPD]: External resource flash init failed, ret=%lu "
                      "(LUT/font/lunar reads will be unavailable)",
                      (unsigned long)resource_ret);
    }

    if (resource_ret == NRF_SUCCESS && p_epd->epd != NULL) {
        ret_code_t ui_ret = GUI_ReloadUICache(p_epd->epd->width, p_epd->epd->height, (uint8_t)p_epd->epd->color);
        if (ui_ret != NRF_SUCCESS) {
            NRF_LOG_WARNING("[EPD]: UI cache preload failed: %lu", (unsigned long)ui_ret);
        }
        app_feed_wdt();
    }

    if (resource_ret == NRF_SUCCESS) {
        ret_code_t flash_sleep_ret = epd_ext_resources_sleep();
        if (flash_sleep_ret != NRF_SUCCESS) {
            NRF_LOG_WARNING("[EPD]: External resource flash sleep after init failed: %lu",
                            (unsigned long)flash_sleep_ret);
        }
    }

    EPD_GPIO_Uninit();

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

    uint32_t err_code;
    int retries = 50;
    do {
        err_code = sd_ble_gatts_hvx(p_epd->conn_handle, &hvx_params);
        if (err_code == NRF_ERROR_RESOURCES) {
            nrf_delay_ms(5);
            retries--;
        } else {
            break;
        }
    } while (retries > 0);

    return err_code;
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
    // When the low-battery warning screen is active, suppress periodic screen refreshes.
    // We still sample voltage every LOW_BATTERY_POLL_INTERVAL_S seconds so a critically low battery can power off
    // and recovery can exit low-battery mode without forcing a redraw immediately.
    // 低电量警告页激活后，停止周期性屏幕刷新。
    // 仍然按 LOW_BATTERY_POLL_INTERVAL_S 周期采样电压：过低时直接关机，恢复时退出低电量模式，但不立即强制刷新。
    if (p_epd->low_battery_screen_active && !force_update) {
        if (timestamp % LOW_BATTERY_POLL_INTERVAL_S == 0) {
            uint16_t voltage_mv = EPD_ReadVoltageAndCache();
            if (voltage_mv < CRITICAL_BATTERY_SHUTDOWN_MV) {
                NRF_LOG_ERROR("[EPD]: Critical battery detected in low-battery mode (%d mV), showing power-off screen\n", voltage_mv);
                epd_gui_update_event_t event = {p_epd, timestamp};
                uint32_t err_code = app_sched_event_put(&event, sizeof(epd_gui_update_event_t), epd_gui_update);
                if (err_code != NRF_SUCCESS) {
                    NRF_LOG_ERROR("[EPD]: Failed to schedule critical battery power-off screen: 0x%08x\n", err_code);
                    sleep_mode_enter();
                }
                return;
            }
            if (voltage_mv > LOW_BATTERY_THRESHOLD_MV) {
                NRF_LOG_INFO("[EPD]: Battery recovered to %d mV, leaving low-battery mode\n", voltage_mv);
                p_epd->low_battery_screen_active = false;
                p_epd->force_full_refresh = true;
                // Immediately schedule a GUI update to redraw the normal screen with a full refresh
                epd_gui_update_event_t event = {p_epd, timestamp};
                uint32_t err_code = app_sched_event_put(&event, sizeof(epd_gui_update_event_t), epd_gui_update);
                if (err_code != NRF_SUCCESS) {
                    NRF_LOG_ERROR("[EPD]: Failed to schedule recovery screen update: 0x%08x\n", err_code);
                }
                return;
            }
        }

        if (p_epd->low_battery_screen_active) {
            return;
        }
    }

    // 资源更新会话进行中：暂停周期刷新，避免屏幕刷新占用SPI导致资源写入失败。
    // 空闲超过 EPD_RES_SESSION_TIMEOUT_S 秒(上传中断)则自动恢复周期刷新。
    if (p_epd->res_session_active) {
        if ((int32_t)(timestamp - p_epd->res_session_deadline) >= 0) {
            p_epd->res_session_active = false;
            NRF_LOG_INFO("[EPD]: Resource session idle timeout, resuming periodic refresh\n");
        } else if (!force_update) {
            return;
        }
    }

    // ========================================================================
    // 跨天判定链 (Cross-day detection)
    // ========================================================================
    // 设备时间戳按本地时区递增，day = timestamp / 86400。日期发生跨天时，对"显示日期"
    // 的模式(日历 / 时钟+日历)强制一次全刷：
    //   - 正确性：时钟+日历模式平时只对时间做差分局刷，日期/农历不会随分钟刷新更新；
    //     跨天全刷确保午夜后日期、星期、农历、节气、节日立即刷新，不必等下一次整点全刷。
    //   - I/O：跨天后 lunar_db 单日缓存(按年月日)自动失效，新一天的记录重读一次即缓存，
    //     配合单日缓存实现"今日记录严格 1 读/天"。
    // 放在低电量 / 资源会话早退之后：若跨天恰逢这些暂停期，恢复后仍能在下一 tick 补触发。
    static uint32_t s_last_tick_day = 0;
    static bool s_tick_day_inited = false;
    uint32_t current_day = timestamp / 86400U;
    bool day_rolled_over = false;
    if (!s_tick_day_inited) {
        s_tick_day_inited = true;
        s_last_tick_day = current_day;
    } else if (current_day != s_last_tick_day) {
        s_last_tick_day = current_day;
        day_rolled_over = true;
    }

    uint8_t update_policy = GUI_UPDATE_MANUAL;
    if (p_epd->epd != NULL && p_epd->config.display_mode != MODE_PICTURE) {
        update_policy = GUI_GetModeUpdatePolicy((uint8_t)p_epd->config.display_mode,
                                                p_epd->epd->width, p_epd->epd->height);
    }

    // 跨天仅对含日期的模式有意义(纯时钟无日期、图片模式显示用户图像，均不参与)。
    bool day_change_refresh = day_rolled_over &&
                              (p_epd->config.display_mode == MODE_CALENDAR ||
                               p_epd->config.display_mode == MODE_CLOCK_CALENDAR);

    if (force_update ||
        day_change_refresh ||
        (update_policy == GUI_UPDATE_DAILY && timestamp % 86400 == 0) ||
        (update_policy == GUI_UPDATE_MINUTE && timestamp % 60 == 0)) {

        /* PRD §US-3.2: Refresh debounce guard window.
         * If the last physical refresh started less than EPD_REFRESH_DEBOUNCE_S
         * seconds ago, skip this periodic tick to prevent double-refresh
         * after time sync that crosses a minute boundary.
         * force_update (manual mode switch / initial draw) and day_change_refresh
         * are exempt. */
        if (!force_update && !day_change_refresh && p_epd->last_refresh_start != 0) {
            uint32_t elapsed = timestamp - p_epd->last_refresh_start;
            if (elapsed < EPD_REFRESH_DEBOUNCE_S) {
                NRF_LOG_INFO("[EPD]: Refresh debounce: skipping (last refresh %u s ago, guard=%u s)",
                             elapsed, EPD_REFRESH_DEBOUNCE_S);
                return;
            }
        }

        // 跨天走全刷重绘日期/农历，而非仅对时间做差分局刷。
        if (day_change_refresh) {
            p_epd->force_full_refresh = true;
        }
        // 通过app_scheduler调度GUI更新
        // Schedule GUI update via app_scheduler
        epd_gui_update_event_t event = {p_epd, timestamp};
        app_sched_event_put(&event, sizeof(epd_gui_update_event_t), epd_gui_update);
    }
}
