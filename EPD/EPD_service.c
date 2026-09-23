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
#include "UC8179.h"
#include "epd_battery_policy.h"
#include "epd_clock_refresh.h"
#include "epd_cmd_policy.h"
#include "epd_refresh_policy.h"
#include "calendar_annotations.h"
#include "calendar_annotations_store.h"
#include "font_store.h"
#include "font_transfer.h"
#include "resource_transfer.h"
#include "resource_protocol.h"
#include "resource_store.h"
#include "resource_layout.h"

static void epd_enter_picture_mode(ble_epd_t *p_epd); /* 切图片模式 */
static void epd_gui_update(void* p_event_data, uint16_t event_size);

// ============================================================================
// CRC16计算和传输响应函数
// ============================================================================

#define LOW_BATTERY_POLL_INTERVAL_S 10  // 低电量模式电压轮询周期(秒)
#define EPD_FONT_REFRESH_DELAY_S 5U     // 连续上传多个槽位时合并一次全屏刷新
#define EPD_ANNOTATIONS_RX_TIMEOUT_S 10U
#define EPD_IMAGE_RX_TIMEOUT_S 30U

// SET_TIME 频率限制(秒)。同一时间窗口内仅允许一次 SET_TIME 触发屏幕更新(PRD §4.3)。
#define EPD_SET_TIME_RATE_LIMIT_S 10U

static void epd_display_request(ble_epd_t *p_epd, uint32_t requested_timestamp)
{
    epd_display_queue_request(&p_epd->display_queue, requested_timestamp);
    epd_display_queue_schedule(&p_epd->display_queue, p_epd, epd_gui_update);
}

static void epd_display_complete(ble_epd_t *p_epd)
{
    epd_display_queue_finish(&p_epd->display_queue);
    epd_display_queue_schedule(&p_epd->display_queue, p_epd, epd_gui_update);
}

static uint32_t epd_elapsed_ms(uint32_t start_ticks)
{
    const uint32_t ticks_per_ms =
        (APP_TIMER_CLOCK_FREQ / (APP_TIMER_CONFIG_RTC_FREQUENCY + 1U)) / 1000U;
    const uint32_t elapsed_ticks = app_timer_cnt_diff_compute(app_timer_cnt_get(), start_ticks);

    return (ticks_per_ms == 0U) ? 0U : elapsed_ticks / ticks_per_ms;
}

/**@brief 计算CRC16-CCITT校验值
 *
 * @details 使用多项式0x8408 (反转的0x1021)
 * 用于验证BLE传输数据的完整性
 *
 * @param[in] data  数据指针
 * @param[in] len  数据长度
 *
 * @return CRC16校验值
 */
uint16_t crc16_compute(const uint8_t* data, uint16_t len) {
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
 *
 * @details 响应格式: [0xA0, block_id_L, block_id_H, status]
 * status: 0x00 = 成功(ACK), 0x01 = 失败(NACK,需重传)
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] block_id  块ID
 * @param[in] status  状态码(0x00=成功, 0x01=CRC错误)
 */
static void send_block_response(ble_epd_t* p_epd, uint16_t block_id, uint8_t status) {
    uint8_t rsp[] = {EPD_RSP_BLOCK_ACK, block_id & 0xFF, block_id >> 8, status};
    ble_epd_string_send(p_epd, rsp, sizeof(rsp));
}

/**@brief 发送传输状态响应到APP
 *
 * @details 响应格式: [0xA1, total_L, total_H, received_L, received_H, session_id, active]
 *
 * @param[in] p_epd  EPD服务结构指针
 */
static void send_transfer_status(ble_epd_t* p_epd) {
    /* [0xA1, total_L, total_H, received_L, received_H, session_id, active] */
    uint8_t rsp[7];
    rsp[0] = EPD_RSP_STATUS;
    rsp[1] = p_epd->transfer_ctx.total_blocks & 0xFF;
    rsp[2] = p_epd->transfer_ctx.total_blocks >> 8;
    rsp[3] = p_epd->transfer_ctx.received_blocks & 0xFF;
    rsp[4] = p_epd->transfer_ctx.received_blocks >> 8;
    rsp[5] = p_epd->transfer_ctx.session_id;
    rsp[6] = p_epd->transfer_ctx.transfer_active ? 1 : 0;

    ble_epd_string_send(p_epd, rsp, sizeof(rsp));
    NRF_LOG_INFO("[EPD]: Transfer status sent: total=%d, received=%d, session=%d, active=%d\n",
                 p_epd->transfer_ctx.total_blocks, p_epd->transfer_ctx.received_blocks,
                 p_epd->transfer_ctx.session_id, p_epd->transfer_ctx.transfer_active);
}

// nRF52811默认引脚配置
#define EPD_CFG_52811 {0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02, 0x07, 0xFF, 0x12, 0x07}




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

#if FONT_STORE_MUTABLE || RESOURCE_STORE_WRITABLE
static bool font_blocks_display_command(uint8_t cmd)
{
    return epd_cmd_needs_display(cmd) ||
           cmd == EPD_CMD_INIT ||
           cmd == EPD_CMD_SLEEP ||
           cmd == EPD_CMD_SET_TIME ||
           cmd == EPD_CMD_SET_MODE ||
           cmd == EPD_CMD_SET_ANNOTATIONS;
}

#if FONT_STORE_MUTABLE
static void epd_report_font_busy(ble_epd_t* p_epd,
                                  const uint8_t* p_data,
                                  uint16_t length)
{
    if (p_epd == NULL || p_data == NULL || length == 0U) {
        return;
    }
    /* WRITE_BLOCK has an application-level ACK path.  Tell the image
     * transfer to restart instead of silently dropping the packet and
     * leaving its sender waiting for a response that can never arrive. */
    if (p_data[0] == EPD_CMD_WRITE_BLOCK && length >= 3U) {
        uint16_t block_id = (uint16_t)p_data[1] | ((uint16_t)p_data[2] << 8);
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
    }
}
#endif
#endif


/* 时钟刷新会睡眠面板并可能清空 p_epd->epd。
 * 仅当连接时的引用已失效时才重新打开 GPIO，再 drv->init。 */
static bool epd_ensure_display(ble_epd_t *p_epd)
{
    if (p_epd->epd != NULL) {
        return true;
    }
    EPD_GPIO_Ensure();
    p_epd->epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    return p_epd->epd != NULL;
}

/* WRITE_BLOCK 量大，少打 hexdump。 */
static bool is_bulk_data_command(uint8_t cmd) {
    return cmd == EPD_CMD_WRITE_BLOCK || cmd == EPD_CMD_SET_ANNOTATIONS || cmd == EPD_CMD_FONT_STORE;
}

// 异步命令处理的前向声明
static void epd_process_next_command(void* p_event_data, uint16_t event_size);

// ============================================================================
// 命令队列管理函数结束
// ============================================================================

/**@brief 资源缺失提示屏——GFX 分页回调：把每页黑平面顺序流式写入已打开的 DTM1 数据流。
 *
 * @details 关键：**不使用 PTIN/局部窗口(PTL)寻址**。真机多轮验证表明，全刷 `write_image`
 * 走 PTIN+`_setPartialRamArea` 的逐页局部窗口寻址，在"空 Flash 上电"这种上下文里
 * 极不稳定(白屏/红黑白噪点/无反应)；而 `UC8179_Clear` 用的"DTM1/DTM2 顺序写整帧
 * (不带 PTIN)"是可靠的(clear(true) 真机能出干净白屏)。
 * 本回调即复用该顺序写方式：调用前已 `EPD_WriteCmd(DTM1)`(复位 RAM 写指针到 0)，
 * 这里按页把黑平面数据连续写入，各页拼接正好铺满整帧(与 EPD_FillRAM 同构)。
 * 红平面(DTM2)由外层统一填 0xFF(无红)，故此处忽略 color。
 */
/* skip：临界电压下已画过最后一帧正常画面，不再刷新。 */
typedef struct {
    bool skip;
    bool low_battery;    /* <= 2.80 V：保留布局，暂停周期刷新 */
    bool critical;       /* < 2.70 V：最后一帧正常画面后关机 */
    uint16_t voltage_mv;
} battery_guard_result_t;

/* 读电压并判定低电保持 / 临界关机。 */
static battery_guard_result_t epd_battery_guard(ble_epd_t *p_epd) {
    static uint16_t cached_mv = 3300;
    static bool power_off_drawn = false;
    cached_mv = EPD_ReadVoltageAndCache();

    bool low      = epd_battery_is_low(cached_mv);
    bool critical = epd_battery_is_critical(cached_mv);
    p_epd->low_battery_screen_active = low;

    battery_guard_result_t r = {false, low, critical, cached_mv};

    if (critical) {
        if (power_off_drawn) {
            NRF_LOG_WARNING("[EPD]: Critical battery already handled, skipping redundant update\n");
            r.skip = true;
            return r;
        }
        power_off_drawn = true;
    } else {
        power_off_drawn = false;
    }

    if (low) {
        NRF_LOG_WARNING("[EPD]: Low battery (%d mV), holding periodic refresh\n", cached_mv);
    }
    return r;
}

/* 全刷：init → 绘整屏 → refresh。中途传图则让出。 */
static bool epd_perform_full_refresh(ble_epd_t *p_epd, const epd_model_t *epd,
                                     gui_data_t *data) {
    uint32_t phase_start;

    if (p_epd == NULL || epd == NULL || epd->drv == NULL || data == NULL ||
        epd->drv->init == NULL || epd->drv->write_image == NULL ||
        epd->drv->refresh == NULL) {
        if (p_epd != NULL) p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        return false;
    }

    epd_clock_refresh_invalidate();
    UC8179_ClearBusyTimeout();
    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active)) {
        return false;
    }
    phase_start = app_timer_cnt_get();
    epd->drv->init(epd);
    NRF_LOG_INFO("[EPD]: full init phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:init=%ums", epd_elapsed_ms(phase_start));
    if (UC8179_HadBusyTimeout() || EPD_BusyAbortRequested()) {
        NRF_LOG_ERROR("[EPD]: BUSY timeout during full refresh init\n");
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        return false;
    }

    if (epd->drv->read_temp != NULL) {
        data->temperature = epd->drv->read_temp(epd);
    }

    app_feed_wdt();
    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active)) {
        return false;
    }
    phase_start = app_timer_cnt_get();
    DrawGUI(data, (buffer_callback)epd->drv->write_image, (void *)epd);
    NRF_LOG_INFO("[EPD]: GUI draw phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:draw=%ums", epd_elapsed_ms(phase_start));
    app_feed_wdt();
    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active)) {
        return false;
    }
    phase_start = app_timer_cnt_get();
    epd->drv->refresh(epd);
    NRF_LOG_INFO("[EPD]: panel refresh phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:refresh=%ums", epd_elapsed_ms(phase_start));
    app_feed_wdt();

    if (UC8179_HadBusyTimeout() || EPD_BusyAbortRequested()) {
        NRF_LOG_ERROR("[EPD]: BUSY timeout during full refresh\n");
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        return false;
    }

    EPD_LED_OFF();
    p_epd->last_full_clear = data->timestamp;
    p_epd->force_full_refresh = false;

    if (epd_battery_allows_clock_partial(data->voltage_mv) &&
        epd_is_clock_mode(data->mode)) {
        epd_clock_refresh_mark_valid(data->timestamp);
    }

    return true;
}

/* 把 GAP 设备名填进 GUI ssid。 */
static void epd_fill_device_name(gui_data_t *data) {
    uint16_t dev_name_len = sizeof(data->ssid);
    uint32_t err_code = sd_ble_gap_device_name_get((uint8_t *)data->ssid, &dev_name_len);
    if (err_code == NRF_SUCCESS && dev_name_len > 0) {
        data->ssid[dev_name_len] = '\0';
    }
}

/**@brief GUI更新函数(由app_scheduler调度)
 *
 * @details 此函数处理完整的GUI更新过程,包括:
 * 1. 全刷/局刷决策
 * 2. 电池电压在整屏刷新前更新
 * 3. GUI渲染
 * 4. 显示刷新
 *
 * @param[in] p_event_data  ble_epd_t 指针的调度器副本
 * @param[in] event_size    事件数据大小
 */
static void epd_gui_update(void* p_event_data, uint16_t event_size) {
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    uint32_t requested_timestamp;
    uint16_t refresh_voltage_mv = 3300;  /* 由电量保护更新，结束刷新时可读 */
    bool critical_battery = false;
    bool low_battery = false;
    gui_data_t data;
    display_mode_t mode;

    (void)event_size;
    if (p_epd == NULL ||
        !epd_display_queue_begin(&p_epd->display_queue, &requested_timestamp)) return;
    mode = (display_mode_t)p_epd->config.display_mode;

    /* A refresh event may already be in app_scheduler when the first font
     * packet arrives.  Checking only in ble_epd_on_timer() is too late: that
     * queued event would still enter DrawGUI and contend with the external
     * Flash transaction.  Drop the stale event and let the normal timer (or
     * the delayed font refresh) schedule a new one later. */
    if (resource_protocol_entered() || p_epd->init_pending ||
        p_epd->init_ack_pending || p_epd->disconnect_pending ||
#if FONT_STORE_MUTABLE
        p_epd->font.operation_pending || font_store_update_active()
#else
        false
#endif
        ) {
        epd_display_complete(p_epd);
        return;
    }
#if FONT_STORE_MUTABLE
    if (p_epd->font.refresh_pending &&
        (int32_t)(app_uptime_seconds() - p_epd->font.refresh_due) < 0) {
        epd_display_complete(p_epd);
        return;
    }
#endif

    if (epd_clock_refresh_must_yield(mode, p_epd->transfer_ctx.transfer_active)) {
        epd_display_complete(p_epd);
        return;
    }

    // 设置刷新标志，阻止中断中的 WRITE_BLOCK 重入面板操作。
    p_epd->is_refreshing = true;
    p_epd->last_refresh_start = requested_timestamp;

    app_feed_wdt();  // 开始时喂狗
    EPD_GPIO_Init();
    EPD_ClearBusyAbort();
    UC8179_ClearBusyTimeout();  // 每个刷新周期前清除忙超时标志
    const epd_model_t * epd = p_epd->epd;
    if (epd == NULL) {
        epd = epd_get_model((epd_model_id_t)p_epd->config.model_id);
        p_epd->epd = epd;
        if (!epd_is_clock_mode(mode)) {
            p_epd->force_full_refresh = true;
        }
    }
    EPD_LED_OFF();  // 刷新过程保持 LED 熄灭

    bool use_partial = false;

    // ── Battery guard ────────────────────────────────────────────────────────
    {
        battery_guard_result_t bat = epd_battery_guard(p_epd);
        refresh_voltage_mv = bat.voltage_mv;
        critical_battery   = bat.critical;
        if (bat.skip) goto finish_refresh;
    }
    low_battery = epd_battery_is_low(refresh_voltage_mv);

    // ── 准备 GUI 数据 ────────────────────────────────────────────────────────
    data = (gui_data_t){
        .mode        = mode,
        .color       = epd->color,
        .width       = epd->width,
        .height      = epd->height,
        .timestamp   = requested_timestamp,
        .week_start  = p_epd->config.week_start,
        .temperature = 0,
        .voltage_mv  = refresh_voltage_mv,
    };

    /*
     * UC8179 硬件 OLD/NEW 局刷仅用于时钟区。
     * 低电量及其他显示模式走单一全刷路径。
     */
    {
        tm_t tm_now = {0};
        epd_refresh_kind_in_t kind_in;
        epd_refresh_kind_t kind;

        transformTime(requested_timestamp, &tm_now);
        kind_in = (epd_refresh_kind_in_t){
            .mode = mode,
            .timestamp = requested_timestamp,
            .force_full_refresh = p_epd->force_full_refresh,
            .last_full_clear = p_epd->last_full_clear,
            .clock_state_valid = epd_clock_refresh_is_valid(),
            .full_refresh_minutes = p_epd->config.full_refresh_minutes,
            .hardware_partial_ok =
                (epd->drv->ic == EPD_DRIVER_IC_UC8179 &&
                 epd->drv->init_partial != NULL &&
                 epd->drv->diff_push != NULL &&
                 epd->drv->refresh_partial != NULL &&
                 epd_battery_allows_clock_partial(refresh_voltage_mv)),
            .hour = tm_now.tm_hour,
        };
        kind = epd_refresh_kind(&kind_in);
        use_partial = (kind == EPD_REFRESH_KIND_PARTIAL);
    }

    if (use_partial) {
        NRF_LOG_INFO("[EPD]: Hardware OLD/NEW clock partial refresh\n");
        if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                         p_epd->transfer_ctx.transfer_active)) {
            goto finish_refresh;
        }
        UC8179_ClearBusyTimeout();
        epd->drv->init_partial(epd);
        app_feed_wdt();

        if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                         p_epd->transfer_ctx.transfer_active) ||
            EPD_BusyAbortRequested()) {
            goto finish_refresh;
        }
        if (UC8179_HadBusyTimeout()) {
            NRF_LOG_ERROR("[EPD]: Partial configure failed; using full refresh\n");
        } else if (epd_clock_refresh_try(epd, &data) &&
                   !UC8179_HadBusyTimeout()) {
            NRF_LOG_INFO("[EPD]: Hardware OLD/NEW clock partial refresh complete\n");
            ble_log("[EPD]:part ok");
            goto finish_refresh;
        } else {
            NRF_LOG_WARNING(
                "[EPD]: Differential refresh unavailable; using full refresh\n");
        }
        if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                         p_epd->transfer_ctx.transfer_active) ||
            EPD_BusyAbortRequested()) {
            goto finish_refresh;
        }
        epd_clock_refresh_invalidate();
    } else if (epd_is_clock_mode(mode) && !low_battery) {
        NRF_LOG_INFO("[EPD]: Performing clock maintenance full refresh\n");
        ble_log("[EPD]:full ts=%u %s", requested_timestamp,
                p_epd->force_full_refresh ? "force" :
                (p_epd->last_full_clear == 0U ? "first" : "period"));
    }

    NRF_LOG_INFO("[EPD]: Using full refresh\n");
    ble_log("[EPD]:full");
    epd_fill_device_name(&data);

    if (!epd_perform_full_refresh(p_epd, epd, &data)) {
        NRF_LOG_ERROR("[EPD]: Full refresh failed\n");
        goto finish_refresh;
    }

    ble_log("[EPD]:full ok");

finish_refresh:
    app_feed_wdt();

    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active) &&
        p_epd->gpio_connection_ref) {
        goto release_gpio;
    }

    // ========================================================================
    // 刷新后处理
    // ========================================================================
    // 如果刷新期间BLE断开,现在将EPD置于睡眠状态
    // (on_disconnect跳过了此操作以避免中断刷新)
    bool ble_disconnected = (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID);
    if (ble_disconnected) {
        NRF_LOG_INFO("[EPD]: Refresh complete, BLE disconnected - putting EPD to sleep\n");
    }

    if (critical_battery) {
        NRF_LOG_ERROR("[EPD]: Critical battery (%d mV), shutting down\n", refresh_voltage_mv);
    }

    bool clock_mode = epd_is_clock_mode(mode);
    bool power_down_display = clock_mode || ble_disconnected || critical_battery ||
                              !p_epd->gpio_connection_ref;
    if (power_down_display && epd->drv->sleep) {
        NRF_LOG_INFO("[EPD]: Powering down display until next refresh\n");
        epd->drv->sleep(epd);
        nrf_delay_ms(200);  // 等待睡眠生效
        p_epd->epd = NULL;
        /* 保留上次时间戳，下一分钟可用 init_partial 仍按 OLD/NEW 提交前后帧。 */
        if (!clock_mode || critical_battery) {
            p_epd->force_full_refresh = true;
            epd_clock_refresh_invalidate();
        }
    }

    if (critical_battery) {
        sleep_mode_enter();
    }

release_gpio:
    EPD_GPIO_Uninit();  // 所有刷新退出路径释放本次持有的引用

    // 刷新完成后清除刷新标志
    p_epd->is_refreshing = false;

    app_feed_wdt();  // 最后喂狗
    epd_display_complete(p_epd);
}

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
    p_epd->is_notification_enabled = false;
    p_epd->connection_generation++;
    p_epd->config_read_pending = false;
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

#if FONT_STORE_MUTABLE
    /* Do not reset FIFO/command state from the BLE event context.  A font
     * worker may already be running from app_scheduler; let that worker
     * serialize the cancellation and discard stale work in one place. */
    p_epd->font.abort_requested = true;
    p_epd->font.task_pending |= FONT_TASK_TIMEOUT_ABORT;
    p_epd->font.task_wait_tx &= (uint8_t)~FONT_TASK_TIMEOUT_ABORT;
    if (!p_epd->font.task_scheduled) {
        (void)font_task_schedule(p_epd, FONT_TASK_TIMEOUT_ABORT);
    }
    NRF_LOG_WARNING("[FONT]: upload abort deferred after BLE disconnect\n");
#endif

    /* GPIO, panel sleep and image-session cancellation run in main. */

}

/**@brief 更新显示模式(内部函数)
 *
 * @details 如果模式发生变化:
 * 1. 更新配置中的显示模式
 * 2. 写入配置到Flash
 * 3. 设置强制全刷标志,防止模式切换时残影
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] mode   新的显示模式
 */
static void epd_update_display_mode(ble_epd_t* p_epd, display_mode_t mode) {
    if (p_epd->config.display_mode != mode) {
        p_epd->config.display_mode = mode;
        epd_config_write(&p_epd->config);

        // 模式变化时,下次GUI更新时强制全刷
        // 这防止了模式切换时的残影
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
    }
}

/**@brief 离开时钟模式前切回全刷 Init，避免局刷状态干扰写图 */
static void epd_switch_from_clock_mode(ble_epd_t* p_epd) {
    if (p_epd->epd == NULL) return;  // EPD 尚未初始化

    if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        // 重新初始化显示屏以确保处于正常全刷模式
        // 这防止局刷状态干扰直接写入操作
        if (p_epd->epd->drv->init) {
            p_epd->epd->drv->init(p_epd->epd);
            nrf_delay_ms(10);  // 给显示屏重新初始化的时间
        }
    }
}

/* 退出时钟局刷并切到图片模式。 */
static void epd_enter_picture_mode(ble_epd_t *p_epd)
{
    epd_switch_from_clock_mode(p_epd);
    epd_update_display_mode(p_epd, MODE_PICTURE);
}

/* 拼 t=/v=/mtu= 通知串。 */
static size_t epd_append_char(char* out, size_t out_size, size_t pos, char value) {
    if (out_size > 0 && pos < out_size - 1) {
        out[pos] = value;
    }
    return pos + 1;
}

/* 追加无前导零的十进制，超长时只计长度、不再写入。 */
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

/* 生成 "t=123" / "v=3300" 这类前缀+数字串。 */
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

static void epd_send_prefixed_uint(ble_epd_t *p_epd, const char *prefix, uint32_t value)
{
    /* Longest prefix (mtu=), uint32 decimal and terminator fit in 15 bytes. */
    char buf[16];
    size_t len = epd_make_prefixed_uint(buf, sizeof(buf), prefix, value);
    ble_epd_string_send(p_epd, (uint8_t *)buf, len);
}

/**@brief 发送当前时间到BLE对端
 *
 * @param[in] p_epd  EPD服务结构指针
 */

static void epd_send_time(ble_epd_t* p_epd) {
    epd_send_prefixed_uint(p_epd, "t=", timestamp());
}

/**@brief 发送当前电池电压到BLE对端 */
static void epd_send_voltage(ble_epd_t* p_epd) {
    epd_send_prefixed_uint(p_epd, "v=", EPD_ReadVoltageAndCache());
}

/**@brief 发送MTU大小到BLE对端
 *
 * @param[in] p_epd  EPD服务结构指针
 */
static void epd_send_mtu(ble_epd_t* p_epd) {
    epd_send_prefixed_uint(p_epd, "mtu=", p_epd->max_data_len);
}

/**@brief 解析SET_TIME/SYNC_TIME_SILENT中的时间戳
 *
 * @details 载荷格式:
 * - [1..4]: Unix时间戳(大端序, UTC秒)
 * - [5]:    时区偏移(小时, int8_t, 可选, 默认+8)
 *
 * @param[in] p_data  命令数据
 * @param[in] length  数据长度
 *
 * @return 本地时区修正后的时间戳
 */
static uint32_t epd_parse_time_payload(const uint8_t* p_data, uint16_t length) {
    uint32_t parsed_timestamp = ((uint32_t)p_data[1] << 24) |
                                ((uint32_t)p_data[2] << 16) |
                                ((uint32_t)p_data[3] << 8) |
                                (uint32_t)p_data[4];
    int8_t timezone_hours = (length > 5) ? (int8_t)p_data[5] : 8;
    return parsed_timestamp + (timezone_hours * 60 * 60);
}

static bool epd_config_pins_valid(const epd_config_t *cfg)
{
    const uint8_t pins[] = {cfg->mosi_pin, cfg->sclk_pin, cfg->cs_pin,
        cfg->dc_pin, cfg->rst_pin, cfg->busy_pin, cfg->bs_pin,
        cfg->en_pin, cfg->led_pin, cfg->wakeup_pin};
    for (unsigned i = 0; i < sizeof(pins); i++) {
        if (i >= 6U && pins[i] == 0xFFU) continue;
        if (!nrf_gpio_pin_present_check(pins[i])) return false;
    }
    return true;
}

static void cmd_set_pins(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 改引脚映射并重开 GPIO */
{
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
    if (!epd_config_pins_valid(&candidate)) return;
    p_epd->config = candidate;
    epd_config_write(&p_epd->config);
    EPD_GPIO_Uninit();
    EPD_GPIO_Load(&p_epd->config);
    EPD_GPIO_Init();
    app_feed_wdt();
}

static void cmd_init(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 初始化驱动，回 MTU/时间/电压 */
{
    if (p_epd->init_pending || p_epd->init_ack_pending) return;
    p_epd->init_model = length > 1U ? p_data[1] : p_epd->config.model_id;
    p_epd->init_token = length > 2U ? p_data[2] : 0U;
    p_epd->init_prior_commands = p_epd->cmd_queue.count;
    p_epd->init_pending = true;
}

/* The sole INIT worker runs in main, after a refresh returns. */
static void epd_init_process(ble_epd_t *p_epd)
{
    if (p_epd->disconnect_pending || !p_epd->gpio_connection_ref ||
        p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) return;
    if (p_epd->init_pending && p_epd->init_prior_commands == 0U &&
        !p_epd->init_ack_pending && !p_epd->is_refreshing &&
#if FONT_STORE_MUTABLE
        !p_epd->font.operation_pending && !font_store_update_active()
#else
        true
#endif
        ) {
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
        UC8179_ClearBusyTimeout();
        p_epd->epd = epd_init((epd_model_id_t)model);
        p_epd->init_status = UC8179_HadBusyTimeout() ? 1U : 0U;
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
        refresh = p_epd->transfer_ctx.transfer_active;
        memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
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

static void cmd_clear(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 清屏并进图片模式 */
{
    if (!epd_ensure_display(p_epd)) return;
    if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        NRF_LOG_INFO("Switch mode: clear\n");
    }
    epd_enter_picture_mode(p_epd);
    p_epd->epd->drv->clear(p_epd->epd, length > 1 ? p_data[1] : true);
    app_feed_wdt();
}

static void cmd_send_command(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 透传控制器命令 */
{
    if (length < 2) return;
    if (!epd_ensure_display(p_epd)) return;
    if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        NRF_LOG_INFO("Switch mode: cmd\n");
        epd_enter_picture_mode(p_epd);
    }
    EPD_WriteCmd(p_data[1]);
    app_feed_wdt();
}

static void cmd_send_data(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 透传控制器数据 */
{
    if (!epd_ensure_display(p_epd)) return;
    if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        NRF_LOG_INFO("Switch mode: data\n");
        epd_enter_picture_mode(p_epd);
    }
    EPD_WriteData(&p_data[1], length - 1);
    app_feed_wdt();
}

static void cmd_refresh(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 刷新显存到屏幕 */
{
    (void)p_data; (void)length;
    if (!epd_ensure_display(p_epd)) return;
    EPD_ClearBusyAbort();
    epd_enter_picture_mode(p_epd);
    p_epd->epd->drv->refresh(p_epd->epd);
    p_epd->transfer_ctx.transfer_active = false;
    app_feed_wdt();
}

static void cmd_sleep(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 面板睡眠 */
{
    (void)p_data; (void)length;
    if (p_epd->epd == NULL) return;
    p_epd->epd->drv->sleep(p_epd->epd);
    p_epd->epd = NULL;
    p_epd->force_full_refresh = true;
    epd_clock_refresh_invalidate();
    app_feed_wdt();
}

static void cmd_set_time(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 校时（可带模式），受频率限制 */
{
    epd_set_time_action_t action;
    bool rate_limited;
    bool mode_changed = false;
    uint32_t now;

    if (length < 5) return;

    now = app_uptime_seconds();
    rate_limited = (p_epd->last_set_time_ts != 0 &&
                    (now - p_epd->last_set_time_ts) < EPD_SET_TIME_RATE_LIMIT_S);
    action = epd_set_time_action(rate_limited, length > 6, (length > 6) ? p_data[6] : 0);

    if (rate_limited) {
        NRF_LOG_WARNING("[EPD]: SET_TIME rate-limited (last %u s ago)",
                        now - p_epd->last_set_time_ts);
    }

    if (action.apply_mode) {
        display_mode_t prev_mode = (display_mode_t)p_epd->config.display_mode;
        epd_update_display_mode(p_epd, (display_mode_t)action.mode);
        if (prev_mode != (display_mode_t)action.mode) {
            mode_changed = true;
        }
    }

    if (action.apply_time) {
        uint32_t ts = epd_parse_time_payload(p_data, length);
        set_timestamp(ts);
        p_epd->last_set_time_ts = now;
        ble_epd_on_timer(p_epd, ts, true);
    } else if (mode_changed) {
        ble_epd_on_timer(p_epd, timestamp(), true);
    }
    app_feed_wdt();
}

static void cmd_sync_time_silent(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 只改时间戳，不刷新 */
{
    if (length < 5) return;
    uint32_t ts = epd_parse_time_payload(p_data, length);
    set_timestamp(ts);
    NRF_LOG_INFO("[EPD]: Time synced silently\n");
    epd_send_time(p_epd);
    epd_send_mtu(p_epd);
    app_feed_wdt();
}

static void cmd_set_week_start(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 0=周日，立即写配置 */
{
    if (length < 2) return;
    if (p_data[1] < 7 && p_data[1] != p_epd->config.week_start) {
        p_epd->config.week_start = p_data[1];
        epd_config_write(&p_epd->config);
    }
    app_feed_wdt();
}

static ble_epd_t *s_anno_epd;

static void epd_refresh_annotations(ble_epd_t *p_epd)
{
    if (p_epd == NULL || p_epd->config.display_mode == MODE_PICTURE) {
        return;
    }
    p_epd->force_full_refresh = true;
    ble_epd_on_timer(p_epd, timestamp(), true);
}

#if FONT_STORE_MUTABLE
void epd_refresh_fonts(ble_epd_t *p_epd)
{
    if (p_epd == NULL || p_epd->config.display_mode == MODE_PICTURE) {
        return;
    }
    p_epd->font.refresh_pending = true;
    p_epd->font.refresh_due = app_uptime_seconds() + EPD_FONT_REFRESH_DELAY_S;
}
#endif

static void annotations_store_applied(void)
{
    epd_refresh_annotations(s_anno_epd);
}

static calendar_annotations_tx_result_t annotations_tx_send(void *context,
                                                            uint8_t *data,
                                                            uint16_t length)
{
    uint32_t err = ble_epd_notification_try_send((ble_epd_t *)context, data, length);
    if (err == NRF_SUCCESS) return CALENDAR_ANNO_TX_SENT;
    if (err == NRF_ERROR_RESOURCES) return CALENDAR_ANNO_TX_BLOCKED;
    return CALENDAR_ANNO_TX_FAILED;
}

static void cmd_set_annotations(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length)
{
    calendar_anno_rx_t rx;
    uint16_t offset;
    uint16_t total;

    if (length < 2) return;
    if (p_epd->annotations_rx_abort_requested) return;
    if (p_data[1] == CALENDAR_ANNO_OP_GET) {
        calendar_annotations_tx_start(&p_epd->annotations_tx);
        app_feed_wdt();
        return;
    }
    if (p_epd->annotations_tx.active) {
        NRF_LOG_WARNING("anno: mutation rejected during readback\n");
        return;
    }
    if (p_data[1] == CALENDAR_ANNO_OP_CLEAR) {
        if (calendar_annotations_store_clear()) {
            epd_refresh_annotations(p_epd);
        }
        app_feed_wdt();
        return;
    }
    if (p_data[1] != CALENDAR_ANNO_OP_WRITE || length < 6) return;
    offset = (uint16_t)p_data[2] | ((uint16_t)p_data[3] << 8);
    total = (uint16_t)p_data[4] | ((uint16_t)p_data[5] << 8);
    if (offset == 0U && calendar_annotations_store_busy()) {
        NRF_LOG_WARNING("anno: drop write, external store busy\n");
        return;
    }
    rx = calendar_annotations_rx_write(offset, total, &p_data[6], (uint16_t)(length - 6U));
    if (rx == CALENDAR_ANNO_RX_MORE) {
        p_epd->annotations_rx_last_activity = app_uptime_seconds();
    }
    if (rx == CALENDAR_ANNO_RX_DONE) {
        p_epd->annotations_rx_last_activity = 0U;
        calendar_annotations_store_save();
        epd_refresh_annotations(p_epd);
    } else if (rx == CALENDAR_ANNO_RX_RECOVERED) {
        p_epd->annotations_rx_last_activity = 0U;
    }
    app_feed_wdt();
}


static void cmd_set_mode(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 切显示模式并立刻刷新 */
{
    if (length < 2) return;
    if (epd_display_mode_valid(p_data[1])) {
        display_mode_t new_mode = (display_mode_t)p_data[1];
        NRF_LOG_INFO("[EPD]: CMD_SET_MODE: switching to mode %d\n", new_mode);
        if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
            if (!epd_is_clock_mode(new_mode)) {
                epd_switch_from_clock_mode(p_epd);
            }
        }
        epd_update_display_mode(p_epd, new_mode);
        ble_epd_on_timer(p_epd, timestamp(), true);
    }
    app_feed_wdt();
}

static void cmd_write_image(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 旧版无 CRC 写 RAM */
{
    if (length < 3) return;
    if (epd_picture_write_blocked(p_epd->is_refreshing)) return;
    if (!epd_ensure_display(p_epd)) return;
    if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        NRF_LOG_INFO("Switch mode: img\n");
    }
    EPD_ClearBusyAbort();
    epd_enter_picture_mode(p_epd);
    p_epd->epd->drv->write_ram(p_epd->epd, p_data[1], &p_data[2], length - 2);
    app_feed_wdt();
}

static void cmd_write_block(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* CRC 分块顺序写入 */
{
    uint16_t block_id;

    if (length < 8) return;
    if (epd_picture_write_blocked(p_epd->is_refreshing) ||
        p_epd->disconnect_pending || !p_epd->gpio_connection_ref || p_epd->cmd_queue.is_processing ||
        p_epd->init_pending || p_epd->init_running || p_epd->init_ack_pending) {
        block_id = p_data[1] | (p_data[2] << 8);
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
        app_feed_wdt();
        return;
    }
    if (!epd_ensure_display(p_epd)) return;
    block_id = p_data[1] | (p_data[2] << 8);
    uint16_t total_blocks = p_data[3] | (p_data[4] << 8);
    uint8_t  cfg         = p_data[5];
    uint16_t payload_len = length - 8;
    uint8_t *payload     = &p_data[6];
    uint16_t recv_crc    = p_data[length - 2] | (p_data[length - 1] << 8);
    uint16_t calc_crc    = crc16_compute(payload, payload_len);

    if (calc_crc == recv_crc) {
        p_epd->image_last_activity = app_uptime_seconds();
        if (block_id >= EPD_MAX_BLOCKS || block_id >= total_blocks) {
            NRF_LOG_WARNING("[EPD]: Block %d invalid (max=%d, total=%d)\n",
                            block_id, EPD_MAX_BLOCKS, total_blocks);
            send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_INVALID);
            app_feed_wdt();
            return;
        }
        if (!epd_block_total_valid(total_blocks)) {
            NRF_LOG_WARNING("[EPD]: Invalid total_blocks=%d\n", total_blocks);
            send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_INVALID);
            app_feed_wdt();
            return;
        }
        {
            epd_block_accept_t accept = epd_block_accept(
                block_id, p_epd->transfer_ctx.received_blocks,
                p_epd->transfer_ctx.transfer_active);

            if (accept == EPD_BLOCK_REJECT) {
                uint8_t session = p_epd->transfer_ctx.session_id;
                NRF_LOG_WARNING("[EPD]: Block %d gap (have %d), restart from 0\n",
                                block_id, p_epd->transfer_ctx.received_blocks);
                memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
                p_epd->transfer_ctx.session_id = session;
                p_epd->transfer_ctx.transfer_active = true;
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
                app_feed_wdt();
                return;
            }

            if (block_id == 0 || !p_epd->transfer_ctx.transfer_active) {
                p_epd->transfer_ctx.total_blocks    = total_blocks;
                p_epd->transfer_ctx.received_blocks = 0;
                p_epd->transfer_ctx.transfer_active = true;
                if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
                    NRF_LOG_INFO("Switch mode: block\n");
                }
                EPD_ClearBusyAbort();
                epd_enter_picture_mode(p_epd);
            } else if (p_epd->transfer_ctx.total_blocks != total_blocks) {
                NRF_LOG_WARNING("[EPD]: total_blocks changed mid-transfer: %d -> %d, resetting\n",
                                p_epd->transfer_ctx.total_blocks, total_blocks);
                memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
                app_feed_wdt();
                return;
            }

            if (accept == EPD_BLOCK_DUP) {
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_OK);
                app_feed_wdt();
                return;
            }

            p_epd->epd->drv->write_ram(p_epd->epd, cfg, payload, payload_len);
            p_epd->transfer_ctx.received_blocks++;
            if (block_id == 0 || block_id + 1U == total_blocks) {
                NRF_LOG_INFO("[LEGACY] block=%u/%u cfg=0x%02x CRC OK",
                             block_id, total_blocks, cfg);
            }
        }
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_OK);
    } else {
        NRF_LOG_WARNING("[EPD]: Block %d CRC mismatch! recv=0x%04X, calc=0x%04X\n",
                        block_id, recv_crc, calc_crc);
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_CRC);
    }
    app_feed_wdt();
}

static void cmd_query_status(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 回传传输进度 */
{
    (void)p_data; (void)length;
    send_transfer_status(p_epd);
}

static void cmd_reset_transfer(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 清传输状态并打断时钟忙等 */
{
    if (p_epd->disconnect_pending) return;
    p_epd->image_last_activity = app_uptime_seconds();
    memset(&p_epd->transfer_ctx, 0, sizeof(image_transfer_ctx_t));
    if (length > 1) {
        p_epd->transfer_ctx.session_id = p_data[1];
    }
    /* 卡住时钟节拍但不改 display_mode。首块 WRITE_BLOCK 仍视为时钟模式，
     * 走全刷 Init，从而退出局刷窗口。 */
    p_epd->transfer_ctx.transfer_active = true;
    EPD_RequestBusyAbort();
    epd_clock_refresh_invalidate();
    NRF_LOG_INFO("[EPD]: Transfer state reset, session_id=%d\n", p_epd->transfer_ctx.session_id);
}

static void cmd_set_config(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 整包配置写入并全刷 */
{
    if (length < 2) return;
    epd_config_t candidate = p_epd->config;
    memcpy(&candidate, &p_data[1], (length - 1 > EPD_CONFIG_SIZE) ? EPD_CONFIG_SIZE : length - 1);
    if (!epd_config_pins_valid(&candidate)) return;
    p_epd->config = candidate;
    epd_config_write(&p_epd->config);

    // 配置更新后强制全刷并立刻刷新，使模式、周起始日等马上可见。
    p_epd->force_full_refresh = true;
    ble_epd_on_timer(p_epd, timestamp(), true);

    app_feed_wdt();
}

static void cmd_sys_sleep(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* MCU 睡眠 */
{
    (void)p_epd; (void)p_data; (void)length;
    sleep_mode_enter();
    app_feed_wdt();
}

static void cmd_sys_reset(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* MCU 复位 */
{
    (void)p_epd; (void)p_data; (void)length;
    nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_RESET);
}

static void cmd_cfg_erase(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 擦配置并复位 */
{
    (void)p_data; (void)length;
    epd_config_clear(&p_epd->config);
    /* Reset is deferred until the external tombstone commit is confirmed. */
}

/**@brief 执行单个命令(由队列处理器调用)
 *
 * @details 此函数是所有EPD命令的核心处理函数。
 * 它根据命令ID执行相应的操作,包括:
 * - 引脚配置 (EPD_CMD_SET_PINS)
 * - 初始化 (EPD_CMD_INIT)
 * - 清屏 (EPD_CMD_CLEAR)
 * - 发送命令/数据 (EPD_CMD_SEND_COMMAND/DATA)
 * - 刷新 (EPD_CMD_REFRESH)
 * - 睡眠 (EPD_CMD_SLEEP)
 * - 设置时间 (EPD_CMD_SET_TIME)
 * - 设置模式 (EPD_CMD_SET_MODE)
 * - 写入图像 (EPD_CMD_WRITE_IMAGE)
 * - 系统控制 (EPD_CMD_SYS_RESET/SLEEP/CFG_ERASE)
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] p_data  命令数据指针
 * @param[in] length  命令数据长度
 */
static void epd_execute_command(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    if (p_data == NULL || length == 0) return;

#if FONT_STORE_MUTABLE
    /* A queued display command may have been accepted just before the font
     * transaction set its guard.  Drop it here as a second line of defense;
     * otherwise it can start a panel refresh while the external Flash is
     * being written. */
    if (p_epd->font.operation_pending || font_store_update_active()) {
        if (p_data[0] != EPD_CMD_FONT_STORE &&
            font_blocks_display_command(p_data[0])) {
            NRF_LOG_WARNING("[EPD]: drop display command 0x%02x during font update\n",
                            p_data[0]);
            epd_report_font_busy(p_epd, p_data, length);
            return;
        }
    }
#endif

    if (!is_bulk_data_command(p_data[0])) {
        NRF_LOG_DEBUG("[EPD]: Executing command 0x%02x, LEN=%d\n", p_data[0], length);
    }
    if (!is_bulk_data_command(p_data[0])) {
        NRF_LOG_HEXDUMP_DEBUG(p_data, length);
    }

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
        case EPD_CMD_SET_ANNOTATIONS: cmd_set_annotations(p_epd, p_data, length); break;
#if FONT_STORE_MUTABLE
        case EPD_CMD_FONT_STORE:      cmd_font_store(p_epd, p_data, length);      break;
#endif
        case EPD_CMD_SET_MODE:        cmd_set_mode(p_epd, p_data, length);        break;
        case EPD_CMD_WRITE_IMAGE:     cmd_write_image(p_epd, p_data, length);     break;
        case EPD_CMD_WRITE_BLOCK:     cmd_write_block(p_epd, p_data, length);     break;
        case EPD_CMD_QUERY_STATUS:    cmd_query_status(p_epd, p_data, length);    break;
        case EPD_CMD_RESET_TRANSFER:  cmd_reset_transfer(p_epd, p_data, length);  break;
        case EPD_CMD_SET_CONFIG:      cmd_set_config(p_epd, p_data, length);      break;
        case EPD_CMD_SYS_SLEEP:       cmd_sys_sleep(p_epd, p_data, length);       break;
        case EPD_CMD_SYS_RESET:       cmd_sys_reset(p_epd, p_data, length);       break;
        case EPD_CMD_CFG_ERASE:       cmd_cfg_erase(p_epd, p_data, length);       break;
        default: break;
    }

    // 命令执行后喂狗
    app_feed_wdt();
    if (!is_bulk_data_command(p_data[0])) {
        NRF_LOG_DEBUG("[EPD]: Command 0x%02x execution completed\n", p_data[0]);
    }
}

/**@brief 处理队列中的下一个命令(由app_scheduler调度)
 *
 * @details 此函数是命令队列处理的核心:
 * 1. 从队列中弹出一个命令
 * 2. 执行该命令
 * 3. 如果队列还有命令,调度下一次处理
 * 4. 队列为空时标记处理完成
 *
 *
 * @param[in] p_event_data  EPD服务结构指针的指针(复制的数据)
 * @param[in] event_size  事件数据大小
 */
static void epd_process_next_command(void* p_event_data, uint16_t event_size) {
    (void)event_size;
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    epd_cmd_queue_item_t cmd_item;
    bool available = false;
    CRITICAL_REGION_ENTER();
    p_epd->cmd_queue.is_scheduled = false;
    p_epd->cmd_queue.is_processing = (!p_epd->init_pending || p_epd->init_prior_commands != 0U) &&
                                    !p_epd->disconnect_pending &&
                                    p_epd->gpio_connection_ref && p_epd->conn_handle != BLE_CONN_HANDLE_INVALID;
    if (p_epd->cmd_queue.is_processing) {
        available = epd_cmd_queue_pop(&p_epd->cmd_queue, &cmd_item);
        if (available && p_epd->init_pending && p_epd->init_prior_commands != 0U)
            p_epd->init_prior_commands--;
    }
    CRITICAL_REGION_EXIT();
    if (!p_epd->cmd_queue.is_processing) return;
    if (available) {
        epd_execute_command(p_epd, cmd_item.data, cmd_item.length);
        app_feed_wdt();
    }
    CRITICAL_REGION_ENTER();
    p_epd->cmd_queue.is_processing = false;
    CRITICAL_REGION_EXIT();
    epd_cmd_queue_schedule(&p_epd->cmd_queue, p_epd, epd_process_next_command);
}

/* Called only from the main loop. Persistent writes and failed scheduler
 * submissions must make progress even when no further BLE commands arrive. */
static uint32_t resource_notification_send(void *context, uint32_t generation,
                                           uint8_t *data, uint16_t length)
{
    ble_epd_t *p_epd = context;
    if (p_epd->connection_generation != generation || p_epd->disconnect_pending)
        return NRF_ERROR_INVALID_STATE;
    return ble_epd_notification_try_send(p_epd, data, length);
}

static void epd_record_ack_process(ble_epd_t *p_epd)
{
    uint8_t result;
    uint32_t revision, crc;
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID || p_epd->disconnect_pending ||
        !p_epd->is_notification_enabled) {
        p_epd->record_ack_pending = false;
        (void)epd_config_take_save_result(&result, &revision, &crc);
        (void)calendar_annotations_store_take_save_result(&result, &revision, &crc);
        return;
    }
    if (!p_epd->record_ack_pending) {
        uint8_t partition = 0U;
        if (epd_config_take_save_result(&result, &revision, &crc))
            partition = RS_PART_EPD_CONFIG;
        else if (calendar_annotations_store_take_save_result(&result, &revision, &crc))
            partition = RS_PART_CALENDAR_ANNOTATIONS;
        if (partition != 0U) {
            p_epd->record_ack[0] = EPD_RSP_RECORD_COMMIT;
            p_epd->record_ack[1] = partition;
            p_epd->record_ack[2] = result;
            p_epd->record_ack[3] = 0U;
            resource_put_u32(p_epd->record_ack + 4, revision);
            resource_put_u32(p_epd->record_ack + 8, crc);
            p_epd->record_ack_pending = true;
        }
    }
    if (p_epd->record_ack_pending &&
        ble_epd_notification_try_send(p_epd, p_epd->record_ack,
                                      sizeof(p_epd->record_ack)) == NRF_SUCCESS)
        p_epd->record_ack_pending = false;
}

static void epd_config_readback_process(ble_epd_t *p_epd)
{
    if (!p_epd->config_read_pending) return;
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID || p_epd->disconnect_pending ||
        !p_epd->is_notification_enabled) {
        p_epd->config_read_pending = false;
        return;
    }
    if (p_epd->cmd_queue.count || p_epd->cmd_queue.is_processing ||
        p_epd->init_pending || p_epd->init_running || epd_config_busy()) return;
    uint8_t reply[3U + sizeof(epd_config_t)] = {EPD_RSP_CONFIG, p_epd->config_read_token};
    reply[2] = epd_config_is_saved(&p_epd->config) ? RS_OK : RS_IO;
    memcpy(reply + 3, &p_epd->config, sizeof(p_epd->config));
    if (ble_epd_notification_try_send(p_epd, reply, sizeof(reply)) == NRF_SUCCESS)
        p_epd->config_read_pending = false;
}

void ble_epd_process(ble_epd_t* p_epd) {
    epd_connection_process(p_epd);
    epd_init_process(p_epd);
    resource_transfer_process(p_epd, p_epd->connection_generation,
        p_epd->conn_handle != BLE_CONN_HANDLE_INVALID && !p_epd->disconnect_pending,
        !p_epd->is_refreshing
#if FONT_STORE_MUTABLE
            && !p_epd->font.operation_pending && !font_store_update_active()
#endif
        ,
        app_uptime_seconds(), resource_notification_send);
    if (p_epd->annotations_rx_abort_requested) {
        (void)calendar_annotations_rx_cancel();
        CRITICAL_REGION_ENTER();
        p_epd->annotations_rx_abort_requested = false;
        p_epd->annotations_rx_last_activity = 0U;
        CRITICAL_REGION_EXIT();
    }
    epd_config_process();
    calendar_annotations_store_process();
    epd_record_ack_process(p_epd);
    epd_config_readback_process(p_epd);
    calendar_annotations_tx_process(&p_epd->annotations_tx, p_epd->max_data_len,
                                    annotations_tx_send, p_epd);
    if (epd_config_clear_complete()) NVIC_SystemReset();
    epd_cmd_queue_schedule(&p_epd->cmd_queue, p_epd, epd_process_next_command);
    epd_display_queue_schedule(&p_epd->display_queue, p_epd, epd_gui_update);
#if FONT_STORE_MUTABLE
    font_task_retry(p_epd);
#endif
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
#if RESOURCE_STORE_WRITABLE
    /* Avoid display/SPI commands only while an explicit resource write session
     * is active. Leaving the session immediately restores normal operation. */
    if (resource_protocol_entered() &&
        (p_data[0] == EPD_CMD_FONT_STORE || font_blocks_display_command(p_data[0]))) return;
#endif
    if (!is_bulk_data_command(p_data[0])) {
        NRF_LOG_DEBUG("[EPD]: on_write received command 0x%02x, LEN=%d\n", p_data[0], length);
    }

    if (p_data[0] != EPD_CMD_WRITE_BLOCK) {
        ble_log("[CMD]:%02x/%d", p_data[0], length);
    }

#if FONT_STORE_MUTABLE
    /* GET is read-only and must never enter the two-entry display queue.
     * During a busy notification window, restart the dedicated list worker
     * from slot zero; duplicate A3 records are harmless and this prevents a
     * browser list request from being silently lost behind a refresh. */
    if (p_data[0] == EPD_CMD_FONT_STORE && length >= 2U &&
        p_data[1] == FONT_STORE_OP_GET) {
        font_store_send_list(p_epd);
        return;
    }

    /* Stop timer-driven display work as soon as a mutating font command is
     * received, even if the command is still waiting in the app scheduler. */
    bool font_guard = (p_data[0] == EPD_CMD_FONT_STORE && length >= 2U &&
                       p_data[1] != 0x00U);
    if (font_guard && p_epd->font.abort_requested) return;
    if (font_guard) {
        p_epd->font.operation_pending = true;
        p_epd->font.last_activity_ts = app_uptime_seconds();
    }

    /* High-speed font data is copied into a dedicated FIFO immediately.  It
     * must not consume one of the two general command-queue entries. */
    if (p_data[0] == EPD_CMD_FONT_STORE && length >= 2U &&
        p_data[1] == FONT_STORE_OP_STREAM_DATA) {
        cmd_font_store(p_epd, p_data, length);
        return;
    }

    /* GET only schedules three short notifications and does not mutate the
     * store.  Mutating control commands get a private scheduler slot so a
     * pending refresh can never make BEGIN/COMMIT/ABORT disappear from the
     * two-entry display command queue. */
    if (p_data[0] == EPD_CMD_FONT_STORE && length >= 2U &&
        p_data[1] != FONT_STORE_OP_GET) {
        if (p_epd->font.cmd_pending) {
            font_store_send_ack(p_epd, p_data[1], FONT_STORE_BUSY, 0xFFU);
            return;
        }
        if (length > sizeof(p_epd->font.cmd.data)) {
            font_store_send_ack(p_epd, p_data[1], FONT_STORE_INVALID, 0xFFU);
            p_epd->font.operation_pending = false;
            return;
        }
        p_epd->font.cmd.cmd = EPD_CMD_FONT_STORE;
        memcpy(p_epd->font.cmd.data, p_data, length);
        p_epd->font.cmd.length = length;
        p_epd->font.cmd_pending = true;
        (void)font_task_schedule(p_epd, FONT_TASK_CONTROL);
        return;
    }

    if ((p_epd->font.operation_pending || font_store_update_active()) &&
        p_data[0] != EPD_CMD_FONT_STORE &&
        font_blocks_display_command(p_data[0])) {
        NRF_LOG_WARNING("[EPD]: reject display command 0x%02x during font update\n",
                        p_data[0]);
        epd_report_font_busy(p_epd, p_data, length);
        return;
    }
#endif

    // 紧急命令绕过队列立即执行
    if (is_urgent_command(p_data[0]) || epd_cmd_is_immediate(p_data[0])) {
        if (!is_bulk_data_command(p_data[0])) {
            NRF_LOG_INFO("[EPD]: Immediate command 0x%02x, bypassing queue\n", p_data[0]);
        }
        epd_execute_command(p_epd, p_data, length);
        return;
    }

    if (epd_cmd_queue_push(&p_epd->cmd_queue, p_data, length)) {
        epd_cmd_queue_schedule(&p_epd->cmd_queue, p_epd, epd_process_next_command);
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
            static uint16_t length = sizeof(epd_config_t);
            NRF_LOG_DEBUG("send epd config\n");
            uint32_t err_code = ble_epd_string_send(p_epd, (uint8_t*)&p_epd->config, length);
            if (err_code != NRF_ERROR_INVALID_STATE) APP_ERROR_CHECK(err_code);
            /* 未 INIT 也上报 MTU/电压，供网页端计算 WRITE_BLOCK 分块。
             * 默认 HTML MTU=20 会超出 EPD_MAX_BLOCKS。 */
            epd_send_mtu(p_epd);
            epd_send_voltage(p_epd);
        } else {
            p_epd->is_notification_enabled = false;
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
            /* Notification workers use a single non-blocking HVX attempt.
             * Retry only after SoftDevice reports that TX queue space was
             * released, so BLE event handling never sleeps on resources. */
#if FONT_STORE_MUTABLE
            font_task_wake_tx(p_epd, FONT_TASK_LIST |
                                       FONT_TASK_OPERATION_ACK |
                                       FONT_TASK_STREAM_ACK);
#endif
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

    // 初始化服务结构
    p_epd->max_data_len = BLE_EPD_MAX_DATA_LEN;
    memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    p_epd->is_notification_enabled = false;
    p_epd->force_full_refresh = false;
    p_epd->last_full_clear    = 0;  // 尚未全刷
    p_epd->is_refreshing = false;
    p_epd->low_battery_screen_active = false;
    p_epd->gpio_connection_ref = false;
    p_epd->disconnect_pending = false;
    p_epd->connection_generation = 0U;
    p_epd->init_pending = false;
    p_epd->init_running = false;
    p_epd->init_ack_pending = false;
    p_epd->image_last_activity = 0U;
    p_epd->annotations_tx = (calendar_annotations_tx_t){0};
    memset(&p_epd->font, 0, sizeof(p_epd->font));
    epd_display_queue_init(&p_epd->display_queue);
    calendar_annotations_tx_cancel(&p_epd->annotations_tx);
    p_epd->annotations_rx_last_activity = 0U;
    p_epd->annotations_rx_abort_requested = false;
    p_epd->record_ack_pending = false;
    p_epd->config_read_pending = false;
    p_epd->last_refresh_start = 0;
    p_epd->last_set_time_ts = 0;

    // 初始化命令队列
    epd_cmd_queue_init(&p_epd->cmd_queue);

    // 初始化并读取配置
    uint32_t config_err = epd_config_init(&p_epd->config);
    if (config_err != NRF_SUCCESS) return config_err;
    epd_config_read(&p_epd->config);
    s_anno_epd = p_epd;
    calendar_annotations_store_init();
    calendar_annotations_store_on_applied(annotations_store_applied);

    // 空配置或非法引脚映射恢复为板级默认值，避免直接索引无效寄存器。
    if (epd_config_empty(&p_epd->config) || !epd_config_pins_valid(&p_epd->config)) {
        uint8_t cfg[] = EPD_CFG_52811;
        memcpy(&p_epd->config, cfg, sizeof(cfg));
        // 设置默认值
        if (p_epd->config.display_mode == 0xFF) p_epd->config.display_mode = MODE_CLOCK_CALENDAR;
        if (p_epd->config.week_start == 0xFF) p_epd->config.week_start = 0;
        if (p_epd->config.full_refresh_minutes == 0xFF) p_epd->config.full_refresh_minutes = 0;  // 0 = hourly (60 min)
        epd_config_write(&p_epd->config);
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
    return sd_ble_gatts_hvx(p_epd->conn_handle, &hvx_params);
}

uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length) {
    // 检查连接状态和通知是否启用
    if ((p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) || (!p_epd->is_notification_enabled))
        return NRF_ERROR_INVALID_STATE;
    if (length > p_epd->max_data_len) return NRF_ERROR_INVALID_PARAM;

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
    if (calendar_annotations_rx_active() &&
        (uint32_t)(app_uptime_seconds() - p_epd->annotations_rx_last_activity) >=
            EPD_ANNOTATIONS_RX_TIMEOUT_S) {
        p_epd->annotations_rx_abort_requested = true;
    }

#if FONT_STORE_MUTABLE
    /* Retry deferred font tasks from the timer path.  This path only posts a
     * scheduler event; Flash/SPI work remains in font_task_worker(). */
    font_task_retry(p_epd);
    if ((p_epd->font.operation_pending || font_store_update_active()) &&
        p_epd->font.last_activity_ts != 0U &&
        (uint32_t)(app_uptime_seconds() - p_epd->font.last_activity_ts) >=
            FONT_STORE_IDLE_TIMEOUT_S &&
        (p_epd->font.task_pending & FONT_TASK_TIMEOUT_ABORT) == 0U) {
        (void)font_task_schedule(p_epd, FONT_TASK_TIMEOUT_ABORT);
    }

    /* Do not enqueue a refresh while a font transaction is being received or
     * while its post-commit merge window is active.  The old implementation
     * only checked the update flag when the delayed refresh became due, so a
     * periodic refresh could still start in the middle of an upload. */
    if (p_epd->font.operation_pending || font_store_update_active()) {
        return;
    }

    /* Font commits used to start a full refresh immediately from the BLE
     * command handler.  On a 7.5-inch panel that blocks the app scheduler for
     * several seconds, so the following list request or next font upload
     * timed out even though the Flash transaction had already succeeded.
     * Defer and coalesce the refresh; an active upload or queued command wins. */
    if (p_epd->font.refresh_pending &&
        (int32_t)(app_uptime_seconds() - p_epd->font.refresh_due) >= 0 &&
        !p_epd->is_refreshing &&
        !font_store_update_active() &&
        p_epd->cmd_queue.count == 0U &&
        !p_epd->cmd_queue.is_processing) {
        p_epd->font.refresh_pending = false;
        p_epd->force_full_refresh = true;
        force_update = true;
    }

    // 低电量保持：停周期刷新，仍显示月历/时钟。每 LOW_BATTERY_POLL_INTERVAL_S
    // 采样：临界则再刷一帧正常画面后关机，恢复则强制全刷。
#endif
    if (p_epd->low_battery_screen_active && !force_update) {
        if (timestamp % LOW_BATTERY_POLL_INTERVAL_S == 0) {
            uint16_t voltage_mv = EPD_ReadVoltageAndCache();

            if (epd_battery_is_critical(voltage_mv)) {
                epd_display_request(p_epd, timestamp);
                return;
            }

            // 电压恢复到低电量阈值以上时，退出低电量模式，并强制全刷恢复正常屏幕显示
            if (!epd_battery_is_low(voltage_mv)) {
                p_epd->low_battery_screen_active = false;
                p_epd->force_full_refresh = true;
                // 立刻调度 GUI 更新，全刷恢复正常界面
                epd_display_request(p_epd, timestamp);
                return;
            }
        }

        if (epd_battery_should_blink_led(true, false, timestamp)) {
            EPD_LED_BLINK();
        }

        // 低电量模式下不进行周期刷新，避免屏幕刷新占用SPI导致资源写入失败
        if (p_epd->low_battery_screen_active) {
            return;
        }
    }

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
