/*!
 * @file epd_service_cmd.c
 * @brief EPD 服务的命令处理、配置校验/应用、命令队列执行
 *
 * 由 EPD/service/EPD_service.c 按职责拆出；跨模块接口声明见
 * epd_service_internal.h。只搬代码，不改行为/协议/时序。
 */

#include "EPD_service.h"
#include "epd_service_internal.h"
#include "epd_board.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "main.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_pwr_mgmt.h"
#include "epd_clock_refresh.h"
#include "epd_cmd_policy.h"
#include "epd_refresh_policy.h"
#include "calendar_annotations.h"
#include "calendar_annotations_store.h"
#include "resource_transfer.h"
#include "resource_protocol.h"
#include "resource_store.h"
#include "todo_items.h"
#include "fonts.h"

// SET_TIME 频率限制(秒)。同一时间窗口内仅允许一次 SET_TIME 触发屏幕更新(PRD §4.3)。
#define EPD_SET_TIME_RATE_LIMIT_S 10U

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
    if (p_epd->config.display_mode != mode || epd_config_read_pending()) {
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
void epd_enter_picture_mode(ble_epd_t *p_epd)
{
    epd_switch_from_clock_mode(p_epd);
    epd_update_display_mode(p_epd, MODE_PICTURE);
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
    /* The wire-format prefix contains pin bytes and the model ID. Validate
     * it in place, keeping optional pins disabled by 0xFF. */
    const uint8_t *pins = (const uint8_t *)cfg;
    for (unsigned i = 0; i < offsetof(epd_config_t, display_mode); i++) {
        if (i == offsetof(epd_config_t, model_id)) continue;
        if (i >= offsetof(epd_config_t, bs_pin) && pins[i] == 0xFFU) continue;
        if (!nrf_gpio_pin_present_check(pins[i])) return false;
    }
    return true;
}

/* 0xFF selects the board's default panel; other unknown IDs are invalid. */
bool epd_config_validate(const epd_config_t *cfg)
{
    if (!epd_config_pins_valid(cfg) || !epd_display_mode_valid(cfg->display_mode) ||
        cfg->week_start > 6U ||
        (cfg->model_id != 0xFFU && !epd_model_supported(cfg->model_id))) return false;
    return true;
}

void epd_config_defaults(epd_config_t *cfg)
{
    *cfg = (epd_config_t)EPD_BOARD_DEFAULT_CONFIG;
    cfg->display_mode = MODE_CLOCK_CALENDAR;
}

/* Main only. Retire the old hardware before loading new pins; the connection,
 * INIT or GUI worker acquires/rebuilds it on its next turn. Display-only edits
 * keep the GPIO reference and panel instance intact. */
bool epd_config_apply(ble_epd_t *p_epd, epd_config_t *candidate)
{
    if (!epd_config_validate(candidate)) return false;
    if (memcmp(&p_epd->config, candidate, sizeof(*candidate)) == 0) return true;
    bool hardware_changed = memcmp(&p_epd->config, candidate,
                                   offsetof(epd_config_t, display_mode)) != 0;
    if (hardware_changed) {
        if (p_epd->epd != NULL) {
            EPD_ClearBusyAbort();
            p_epd->epd->drv->sleep(p_epd->epd);
            p_epd->epd = NULL;
        }
        if (p_epd->gpio_connection_ref) {
            EPD_GPIO_Uninit();
            p_epd->gpio_connection_ref = false;
        }
    }
    p_epd->config = *candidate;
    if (hardware_changed) EPD_GPIO_Load(&p_epd->config);
    p_epd->force_full_refresh = true;
    epd_clock_refresh_invalidate();
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
    if (!epd_config_apply(p_epd, &candidate)) return;
    epd_config_write(&p_epd->config);
}

static void cmd_init(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 初始化驱动，回 MTU/时间/电压 */
{
    if (p_epd->init_pending || p_epd->init_ack_pending) return;
    p_epd->init_model = length > 1U ? p_data[1] : p_epd->config.model_id;
    p_epd->init_token = length > 2U ? p_data[2] : 0U;
    p_epd->init_prior_commands = p_epd->cmd_queue.count;
    p_epd->init_pending = true;
}

static void cmd_display(ble_epd_t *p_epd, uint8_t *data, uint16_t length)
{
    uint32_t generation = p_epd->connection_generation;
    rs_result_t result = RS_OK;
    if (length > 3U) {
        epd_command_ack(p_epd, data, length, RS_INVALID);
        return;
    }
    EPD_ClearBusyAbort();
    epd_service_busy_error_clear(p_epd);
    if (!epd_ensure_display(p_epd) || EPD_BusyAbortRequested() || epd_service_busy_error(p_epd) ||
        p_epd->epd->drv == NULL ||
        (data[0] == EPD_CMD_CLEAR ? p_epd->epd->drv->clear == NULL :
                                  p_epd->epd->drv->refresh == NULL)) result = RS_IO;
    else {
        epd_enter_picture_mode(p_epd);
        if (data[0] == EPD_CMD_CLEAR)
            p_epd->epd->drv->clear(p_epd->epd, length > 1U ? data[1] : true);
        else {
            p_epd->epd->drv->refresh(p_epd->epd);
            p_epd->transfer_ctx.transfer_active = false;
        }
        if (EPD_BusyAbortRequested() || epd_service_busy_error(p_epd)) result = RS_IO;
    }
    CRITICAL_REGION_ENTER();
    if (generation == p_epd->connection_generation && !p_epd->disconnect_pending)
        epd_command_ack(p_epd, data, length, result);
    CRITICAL_REGION_EXIT();
}

static void cmd_sleep(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 面板睡眠 */
{
    (void)p_data; (void)length;
    if (p_epd->epd == NULL) return;
    p_epd->epd->drv->sleep(p_epd->epd);
    p_epd->epd = NULL;
    p_epd->force_full_refresh = true;
    epd_clock_refresh_invalidate();
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
}

static void cmd_sync_time_silent(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 只改时间戳，不刷新 */
{
    if (length < 5) return;
    uint32_t ts = epd_parse_time_payload(p_data, length);
    set_timestamp(ts);
    NRF_LOG_INFO("[EPD]: Time synced silently\n");
    epd_send_time(p_epd);
    epd_send_mtu(p_epd);
}

static void cmd_set_week_start(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 0=周日，立即写配置 */
{
    if (length < 2) return;
    if (p_data[1] < 7 &&
        (p_data[1] != p_epd->config.week_start || epd_config_read_pending())) {
        p_epd->config.week_start = p_data[1];
        epd_config_write(&p_epd->config);
    }
}

static ble_epd_t *s_anno_epd;

/* ble_epd_init()（EPD_service.c）在初始化时登记服务实例；静态量仍归本模块。 */
void epd_annotations_set_service(ble_epd_t *p_epd)
{
    s_anno_epd = p_epd;
}

static void epd_refresh_annotations(ble_epd_t *p_epd)
{
    if (p_epd == NULL || p_epd->config.display_mode == MODE_PICTURE) {
        return;
    }
    p_epd->force_full_refresh = true;
    ble_epd_on_timer(p_epd, timestamp(), true);
}


void annotations_store_applied(void)
{
    epd_refresh_annotations(s_anno_epd);
}

calendar_annotations_tx_result_t annotations_tx_send(void *context,
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
}

bool epd_todo_font_valid(void)
{
    if (todo_items_font_slot() != 3U ||
        GUI_TodoFontMatches(todo_items_font_size(), todo_items_font_key())) return true;
    todo_items_suspend_display();
    return false;
}

static void cmd_set_mode(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 切显示模式并立刻刷新 */
{
    if (length < 2) return;
    uint32_t generation = p_epd->connection_generation;
    if (length != EPD_MODE_REQUEST_SIZE ||
        !epd_display_mode_valid(p_data[1]) ||
        p_data[3] > 6U) {
        epd_command_ack(p_epd, p_data, length, RS_INVALID);
        return;
    }
    /* Mode commands run in main: retry a transient startup read failure before
     * rejecting TODO. Other modes remain usable if this optional package is bad. */
    if (epd_is_todo_mode((display_mode_t)p_data[1]) && !todo_items_ready() && !todo_items_init()) {
        epd_command_ack(p_epd, p_data, length, RS_INVALID);
        return;
    }
    if (epd_is_todo_mode((display_mode_t)p_data[1]) && !epd_todo_font_valid()) {
        epd_command_ack(p_epd, p_data, length, RS_INVALID);
        return;
    }
    set_timestamp(epd_parse_time_payload(p_data + 3, 6U));
    if (p_epd->config.week_start != p_data[3]) {
        p_epd->config.week_start = p_data[3];
        epd_config_write(&p_epd->config);
    }
    {
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
    if (generation == p_epd->connection_generation && !p_epd->disconnect_pending)
        epd_command_ack(p_epd, p_data, length, RS_OK);
}

static void cmd_set_config(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 整包配置写入并全刷 */
{
    if (length < 2) return;
    epd_config_t candidate = p_epd->config;
    memcpy(&candidate, &p_data[1], (length - 1 > EPD_CONFIG_SIZE) ? EPD_CONFIG_SIZE : length - 1);
    if (!epd_config_apply(p_epd, &candidate)) return;
    epd_config_write(&p_epd->config);

    // 配置更新后强制全刷并立刻刷新，使模式、周起始日等马上可见。
    p_epd->force_full_refresh = true;
    ble_epd_on_timer(p_epd, timestamp(), true);
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
 * - 刷新 (EPD_CMD_REFRESH)
 * - 睡眠 (EPD_CMD_SLEEP)
 * - 设置时间 (EPD_CMD_SET_TIME)
 * - 设置模式 (EPD_CMD_SET_MODE)
 * - CRC 分块写入图像 (EPD_CMD_WRITE_BLOCK)
 * - 系统控制 (EPD_CMD_SYS_RESET/SLEEP/CFG_ERASE)
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] p_data  命令数据指针
 * @param[in] length  命令数据长度
 */
void epd_execute_command(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    if (p_data == NULL || length == 0) return;
    if ((resource_protocol_entered() || resource_transfer_running()) &&
        resource_blocks_display_command(p_data[0])) {
        epd_command_ack(p_epd, p_data, length, RS_BUSY);
        return;
    }


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
        case EPD_CMD_CLEAR:
        case EPD_CMD_REFRESH:         cmd_display(p_epd, p_data, length);         break;
        case EPD_CMD_SLEEP:           cmd_sleep(p_epd, p_data, length);           break;
        case EPD_CMD_SET_TIME:        cmd_set_time(p_epd, p_data, length);        break;
        case EPD_CMD_SYNC_TIME_SILENT: cmd_sync_time_silent(p_epd, p_data, length); break;
        case EPD_CMD_SET_WEEK_START:  cmd_set_week_start(p_epd, p_data, length);  break;
        case EPD_CMD_SET_ANNOTATIONS: cmd_set_annotations(p_epd, p_data, length); break;
        case EPD_CMD_SET_MODE:        cmd_set_mode(p_epd, p_data, length);        break;
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
void epd_process_next_command(void* p_event_data, uint16_t event_size) {
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
    epd_service_schedule_cmd(p_epd);
}
