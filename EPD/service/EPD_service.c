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
 * - 使用环形缓冲区(circular buffer)管理命令队列
 * - 支持最多2个普通命令同时排队 (EPD_CMD_QUEUE_SIZE)
 * - 通过app_scheduler异步顺序执行命令
 * - 紧急命令绕过队列；INIT 只登记请求，由主循环初始化并确认完成
 *
 * 工作流程:
 * (1) BLE写入特性 -> on_write() -> epd_service_on_write()
 * (2) 紧急命令直接执行,普通命令入队
 * (3) 如果队列未在处理,通过app_scheduler启动处理
 * (4) epd_process_next_command()从队列取出一个命令执行
 * (5) 执行完毕后,如果队列还有命令,继续调度下一个
 * (6) 队列为空时标记is_processing=false
 *
 * ============================================================================
 * 2. 刷新策略 (epd_refresh_policy + epd_clock_refresh)
 * ============================================================================
 * 何时刷、全/局刷由 policy 决定；局刷窗口由 clock_layout 几何合同决定。
 *
 * ============================================================================
 * 3. 夜间静默逻辑 (Quiet Hours)
 * ============================================================================
 * 时间范围: 01:00 - 05:59
 * 目的: 减少夜间闪烁和功耗
 * 效果: 跳过整点全刷,仅使用局刷
 * 注意: 首次全刷和强制全刷不受夜间静默影响
 *
 * ============================================================================
 * 4. 显示模式管理
 * ============================================================================
 * - MODE_PICTURE (0): 图片模式,用于显示自定义图片
 * - MODE_CALENDAR (1): 日历模式,每天00:00更新
 * - MODE_CLOCK_CALENDAR (2): 时钟+日历模式,每分钟更新时间区域
 * - MODE_CLOCK (3): 时钟模式,每分钟更新
 *
 * ============================================================================
 * 5. BLE服务管理
 * ============================================================================
 * - 处理连接/断开事件
 * - 处理特性写入(命令接收)
 * - 发送通知给对端
 */

#include "EPD_service.h"
#include "epd_service_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ble_srv_common.h"
#include "main.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "sdk_macros.h"
#include "epd_battery_policy.h"
#include "epd_clock_refresh.h"
#include "epd_cmd_policy.h"
#include "epd_refresh_policy.h"
#include "calendar_annotations.h"
#include "calendar_annotations_store.h"
#include "resource_transfer.h"
#include "resource_protocol.h"
#include "resource_store.h"
#include "resource_layout.h"
#include "epd_resources.h"
#include "todo_items.h"

#define LOW_BATTERY_POLL_INTERVAL_S 10  // 低电量模式电压轮询周期(秒)
#define EPD_ANNOTATIONS_RX_TIMEOUT_S 10U
#define EPD_IMAGE_RX_TIMEOUT_S 30U


// ============================================================================
// 命令队列管理函数
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

/**@brief 检查命令是否紧急(需要立即执行)
 *
 * @details 紧急命令绕过队列立即执行。INIT 必须与 WRITE_BLOCK 同级，
 * 否则 GATT 写完成不代表屏已初始化。REFRESH 保持排队，避免堵 BLE。
 */
static bool is_urgent_command(uint8_t cmd) {
    return epd_cmd_is_urgent(cmd);
}

bool resource_blocks_display_command(uint8_t cmd)
{
    return cmd != EPD_CMD_QUERY_STATUS && cmd != EPD_CMD_SYS_RESET && cmd != EPD_CMD_SYS_SLEEP;
}


/* 时钟刷新会睡眠面板并可能清空 p_epd->epd。
 * 仅当连接时的引用已失效时才重新打开 GPIO，再 drv->init。 */
bool epd_ensure_display(ble_epd_t *p_epd)
{
    if (p_epd->epd != NULL) {
        return true;
    }
    EPD_GPIO_Ensure();
    p_epd->epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    return p_epd->epd != NULL;
}

/* 面板 BUSY 错误标志：实例存在时走驱动虚表（新面板可自定义状态）；睡眠后
 * 实例为 NULL、初始化前也要能清理/查询，此时回退 HAL 全局标志（历史语义）。
 * 当前 UC8179 的虚表项与 HAL 标志指向同一状态，两种路径完全等价。 */
void epd_service_busy_error_clear(ble_epd_t *p_epd)
{
    if (p_epd->epd != NULL) {
        epd_panel_clear_busy_error(p_epd->epd); /* 空指针安全的驱动虚表包装 */
    } else {
        EPD_PanelBusyErrorClear();              /* 未实例化：HAL 全局兜底 */
    }
}

bool epd_service_busy_error(const ble_epd_t *p_epd)
{
    if (p_epd->epd != NULL) return epd_panel_busy_error(p_epd->epd);
    return EPD_PanelBusyError();
}

/* WRITE_BLOCK 量大，少打 hexdump。 */
bool is_bulk_data_command(uint8_t cmd) {
    return cmd == EPD_CMD_WRITE_BLOCK || cmd == EPD_CMD_SET_ANNOTATIONS;
}

/* 队列调度统一入口。拆分前所有调用点在同一 TU，LTO 只生成一份
 * app_sched_event_put/epd_work_post 克隆；拆分后按调用点克隆会白烧约 200B。
 * noinline 让调度体只在一处生成（Keil/ARMCC 无 __GNUC__，宏为空）。 */
#if defined(__GNUC__) && !defined(__CC_ARM)
#define EPD_SERVICE_SCHED_NOINLINE __attribute__((noinline))
#else
#define EPD_SERVICE_SCHED_NOINLINE
#endif

EPD_SERVICE_SCHED_NOINLINE void epd_service_schedule_cmd(ble_epd_t *p_epd)
{
    epd_cmd_queue_schedule(&p_epd->cmd_queue, p_epd, epd_process_next_command);
}

EPD_SERVICE_SCHED_NOINLINE void epd_service_schedule_display(ble_epd_t *p_epd)
{
    epd_display_queue_schedule(&p_epd->display_queue, p_epd, epd_gui_update);
}


// ============================================================================
// 命令队列管理函数结束
// ============================================================================

/**@brief BLE连接事件处理函数
 *
 * @details 处理BLE连接事件:
 * 1. 保存连接句柄
 * 2. 主循环取得 GPIO 引用并闪烁 LED
 *
 * @param[in] p_epd  EPD服务结构
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针
 */
static void on_connect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    p_epd->conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
    p_epd->max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3U;
    p_epd->offline.state = EPD_ONLINE;
    /* Main acquires GPIO before accepting display writes. */
}

/**@brief BLE断开事件处理函数
 *
 * @details 处理BLE断开事件:
 * 1. 重置连接句柄
 * 2. 取消连接所属请求，丢弃尚未执行的旧命令
 * 3. 主循环在当前面板操作返回后，清理传图状态、休眠并释放 GPIO
 *
 * @param[in] p_epd  EPD服务结构
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针
 */
static void on_disconnect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    UNUSED_PARAMETER(p_ble_evt);
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    epd_offline_disconnected(&p_epd->offline);
    p_epd->is_notification_enabled = false;
    p_epd->connection_generation++;
    p_epd->config_read_pending = false;
    p_epd->initial_notify = 0U;
    p_epd->mode_ack_pending = false;
    p_epd->picture_entry_pending = false;
    p_epd->bulk_reply_pending = false;
    p_epd->disconnect_pending = true;
    p_epd->init_pending = false;
    p_epd->init_ack_pending = false;
    CRITICAL_REGION_ENTER();
    p_epd->cmd_queue.head = 0;
    p_epd->cmd_queue.tail = 0;
    p_epd->cmd_queue.count = 0;
    CRITICAL_REGION_EXIT();
    calendar_annotations_tx_cancel(&p_epd->annotations_tx);
    if (calendar_annotations_rx_active()) {
        p_epd->annotations_rx_abort_requested = true;
    }


    /* GPIO, panel sleep and image-session cancellation run in main. */

}

/* S-1：首块 WRITE_BLOCK 登记后的图片模式入口，只在主循环执行。
 * 中断里只置 picture_entry_pending 并回 RESTART，避免 UC8179 复位/BUSY
 * 等待（最长 1000 ms，未初始化时还会二次 init）占用 SoftDevice 事件中断。
 * 有刷新/INIT/资源会话/断连时保留 pending，下一轮主循环再试；网页此时
 * 会按 RESTART 语义从块 0 重传。入口执行后成功回 ACK，失败保留可重试。 */
static void epd_picture_entry_process(ble_epd_t *p_epd)
{
    if (!p_epd->picture_entry_pending) return;
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID || p_epd->disconnect_pending ||
        !p_epd->gpio_connection_ref || p_epd->is_refreshing ||
        p_epd->init_pending || p_epd->init_running || p_epd->init_ack_pending ||
        resource_protocol_entered() || resource_transfer_running()) {
        return; /* 保留 pending，等主循环下一次 tick；中断已回 RESTART。 */
    }
    if (p_epd->epd != NULL &&
        !epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        p_epd->picture_entry_pending = false;
        return; /* 已由其它主循环路径完成入口（幂等），无需重复 init。 */
    }
    EPD_ClearBusyAbort();
    epd_service_busy_error_clear(p_epd);
    if (!epd_ensure_display(p_epd)) {
        p_epd->force_full_refresh = true;
        return; /* 保留 pending：下一轮主循环重试，网页重发块 0 也不丢请求。 */
    }
    epd_enter_picture_mode(p_epd);
    /* 入口期间保持 pending：中断里的块 0 继续回 RESTART（不会重复登记），
     * 完成后再放行正常块写入。 */
    p_epd->picture_entry_pending = false;
    if (epd_service_busy_error(p_epd)) {
        p_epd->force_full_refresh = true; /* 静默重试：块 0 重传会重新走入口。 */
    }
}

/* The sole INIT worker runs in main, after a refresh returns. */
static void epd_init_process(ble_epd_t *p_epd)
{
    if (p_epd->disconnect_pending || !p_epd->gpio_connection_ref ||
        p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) return;
    if (p_epd->init_pending && p_epd->init_prior_commands == 0U &&
        !p_epd->init_ack_pending && !p_epd->is_refreshing) {
        uint32_t generation;
        bool connected;
        uint8_t model, token;
        CRITICAL_REGION_ENTER();
        generation = p_epd->connection_generation;
        connected = !p_epd->disconnect_pending && p_epd->conn_handle != BLE_CONN_HANDLE_INVALID;
        model = p_epd->init_model;
        token = p_epd->init_token;
        p_epd->init_running = true;
        p_epd->init_pending = false;
        p_epd->is_refreshing = true;
        CRITICAL_REGION_EXIT();
        if (!connected) {
            p_epd->init_running = false;
            p_epd->is_refreshing = false;
            return;
        }
        EPD_ClearBusyAbort();
        epd_service_busy_error_clear(p_epd);
        p_epd->epd = epd_init((epd_model_id_t)model);
        p_epd->init_status = epd_service_busy_error(p_epd) ? 1U : 0U;
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        if (p_epd->epd->id != p_epd->config.model_id) {
            p_epd->config.model_id = p_epd->epd->id;
            epd_config_write(&p_epd->config);
        }
        p_epd->is_refreshing = false;
        p_epd->init_running = false;
        CRITICAL_REGION_ENTER();
        if (generation == p_epd->connection_generation) {
            p_epd->init_ack_token = token;
            p_epd->init_ack_pending = true;
        }
        CRITICAL_REGION_EXIT();
        if (generation == p_epd->connection_generation) {
            epd_send_mtu(p_epd);
            epd_send_time(p_epd);
            epd_send_voltage(p_epd);
        }
    }
    if (p_epd->init_ack_pending) {
        uint8_t response[] = {EPD_RSP_INIT, p_epd->init_ack_token, p_epd->init_status,
                             (uint8_t)p_epd->max_data_len, (uint8_t)(p_epd->max_data_len >> 8)};
        uint32_t err = ble_epd_notification_try_send(p_epd, response, sizeof(response));
        if (err != NRF_ERROR_RESOURCES) p_epd->init_ack_pending = false;
    }
}

/* Connection-owned GPIO and abandoned transfers are retired in main only. */
static void epd_connection_process(ble_epd_t *p_epd)
{
    bool refresh = false;
    if (p_epd->disconnect_pending) {
        if (p_epd->epd != NULL) {
            EPD_ClearBusyAbort();
            p_epd->epd->drv->sleep(p_epd->epd);
            p_epd->epd = NULL;
        }
        if (p_epd->gpio_connection_ref) {
            EPD_GPIO_Uninit();
            p_epd->gpio_connection_ref = false;
        }
        CRITICAL_REGION_ENTER();
        refresh = true; /* Apply time/mode/resource changes after maintenance. */
        memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
        p_epd->picture_entry_pending = false;
        p_epd->bulk_reply_pending = false;
        p_epd->disconnect_pending = false;
        CRITICAL_REGION_EXIT();
    }
    CRITICAL_REGION_ENTER();
    if (p_epd->transfer_ctx.transfer_active &&
        (uint32_t)(app_uptime_seconds() - p_epd->image_last_activity) >= EPD_IMAGE_RX_TIMEOUT_S) {
        memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
        refresh = true;
    }
    CRITICAL_REGION_EXIT();
    if (refresh) {
        EPD_ClearBusyAbort();
        epd_clock_refresh_invalidate();
        p_epd->force_full_refresh = true;
        if (p_epd->config.display_mode != MODE_PICTURE) epd_display_request(p_epd, timestamp());
    }
    if (p_epd->conn_handle != BLE_CONN_HANDLE_INVALID && !p_epd->gpio_connection_ref) {
        EPD_GPIO_Init();
        p_epd->gpio_connection_ref = true;
        EPD_LED_BLINK();
    }
}

static bool epd_resource_idle(const ble_epd_t *p_epd)
{
    return !p_epd->is_refreshing && !p_epd->init_pending && !p_epd->init_running &&
           !p_epd->init_ack_pending && !p_epd->mode_ack_pending && !p_epd->initial_notify &&
           !p_epd->cmd_queue.count &&
           !p_epd->cmd_queue.is_processing && !p_epd->transfer_ctx.transfer_active &&
           !p_epd->annotations_tx.active &&
           !epd_config_busy() && calendar_annotations_store_maintenance_ready();
}

/* A temporary boot read failure uses defaults only in RAM. Recover the saved
 * configuration when main is idle, unless a newer user setting superseded it. */
static void epd_config_restore(ble_epd_t *p_epd)
{
    if (!epd_config_read_pending() || !epd_resource_idle(p_epd)) return;
    epd_config_t candidate;
    p_epd->is_refreshing = true; /* Exclude IRQ image writes during Flash/GPIO work. */
    if (epd_config_retry_read(&candidate)) {
        if (!epd_config_validate(&candidate)) {
            epd_config_defaults(&candidate);
            epd_config_write(&candidate);
        }
        (void)epd_config_apply(p_epd, &candidate);
        p_epd->force_full_refresh = true;
        epd_display_retry_reset();
        if (p_epd->config.display_mode != MODE_PICTURE)
            epd_display_request(p_epd, timestamp());
    }
    p_epd->is_refreshing = false;
}

static rs_result_t epd_resource_event(void *context, resource_event_t event, uint16_t changed)
{
    ble_epd_t *p_epd = context;
    if (event == RESOURCE_ENTER) return epd_resource_idle(p_epd) ? RS_OK : RS_BUSY;
    if (event == RESOURCE_INVALIDATE) {
        epd_resources_invalidate_partitions(changed);
        epd_clock_refresh_invalidate();
        if (changed & ((1U << RS_PART_PARTITION_TABLE) | (1U << RS_PART_TODO_ITEMS)))
            todo_items_suspend_display();
        else
            todo_items_invalidate();
        return RS_OK;
    }
    if (event == RESOURCE_OFFLINE) {
        if (!epd_resource_idle(p_epd)) return RS_BUSY;
        p_epd->offline.state = EPD_OFFLINE_ACK;
        return RS_OK;
    }
    /* A table replacement also changes the record locators. Never keep an old
     * RAM config/annotation view after installing factory or migrated records. */
    if (changed & ((1U << RS_PART_PARTITION_TABLE) | (1U << RS_PART_EPD_CONFIG))) {
        epd_config_t config;
        epd_config_read(&config);
        if (epd_config_read_pending()) return RS_IO;
        if (!epd_config_apply(p_epd, &config)) return RS_INVALID;
    }
    if (changed & ((1U << RS_PART_PARTITION_TABLE) | (1U << RS_PART_CALENDAR_ANNOTATIONS))) {
        calendar_annotations_store_init();
        if (!calendar_annotations_store_valid()) return RS_INVALID;
    }
    if ((changed & ((1U << RS_PART_PARTITION_TABLE) | (1U << RS_PART_TODO_ITEMS))) ||
        (epd_is_todo_mode((display_mode_t)p_epd->config.display_mode) && !todo_items_ready())) {
        /* Retry a previously failed boot/activation read in main context.
         * A valid unchanged list stays cached; optional TODO damage does not
         * block activation of the other display modes. */
        if (!todo_items_init()) return RS_INVALID;
    }
    /* LEAVE already validated unchanged resources; RUN_OFFLINE can reuse them. */
    rs_result_t result = !changed && epd_resources_ready() ? RS_OK : epd_resources_apply();
    if (result == RS_OK && !epd_todo_font_valid()) return RS_INVALID;
    if (result == RS_OK) {
        epd_display_retry_reset();
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        if (p_epd->config.display_mode != MODE_PICTURE) epd_display_request(p_epd, timestamp());
    }
    /* Direct picture operation does not require installed GUI resources. */
    return !changed && p_epd->config.display_mode == MODE_PICTURE ? RS_OK : result;
}

bool ble_epd_take_offline_request(ble_epd_t *p_epd)
{
    if (p_epd->offline.state != EPD_OFFLINE_READY || !epd_resource_idle(p_epd)) return false;
    p_epd->offline.state = EPD_OFFLINE_ACTIVE;
    return true;
}

void ble_epd_process(ble_epd_t* p_epd) {
    epd_connection_process(p_epd);
    epd_init_process(p_epd);
    epd_picture_entry_process(p_epd);
    epd_battery_process(p_epd);
    resource_protocol_set_data_limit(p_epd->max_data_len);
    resource_transfer_process(p_epd, p_epd->connection_generation,
        p_epd->conn_handle != BLE_CONN_HANDLE_INVALID && !p_epd->disconnect_pending,
        epd_resource_idle(p_epd), app_uptime_seconds(), resource_notification_send);
    if (p_epd->annotations_rx_abort_requested) {
        (void)calendar_annotations_rx_cancel();
        CRITICAL_REGION_ENTER();
        p_epd->annotations_rx_abort_requested = false;
        p_epd->annotations_rx_last_activity = 0U;
        CRITICAL_REGION_EXIT();
    }
    if (!resource_protocol_entered()) {
        epd_config_restore(p_epd);
        epd_config_process();
        calendar_annotations_store_process();
    }
    epd_control_notifications_process(p_epd);
    epd_bulk_reply_process(p_epd);
    epd_record_ack_process(p_epd);
    epd_config_readback_process(p_epd);
    calendar_annotations_tx_process(&p_epd->annotations_tx, p_epd->max_data_len,
                                    annotations_tx_send, p_epd);
    if (epd_config_clear_complete()) NVIC_SystemReset();
    epd_service_schedule_cmd(p_epd);
    epd_service_schedule_display(p_epd);
}

/**@brief 服务写入处理包装函数(将命令入队或执行)
 *
 * @details 此函数是BLE写入的入口点:
 * 1. 紧急命令绕过队列立即执行
 * 2. 普通命令添加到队列
 * 3. 如果队列未在处理,启动队列处理
 *
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] p_data  命令数据指针
 * @param[in] length  命令数据长度
 */
static void epd_service_on_write(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    if (p_data == NULL || length == 0) return;
    if (p_epd->offline.state != EPD_ONLINE &&
        !(length >= 3U && p_data[0] == EPD_CMD_RESOURCE && p_data[1] == 8U)) return;
    if (p_data[0] == EPD_CMD_GET_CONFIG) {
        if (length == 2U && !p_epd->config_read_pending) {
            p_epd->config_read_token = p_data[1];
            p_epd->config_read_pending = true;
        }
        return;
    }
    if (p_data[0] == EPD_CMD_RESOURCE) {
        resource_transfer_receive(p_data, length, p_epd->connection_generation);
        return;
    }
    /* Resource SPI work and maintenance sessions exclude display/config writes. */
    if ((resource_protocol_entered() || resource_transfer_running()) &&
        resource_blocks_display_command(p_data[0])) {
        epd_command_ack(p_epd, p_data, length, RS_BUSY);
        return;
    }
    if (!is_bulk_data_command(p_data[0])) {
        NRF_LOG_DEBUG("[EPD]: on_write received command 0x%02x, LEN=%d\n", p_data[0], length);
    }

    if (p_data[0] != EPD_CMD_WRITE_BLOCK) {
        ble_log("[CMD]:%02x/%d", p_data[0], length);
    }


    // 紧急命令绕过队列立即执行
    if (is_urgent_command(p_data[0]) || epd_cmd_is_immediate(p_data[0])) {
        if (!is_bulk_data_command(p_data[0])) {
            NRF_LOG_INFO("[EPD]: Immediate command 0x%02x, bypassing queue\n", p_data[0]);
        }
        epd_execute_command(p_epd, p_data, length);
        return;
    }

    if (epd_cmd_queue_push(&p_epd->cmd_queue, p_data, length)) {
        epd_service_schedule_cmd(p_epd);
    } else {
        epd_command_ack(p_epd, p_data, length, RS_BUSY);
    }
}

/**@brief BLE GATTS写入事件处理函数
 *
 * @details 处理两种类型的写入:
 * 1. CCCD写入:启用/禁用通知,启用时发送配置
 * 2. 特性值写入:调用命令处理函数
 *
 *
 * @param[in] p_epd  EPD服务结构
 * @param[in] p_ble_evt 从BLE协议栈接收的事件指针
 */
static void on_write(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    ble_gatts_evt_write_t* p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;

    // 检查是否是CCCD写入(通知启用/禁用)
    if ((p_evt_write->handle == p_epd->char_handles.cccd_handle) && (p_evt_write->len == 2)) {
        if (ble_srv_is_notification_enabled(p_evt_write->data)) {
            // 通知已启用,发送配置给对端
            NRF_LOG_DEBUG("notification enabled\n");
            p_epd->is_notification_enabled = true;
            ble_log("[SYS]:mtu=%d rst=0x%X", p_epd->max_data_len, get_reset_reason());
            p_epd->initial_notify = 1U;
        } else {
            p_epd->is_notification_enabled = false;
            p_epd->initial_notify = 0U;
            p_epd->mode_ack_pending = false;
            p_epd->bulk_reply_pending = false;
        }
    } else if (p_evt_write->handle == p_epd->char_handles.value_handle) {
        // 特性值写入,处理命令
        epd_service_on_write(p_epd, p_evt_write->data, p_evt_write->len);
    } else {
        // 不做任何事。此事件与此服务无关。
    }
}

/**@brief nRF52811的BLE事件处理函数
 *
 * @param[in] p_ble_evt  BLE事件指针
 * @param[in] p_context  上下文指针(EPD服务结构)
 */
void ble_epd_evt_handler(ble_evt_t const* p_ble_evt, void* p_context) {
    if (p_context == NULL || p_ble_evt == NULL) return;

    ble_epd_t* p_epd = (ble_epd_t*)p_context;
    ble_epd_on_ble_evt(p_epd, (ble_evt_t*)p_ble_evt);
}

/**@brief BLE事件处理函数
 *
 * @details 分发BLE事件到相应的处理函数:
 * - BLE_GAP_EVT_CONNECTED: 连接事件
 * - BLE_GAP_EVT_DISCONNECTED: 断开事件
 * - BLE_GATTS_EVT_WRITE: 写入事件
 *
 * @param[in] p_epd  EPD服务结构
 * @param[in] p_ble_evt BLE事件指针
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

        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            epd_offline_completed(&p_epd->offline, p_ble_evt->evt.gatts_evt.params.hvn_tx_complete.count);
            /* Notification workers use a single non-blocking HVX attempt.
             * Retry only after SoftDevice reports that TX queue space was
             * released, so BLE event handling never sleeps on resources. */
            calendar_annotations_tx_wake(&p_epd->annotations_tx);
            break;

        default:
            // 无需实现。
            break;
    }
}

/**@brief 初始化BLE服务(内部函数)
 *
 * @details 初始化BLE服务,包括:
 * 1. 注册自定义UUID
 * 2. 添加主服务
 * 3. 添加EPD特性(可写、可通知)
 * 4. 添加应用版本特性(只读)
 *
 *
 * @param[in] p_epd  EPD服务结构指针
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
static uint32_t epd_service_init(ble_epd_t* p_epd) {
    ble_uuid_t ble_uuid = {0};
    ble_uuid128_t base_uuid = BLE_UUID_EPD_SVC_BASE;
    ble_add_char_params_t add_char_params;
    uint8_t app_version = APP_VERSION;

    // 注册自定义UUID
    VERIFY_SUCCESS(sd_ble_uuid_vs_add(&base_uuid, &ble_uuid.type));

    // 添加主服务
    ble_uuid.uuid = BLE_UUID_EPD_SVC;
    VERIFY_SUCCESS(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid, &p_epd->service_handle));

    // 添加EPD特性(可写、可通知)
    memset(&add_char_params, 0, sizeof(add_char_params));
    add_char_params.uuid = BLE_UUID_EPD_CHAR;
    add_char_params.uuid_type = ble_uuid.type;
    add_char_params.max_len = BLE_EPD_MAX_DATA_LEN;
    add_char_params.init_len = sizeof(uint8_t);
    add_char_params.is_var_len = true;
    add_char_params.char_props.notify = 1;  // 支持通知
    add_char_params.char_props.write = 1;  // 支持写入(带响应)
    add_char_params.char_props.write_wo_resp = 1;  // 支持写入(无响应)
    add_char_params.read_access = SEC_OPEN;
    /* This is the shared, historically unpaired Web Bluetooth command
     * characteristic (including INIT). Requiring encryption here also forces
     * pairing before any ordinary command can run. Font CRC/transactions do
     * not require changing the transport's security contract. */
    add_char_params.write_access = SEC_OPEN;
    add_char_params.cccd_write_access = SEC_OPEN;

    VERIFY_SUCCESS(characteristic_add(p_epd->service_handle, &add_char_params, &p_epd->char_handles));

    // 添加应用版本特性(只读)
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
 *
 * @details 进入睡眠模式前的准备:
 * 1. 关闭LED
 * 2. 配置唤醒引脚
 *
 *
 * @param[in] p_epd  EPD服务结构指针
 */
void ble_epd_sleep_prepare(ble_epd_t* p_epd) {
    EPD_LED_OFF();
    // 配置唤醒脚
    if (p_epd->config.wakeup_pin != 0xFF) {
        nrf_gpio_cfg_sense_input(p_epd->config.wakeup_pin, NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_HIGH);
    }
}

/**@brief 初始化EPD服务
 *
 * @details 初始化EPD服务的完整流程:
 * 1. 初始化服务结构
 * 2. 初始化命令队列
 * 3. 加载或创建默认配置
 * 4. 初始化GPIO和EPD驱动
 * 5. 添加BLE服务
 *
 *
 * @param[in] p_epd  EPD服务结构指针
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
uint32_t ble_epd_init(ble_epd_t* p_epd) {
    if (p_epd == NULL) return NRF_ERROR_NULL;

    /* Own initialization here so callers need not pre-clear the service. */
    memset(p_epd, 0, sizeof(*p_epd));
    p_epd->max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3U;
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    epd_display_queue_init(&p_epd->display_queue);
    calendar_annotations_tx_cancel(&p_epd->annotations_tx);
    epd_cmd_queue_init(&p_epd->cmd_queue);
    resource_protocol_set_handler(epd_resource_event, p_epd);

    // 初始化并读取配置
    uint32_t config_err = epd_config_init(&p_epd->config);
    if (config_err != NRF_SUCCESS) return config_err;
    epd_config_read(&p_epd->config);
    epd_annotations_set_service(p_epd);
    calendar_annotations_store_init();
    calendar_annotations_store_on_applied(annotations_store_applied);
    (void)todo_items_init(); /* An absent optional partition is an empty list. */

    // Invalid persisted fields recover together, using the same validation as updates.
    if (!epd_config_validate(&p_epd->config)) {
        epd_config_defaults(&p_epd->config);
        if (!epd_config_read_pending()) epd_config_write(&p_epd->config);
    }

    // 加载配置
    EPD_GPIO_Load(&p_epd->config);

    // 添加BLE服务
    return epd_service_init(p_epd);
}

/**@brief 通过BLE发送字符串
 *
 * @details 将字符串作为通知发送给BLE对端。
 *
 * @param[in] p_epd    EPD服务结构指针
 * @param[in] p_string 要发送的字符串
 * @param[in] length   字符串长度
 *
 * @return 成功返回NRF_SUCCESS,否则返回错误代码
 */
uint32_t ble_epd_notification_try_send(ble_epd_t* p_epd,
                                               uint8_t* p_string,
                                               uint16_t length)
{
    ble_gatts_hvx_params_t hvx_params;

    if (p_epd == NULL ||
        (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) ||
        (!p_epd->is_notification_enabled)) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (p_string == NULL || length > p_epd->max_data_len) {
        return NRF_ERROR_INVALID_PARAM;
    }

    memset(&hvx_params, 0, sizeof(hvx_params));
    hvx_params.handle = p_epd->char_handles.value_handle;
    hvx_params.p_data = p_string;
    hvx_params.p_len = &length;
    hvx_params.type = BLE_GATT_HVX_NOTIFICATION;
    uint32_t result;
    CRITICAL_REGION_ENTER();
    result = sd_ble_gatts_hvx(p_epd->conn_handle, &hvx_params);
    if (result == NRF_SUCCESS) {
        bool final_ack = p_epd->offline.state == EPD_OFFLINE_ACK && length >= 4U &&
                         p_string[0] == EPD_RSP_RESOURCE && p_string[1] == 8U &&
                         p_string[3] == RS_OK;
        epd_offline_queued(&p_epd->offline, final_ack);
    }
    CRITICAL_REGION_EXIT();
    return result;
}

uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length) {
    /* Notifications are best-effort here.  Retrying synchronously can block
     * the BLE/application event path for 250 ms when the HVX queue is full.
     * Font-specific responses already use the scheduler/TX_COMPLETE path. */
    return ble_epd_notification_try_send(p_epd, p_string, length);
}

/**@brief 定时器事件处理函数,触发GUI更新
 *
 * @details 定期调用以根据当前模式更新显示:
 * - MODE_CALENDAR: 每天00:00:00更新 (timestamp % 86400 == 0)
 * - MODE_CLOCK: 每分钟更新 (timestamp % 60 == 0)
 * - MODE_CLOCK_CALENDAR: 每分钟更新 (timestamp % 60 == 0)
 * - force_update为true时强制更新
 *
 * @param[in] p_epd         EPD服务结构指针
 * @param[in] timestamp     当前Unix时间戳
 * @param[in] force_update  为true时强制更新
 */
void ble_epd_on_timer(ble_epd_t* p_epd, uint32_t timestamp, bool force_update) {
    if (resource_protocol_entered()) return;
    if (force_update || p_epd->config.display_mode == MODE_PICTURE)
        epd_display_retry_reset();
    if (calendar_annotations_rx_active() &&
        (uint32_t)(app_uptime_seconds() - p_epd->annotations_rx_last_activity) >=
            EPD_ANNOTATIONS_RX_TIMEOUT_S) {
        p_epd->annotations_rx_abort_requested = true;
    }

    if (p_epd->low_battery_screen_active && !force_update) {
        if (timestamp % LOW_BATTERY_POLL_INTERVAL_S == 0) {
            p_epd->battery_poll_pending = true;
        }

        if (epd_battery_should_blink_led(true, false, timestamp)) {
            p_epd->battery_blink_pending = true;
        }

        // 低电量模式下不进行周期刷新，避免屏幕刷新占用SPI导致资源写入失败
        /* A failed last frame is still outstanding work. Its uptime backoff
         * below limits retries; a successful frame restores the battery hold. */
        if (!epd_display_retry_scheduled()) {
            return;
        }
    }

    /* Explicit boot/user refreshes may show the missing-resource fallback.
     * Periodic work must wait for a successful activation: retrying an erased
     * font/layout cannot repair it and can starve BLE recovery commands. */
    if ((!force_update && !epd_resources_ready()) || p_epd->transfer_ctx.transfer_active ||
        (p_epd->is_refreshing && !force_update)) return;

    // ========================================================================
    // 跨天判定链 (Cross-day detection)
    // ========================================================================
    // 设备时间戳按本地时区递增，day = timestamp / 86400。日期发生跨天时，对默认
    // 日历/时钟模板统一强制一次全刷：时钟模式平时只对时间做差分局刷，日期/农历
    // 不会随分钟刷新更新；跨天全刷确保午夜后日期、星期、农历立即刷新。
    // 放在低电量早退之后：若跨天恰逢暂停期，恢复后仍能在下一 tick 补触发。
    static uint32_t s_last_tick_day = 0;
    static bool s_tick_day_inited = false;
    uint32_t current_day = timestamp / 86400U;
    bool day_rolled_over = false;
    epd_refresh_schedule_in_t schedule_in;
    epd_refresh_schedule_out_t decision;

    if (!s_tick_day_inited) {
        s_tick_day_inited = true;
        s_last_tick_day = current_day;
    } else if (current_day != s_last_tick_day) {
        s_last_tick_day = current_day;
        day_rolled_over = true;
    }

    if (day_rolled_over) epd_display_retry_reset();
    if (!force_update && epd_display_retry_waiting())
        return;

    /* A delayed worker may already have drawn today before the next tick.
     * Do not repeat that successful midnight refresh when the tick catches up. */
    if (p_epd->last_full_clear != 0U &&
        p_epd->last_full_clear / 86400U == current_day) day_rolled_over = false;

    if (epd_display_retry_scheduled() &&
        (!epd_is_todo_mode((display_mode_t)p_epd->config.display_mode) || todo_items_ready()))
        force_update = true;
    if (p_epd->config.display_mode == MODE_CALENDAR &&
        p_epd->last_full_clear / 86400U != current_day) {
        force_update = true;
        p_epd->force_full_refresh = true;
    }

    if (epd_is_todo_mode((display_mode_t)p_epd->config.display_mode) && todo_items_ready() &&
        (p_epd->force_full_refresh ||
         (p_epd->config.display_mode == MODE_TODO_CALENDAR &&
          p_epd->last_full_clear / 86400U != current_day) ||
         todo_items_selection_changed(timestamp))) {
        /* Rollover/config/forced redraws can fail without changing the task
         * selection. The split calendar also compares the successfully drawn
         * day, so an old queued frame cannot consume its only rollover tick.
         * Keep retries behind the uptime backoff until BUSY succeeds. */
        force_update = true;
    }
    schedule_in = (epd_refresh_schedule_in_t){
        .mode = (display_mode_t)p_epd->config.display_mode,
        .timestamp = timestamp,
        .force_update = force_update,
        .low_battery_hold = false,
        .day_rolled = day_rolled_over,
        .last_refresh_start = p_epd->last_refresh_start,
        .transfer_active = p_epd->transfer_ctx.transfer_active,
    };
    decision = epd_refresh_schedule(&schedule_in);
    if (!decision.should_refresh) {
        return;
    }
    if (decision.force_full) {
        p_epd->force_full_refresh = true;
    }
    epd_display_request(p_epd, timestamp);
}
