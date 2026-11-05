/*!
 * @file epd_service_display.c
 * @brief EPD 服务的显示刷新/全刷/时钟局刷/低电量守卫/GUI 数据填充
 *
 * 由 EPD/service/EPD_service.c 按职责拆出；跨模块接口声明见
 * epd_service_internal.h。只搬代码，不改行为/协议/时序。
 */

#include "EPD_service.h"
#include "epd_service_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "app_timer.h"
#include "main.h"
#include "nrf_log.h"
#include "Lunar.h"
#include "epd_battery_policy.h"
#include "epd_clock_refresh.h"
#include "epd_refresh_policy.h"
#include "resource_transfer.h"
#include "resource_protocol.h"
#include "todo_items.h"

/* Failed display attempts use uptime, independent of wall-clock changes.
 * Explicit requests/resource activation and successful frames reset the delay. */
static volatile uint32_t s_display_retry_at;
static volatile uint8_t s_display_retry_step;

void epd_display_retry_reset(void)
{
    s_display_retry_step = 0U;
}

static void epd_display_retry_failed(void)
{
    static const uint8_t delays[] = {1U, 5U, 15U, 30U};
    uint8_t step = s_display_retry_step;
    if (step < sizeof(delays)) step++;
    s_display_retry_at = app_uptime_seconds() + delays[step - 1U];
    s_display_retry_step = step;
}

/* 退避状态查询：静态量留在本模块，ble_epd_on_timer 只通过这两个 helper 判断。 */
bool epd_display_retry_scheduled(void)
{
    return s_display_retry_step != 0U;
}

bool epd_display_retry_waiting(void)
{
    return s_display_retry_step != 0U &&
           (int32_t)(app_uptime_seconds() - s_display_retry_at) < 0;
}

void epd_display_request(ble_epd_t *p_epd, uint32_t requested_timestamp)
{
    epd_display_queue_request(&p_epd->display_queue, requested_timestamp);
    epd_service_schedule_display(p_epd);
}

static void epd_display_complete(ble_epd_t *p_epd)
{
    epd_display_queue_finish(&p_epd->display_queue);
    epd_service_schedule_display(p_epd);
}

#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
static uint32_t epd_elapsed_ms(uint32_t start_ticks)
{
    /* Keep the exact tick ratio: 16 ticks/ms is 16.384 truncated, which makes
     * every logged duration 2.34 % short. 64-bit math is used only in logging
     * builds; elapsed_ticks * 1000 would overflow 32 bits for 24-bit ticks. */
    const uint32_t ticks_per_second =
        APP_TIMER_CLOCK_FREQ / (APP_TIMER_CONFIG_RTC_FREQUENCY + 1U);
    const uint32_t elapsed_ticks = app_timer_cnt_diff_compute(app_timer_cnt_get(), start_ticks);

    return (uint32_t)(((uint64_t)elapsed_ticks * 1000U) / ticks_per_second);
}
#endif

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
    if (cached_mv == 0U) {
        /* Unknown voltage must neither power the panel nor trigger SYSTEMOFF. */
        return (battery_guard_result_t){true, false, false, 0U};
    }

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
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    uint32_t phase_start;
#endif

    if (p_epd == NULL || epd == NULL || epd->drv == NULL || data == NULL ||
        epd->drv->init == NULL || epd->drv->write_image == NULL ||
        epd->drv->refresh == NULL) {
        if (p_epd != NULL) p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        return false;
    }

    epd_clock_refresh_invalidate();
    epd_service_busy_error_clear(p_epd);
    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active)) {
        return false;
    }
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    phase_start = app_timer_cnt_get();
#endif
    epd->drv->init(epd);
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    NRF_LOG_INFO("[EPD]: full init phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:init=%ums", epd_elapsed_ms(phase_start));
#endif
    if (epd_service_busy_error(p_epd) || EPD_BusyAbortRequested()) {
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
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    phase_start = app_timer_cnt_get();
#endif
    DrawGUI(data, (buffer_callback)epd->drv->write_image, (void *)epd);
    if (!GUI_RenderValid()) {
        p_epd->force_full_refresh = true;
        return false;
    }
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    NRF_LOG_INFO("[EPD]: GUI draw phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:draw=%ums", epd_elapsed_ms(phase_start));
#endif
    app_feed_wdt();
    if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                     p_epd->transfer_ctx.transfer_active)) {
        return false;
    }
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    phase_start = app_timer_cnt_get();
#endif
    epd->drv->refresh(epd);
#if NRF_LOG_ENABLED || BLE_LOG_REDIRECT_ALL_NRF_LOG
    NRF_LOG_INFO("[EPD]: panel refresh phase %u ms\n", epd_elapsed_ms(phase_start));
    ble_log("[EPD]:refresh=%ums", epd_elapsed_ms(phase_start));
#endif
    app_feed_wdt();

    if (epd_service_busy_error(p_epd) || EPD_BusyAbortRequested()) {
        NRF_LOG_ERROR("[EPD]: BUSY timeout during full refresh\n");
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        return false;
    }

    EPD_LED_OFF();
    p_epd->last_full_clear = data->timestamp;
    p_epd->force_full_refresh = false;
    epd_display_retry_reset();
    if (epd_is_todo_mode(data->mode)) {
        todo_items_successful_refresh(data->timestamp);
    }

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
void epd_gui_update(void* p_event_data, uint16_t event_size) {
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    uint32_t requested_timestamp;
    uint16_t refresh_voltage_mv = 3300;  /* 由电量保护更新，结束刷新时可读 */
    bool critical_battery = false;
    bool low_battery = false;
    bool refresh_succeeded = false;
    gui_data_t data;
    display_mode_t mode;

    (void)event_size;
    if (p_epd == NULL) return;
    /* Let main retry reliable replies before a slow panel update. Requests
     * remain pending, but this scheduler drain must not requeue itself while
     * HVX is full, otherwise LEAVE/MODE replies can never make progress. */
    if (p_epd->mode_ack_pending || p_epd->initial_notify ||
        resource_transfer_reply_pending() || resource_protocol_entered() ||
        p_epd->init_pending || p_epd->init_ack_pending || p_epd->disconnect_pending) {
        CRITICAL_REGION_ENTER();
        p_epd->display_queue.scheduled = false;
        CRITICAL_REGION_EXIT();
        return;
    }
    if (!epd_display_queue_begin(&p_epd->display_queue, &requested_timestamp)) return;
    mode = (display_mode_t)p_epd->config.display_mode;

    if (epd_clock_refresh_must_yield(mode, p_epd->transfer_ctx.transfer_active)) {
        epd_display_complete(p_epd);
        return;
    }

    /* A queued request can cross a minute, task boundary or midnight. */
    requested_timestamp = timestamp();

    // 设置刷新标志，阻止中断中的 WRITE_BLOCK 重入面板操作。
    p_epd->is_refreshing = true;
    p_epd->last_refresh_start = requested_timestamp;

    app_feed_wdt();  // 开始时喂狗
    EPD_GPIO_Init();
    EPD_ClearBusyAbort();
    epd_service_busy_error_clear(p_epd);  // 每个刷新周期前清除忙超时标志
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
            /* 面板是否支持真 OLD/NEW 局刷由虚表可选能力决定（不绑定 IC）。 */
            .hardware_partial_ok =
                (epd_panel_can_partial(epd) &&
                 epd_battery_allows_clock_partial(refresh_voltage_mv)),
            .hour = tm_now.tm_hour,
        };
        kind = epd_refresh_kind(&kind_in);
        use_partial = (kind == EPD_REFRESH_KIND_PARTIAL);
    }

    if (use_partial) {
        if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                         p_epd->transfer_ctx.transfer_active)) {
            goto finish_refresh;
        }
        epd_service_busy_error_clear(p_epd);
        /* Same eligibility checks as the real partial refresh, run before paying
         * for a panel reset/partial configuration. Conditional clock layouts
         * must fall back to a full refresh without a wasted init_partial(). */
        if (epd_clock_refresh_try(epd, &data, true)) {
            NRF_LOG_INFO("[EPD]: Hardware OLD/NEW clock partial refresh\n");
            epd->drv->init_partial(epd);
            app_feed_wdt();

            if (epd_clock_refresh_must_yield((display_mode_t)p_epd->config.display_mode,
                                             p_epd->transfer_ctx.transfer_active) ||
                EPD_BusyAbortRequested()) {
                goto finish_refresh;
            }
            if (epd_service_busy_error(p_epd)) {
                NRF_LOG_ERROR("[EPD]: Partial configure failed; using full refresh\n");
            } else if (epd_clock_refresh_try(epd, &data, false) &&
                       !epd_service_busy_error(p_epd)) {
                NRF_LOG_INFO("[EPD]: Hardware OLD/NEW clock partial refresh complete\n");
                ble_log("[EPD]:part ok");
                refresh_succeeded = true;
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
        } else {
            NRF_LOG_INFO("[EPD]: Partial refresh ineligible for this layout; "
                         "using full refresh\n");
            ble_log("[EPD]:part skip");
        }
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
    refresh_succeeded = true;

finish_refresh:
    app_feed_wdt();
    if (refresh_succeeded) {
        epd_display_retry_reset();
    } else if (!epd_clock_refresh_must_yield(
                   (display_mode_t)p_epd->config.display_mode,
                   p_epd->transfer_ctx.transfer_active)) {
        p_epd->force_full_refresh = true;
        epd_clock_refresh_invalidate();
        epd_display_retry_failed();
    }

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
    bool power_down_display = clock_mode || epd_is_todo_mode(mode) ||
                              ble_disconnected || critical_battery ||
                              !p_epd->gpio_connection_ref;
    if (power_down_display && epd->drv->sleep) {
        NRF_LOG_INFO("[EPD]: Powering down display until next refresh\n");
        epd->drv->sleep(epd);
        nrf_delay_ms(200);  // 等待睡眠生效
        p_epd->epd = NULL;
        /* 保留上次时间戳，下一分钟可用 init_partial 仍按 OLD/NEW 提交前后帧。 */
        /* Normal sleep preserves the successfully rendered TODO snapshot.
         * Its next requested update already uses full refresh; failures retain
         * their force flag so the RAM-only timer can retry them. */
        if ((!clock_mode && !epd_is_todo_mode(mode)) || critical_battery) {
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

/* Timer callbacks publish work only. SAADC and LED pin changes are serialized
 * with panel/resource operations by the main loop. */
void epd_battery_process(ble_epd_t *p_epd) {
    if (resource_protocol_entered() || p_epd->is_refreshing ||
        p_epd->transfer_ctx.transfer_active) return;
    bool poll, blink;
    CRITICAL_REGION_ENTER();
    poll = p_epd->battery_poll_pending;
    blink = p_epd->battery_blink_pending;
    p_epd->battery_poll_pending = false;
    p_epd->battery_blink_pending = false;
    CRITICAL_REGION_EXIT();
    if (!p_epd->low_battery_screen_active) return;
    if (poll) {
        uint16_t voltage_mv = EPD_ReadVoltageAndCache();
        if (voltage_mv != 0U &&
            (epd_battery_is_critical(voltage_mv) || !epd_battery_is_low(voltage_mv))) {
            p_epd->low_battery_screen_active = epd_battery_is_low(voltage_mv);
            p_epd->force_full_refresh = true;
            epd_display_request(p_epd, timestamp());
            return;
        }
    }
    if (blink) EPD_LED_BLINK();
}
