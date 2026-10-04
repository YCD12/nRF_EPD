/* Real service initialization/notification code with storage and BLE shims. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_protocol.h"
#include "epd_cmd_policy.h"
#include "resource_store.h"
#include "calendar_annotations_tx.h"
#include "epd_offline.h"

#define EPD_CMD_QUEUE_SIZE 2U
#define EPD_CMD_MAX_DATA_LEN 16U
#include "epd_command_queue.h"
#include "epd_display_queue.h"

enum { NRF_SUCCESS, NRF_ERROR_NULL, NRF_ERROR_INVALID_STATE,
       NRF_ERROR_INVALID_PARAM, NRF_ERROR_RESOURCES, TEST_CONFIG_ERROR };
#define BLE_CONN_HANDLE_INVALID UINT16_MAX
#define BLE_EPD_MAX_DATA_LEN 244U
#define BLE_GATT_HVX_NOTIFICATION 1U
#define MODE_CLOCK_CALENDAR 2U
#define EPD_UC8179_750_BWR 7U

typedef struct { uint16_t value_handle; } ble_gatts_char_handles_t;
typedef struct { unsigned unused; } epd_model_t;
typedef struct {
    uint16_t handle;
    uint8_t type;
    uint8_t *p_data;
    uint16_t *p_len;
} ble_gatts_hvx_params_t;

/* PRODUCTION_SERVICE_TYPES */

static ble_epd_t *s_anno_epd;
static epd_config_t stored_config;
static uint32_t config_result, service_result, hvx_result;
static unsigned config_reads, config_writes, store_inits, gpio_loads, service_inits;
static unsigned hvx_calls, critical_depth;
static void *resource_context;
static void (*applied_callback)(void);
static uint8_t sent_data[BLE_EPD_MAX_DATA_LEN];
static uint16_t sent_length;

void test_critical_enter(void) { critical_depth++; }
void test_critical_exit(void) { assert(critical_depth); critical_depth--; }
static bool nrf_gpio_pin_present_check(uint8_t pin) { return pin < 32U; }
static uint16_t EPD_ReadVoltageAndCache(void) { return 3300U; }

uint32_t epd_config_init(epd_config_t *cfg)
{
    (void)cfg;
    return config_result;
}
void epd_config_read(epd_config_t *cfg) { *cfg = stored_config; config_reads++; }
void epd_config_write(epd_config_t *cfg) { stored_config = *cfg; config_writes++; }
bool epd_config_empty(epd_config_t *cfg)
{
    const uint8_t *bytes = (const uint8_t *)cfg;
    for (unsigned i = 0; i < sizeof(*cfg); i++) if (bytes[i] != 0xFFU) return false;
    return true;
}
static void epd_resource_event(void) {}
static void resource_protocol_set_handler(void (*handler)(void), void *context)
{
    assert(handler == epd_resource_event);
    resource_context = context;
}
static void annotations_store_applied(void) {}
static void calendar_annotations_store_init(void) { store_inits++; }
static void calendar_annotations_store_on_applied(void (*callback)(void))
{
    applied_callback = callback;
}
static void EPD_GPIO_Load(epd_config_t *config)
{
    assert(memcmp(config, &stored_config, sizeof(*config)) == 0);
    gpio_loads++;
}
static uint32_t epd_service_init(ble_epd_t *p)
{
    service_inits++;
    p->service_handle = 7U;
    p->char_handles.value_handle = 9U;
    return service_result;
}
static uint32_t sd_ble_gatts_hvx(uint16_t conn, ble_gatts_hvx_params_t *params)
{
    assert(critical_depth >= 1U && critical_depth <= 2U && conn == 3U);
    assert(params->handle == 9U && params->type == BLE_GATT_HVX_NOTIFICATION);
    assert(*params->p_len <= sizeof(sent_data));
    sent_length = *params->p_len;
    memcpy(sent_data, params->p_data, sent_length);
    hvx_calls++;
    return hvx_result;
}

/* PRODUCTION_HANDLERS */

static void reset_shims(void)
{
    stored_config = (epd_config_t){
        .mosi_pin = 20, .sclk_pin = 19, .cs_pin = 6, .dc_pin = 5,
        .rst_pin = 4, .busy_pin = 3, .bs_pin = 2, .model_id = 7,
        .wakeup_pin = 255, .led_pin = 18, .en_pin = 7,
        .display_mode = MODE_CLOCK_CALENDAR, .week_start = 1,
        .full_refresh_minutes = 30,
    };
    config_result = service_result = hvx_result = NRF_SUCCESS;
    config_reads = config_writes = store_inits = gpio_loads = service_inits = 0U;
    hvx_calls = critical_depth = 0U;
    resource_context = NULL;
    applied_callback = NULL;
    s_anno_epd = NULL;
}

static void assert_idle(const ble_epd_t *p)
{
    assert(p->conn_handle == BLE_CONN_HANDLE_INVALID);
    assert(p->max_data_len == BLE_EPD_MAX_DATA_LEN);
    assert(!p->is_notification_enabled && p->epd == NULL);
    assert(!p->force_full_refresh && !p->is_refreshing && !p->low_battery_screen_active);
    assert(!p->gpio_connection_ref && !p->disconnect_pending && !p->connection_generation);
    assert(!p->init_pending && !p->init_running && !p->init_ack_pending);
    assert(!p->init_model && !p->init_token && !p->init_prior_commands);
    assert(!p->init_ack_token && !p->init_status);
    assert(!p->image_last_activity && !p->last_full_clear && !p->last_refresh_start && !p->last_set_time_ts);
    assert(!p->transfer_ctx.transfer_active && !p->transfer_ctx.session_id);
    assert(!p->transfer_ctx.total_blocks && !p->transfer_ctx.received_blocks);
    assert(!p->display_queue.timestamp && !p->display_queue.pending);
    assert(!p->display_queue.processing && !p->display_queue.scheduled);
    assert(!p->cmd_queue.count && !p->cmd_queue.head && !p->cmd_queue.tail);
    assert(!p->cmd_queue.is_processing && !p->cmd_queue.is_scheduled);
    assert(p->annotations_tx.generation == 1U && !p->annotations_tx.offset);
    assert(!p->annotations_tx.active && !p->annotations_tx.waiting_tx);
    assert(!p->annotations_rx_last_activity && !p->annotations_rx_abort_requested);
    assert(!p->record_ack_pending && !p->config_read_pending && !p->config_read_token);
    assert(p->offline.state == EPD_ONLINE && !p->offline.notifications);
    assert(critical_depth == 0U);
}

static void test_init(void)
{
    ble_epd_t p;
    reset_shims();
    assert(ble_epd_init(NULL) == NRF_ERROR_NULL);
    assert(!config_reads && resource_context == NULL);

    /* No caller-side pre-clear: every runtime field starts with stale bytes. */
    memset(&p, 0xA5, sizeof(p));
    assert(ble_epd_init(&p) == NRF_SUCCESS);
    assert_idle(&p);
    assert(memcmp(&p.config, &stored_config, sizeof(p.config)) == 0);
    assert(resource_context == &p && s_anno_epd == &p);
    assert(applied_callback == annotations_store_applied);
    assert(config_reads == 1U && !config_writes && store_inits == 1U);
    assert(gpio_loads == 1U && service_inits == 1U);

    reset_shims();
    memset(&p, 0xA5, sizeof(p));
    config_result = TEST_CONFIG_ERROR;
    assert(ble_epd_init(&p) == TEST_CONFIG_ERROR);
    assert_idle(&p);
    assert(!config_reads && !config_writes && !store_inits && !gpio_loads && !service_inits);
    assert(s_anno_epd == NULL && applied_callback == NULL);

    reset_shims();
    memset(&p, 0xA5, sizeof(p));
    memset(&stored_config, 0xFF, sizeof(stored_config));
    assert(ble_epd_init(&p) == NRF_SUCCESS);
    assert_idle(&p);
    assert(config_writes == 1U && p.config.mosi_pin == 20U);
    assert(p.config.display_mode == MODE_CLOCK_CALENDAR);
    assert(p.config.week_start == 0U && p.config.full_refresh_minutes == 0U);

    /* Invalid persisted display/model fields recover the complete defaults,
     * not just the pin prefix, with the same rules used by config updates. */
    for (unsigned field = 0; field < 3U; field++) {
        reset_shims();
        if (field == 0U) stored_config.display_mode = 4U;
        if (field == 1U) stored_config.week_start = 7U;
        if (field == 2U) stored_config.model_id = 8U;
        assert(ble_epd_init(&p) == NRF_SUCCESS);
        assert(config_writes == 1U && p.config.model_id == EPD_UC8179_750_BWR);
        assert(p.config.display_mode == MODE_CLOCK_CALENDAR);
        assert(p.config.week_start == 0U && p.config.full_refresh_minutes == 0U);
    }
    reset_shims();
    stored_config.model_id = 0xFFU; /* Documented default-panel alias is valid. */
    assert(ble_epd_init(&p) == NRF_SUCCESS);
    assert(!config_writes && p.config.model_id == 0xFFU && p.config.week_start == 1U);
}

static void test_notify(void)
{
    ble_epd_t p;
    uint8_t text[] = {'v', '=', '3', '3', '0', '0'};
    uint8_t final_ack[] = {EPD_RSP_RESOURCE, 8U, 42U, RS_OK};
    reset_shims();
    assert(ble_epd_init(&p) == NRF_SUCCESS);
    assert(ble_epd_string_send(NULL, text, sizeof(text)) == NRF_ERROR_INVALID_STATE);
    assert(ble_epd_string_send(&p, text, sizeof(text)) == NRF_ERROR_INVALID_STATE);
    p.conn_handle = 3U;
    assert(ble_epd_string_send(&p, text, sizeof(text)) == NRF_ERROR_INVALID_STATE);
    p.is_notification_enabled = true;
    assert(ble_epd_string_send(&p, NULL, 0U) == NRF_ERROR_INVALID_PARAM);
    assert(ble_epd_string_send(&p, text, p.max_data_len + 1U) == NRF_ERROR_INVALID_PARAM);
    assert(!hvx_calls && !p.offline.notifications && !critical_depth);

    hvx_result = NRF_ERROR_RESOURCES;
    p.offline.state = EPD_OFFLINE_ACK;
    assert(ble_epd_string_send(&p, final_ack, sizeof(final_ack)) == NRF_ERROR_RESOURCES);
    assert(hvx_calls == 1U && !p.offline.notifications && p.offline.state == EPD_OFFLINE_ACK);
    hvx_result = NRF_SUCCESS;
    assert(ble_epd_string_send(&p, text, sizeof(text)) == NRF_SUCCESS);
    assert(p.offline.notifications == 1U && p.offline.state == EPD_OFFLINE_ACK);
    assert(sent_length == sizeof(text) && !memcmp(sent_data, text, sizeof(text)));
    final_ack[3] = RS_IO;
    assert(ble_epd_string_send(&p, final_ack, sizeof(final_ack)) == NRF_SUCCESS);
    assert(p.offline.notifications == 2U && p.offline.state == EPD_OFFLINE_ACK);
    final_ack[3] = RS_OK;
    assert(ble_epd_string_send(&p, final_ack, sizeof(final_ack)) == NRF_SUCCESS);
    assert(p.offline.notifications == 3U && p.offline.state == EPD_OFFLINE_DRAIN);
    epd_offline_completed(&p.offline, 2U);
    assert(p.offline.notifications == 1U && p.offline.state == EPD_OFFLINE_DRAIN);
    epd_offline_completed(&p.offline, 1U);
    assert(!p.offline.notifications && p.offline.state == EPD_OFFLINE_READY && !critical_depth);
}

static void test_control_retry(void)
{
    ble_epd_t p;
    reset_shims();
    assert(ble_epd_init(&p) == NRF_SUCCESS);
    p.conn_handle = 3U;
    p.is_notification_enabled = true;
    p.initial_notify = 1U;
    hvx_result = NRF_ERROR_RESOURCES;
    epd_control_notifications_process(&p);
    assert(p.initial_notify == 1U && hvx_calls == 1U);
    hvx_result = NRF_SUCCESS;
    epd_control_notifications_process(&p);
    assert(p.initial_notify == 2U && sent_length == sizeof(epd_config_t));
    epd_control_notifications_process(&p);
    assert(p.initial_notify == 3U && sent_length == 7U);
    epd_control_notifications_process(&p);
    assert(p.initial_notify == 0U && sent_length == 6U);
    p.mode_ack_pending = true;
    memcpy(p.mode_ack, (uint8_t[]){EPD_RSP_MODE, 9U, RS_OK, 2U}, 4U);
    hvx_result = NRF_ERROR_RESOURCES;
    epd_control_notifications_process(&p);
    assert(p.mode_ack_pending);
    hvx_result = NRF_SUCCESS;
    epd_control_notifications_process(&p);
    assert(!p.mode_ack_pending && sent_length == 4U && sent_data[1] == 9U);
    p.initial_notify = 1U;
    p.mode_ack_pending = true;
    p.is_notification_enabled = false;
    epd_control_notifications_process(&p);
    assert(!p.initial_notify && !p.mode_ack_pending);
}

int main(void)
{
    test_init();
    test_notify();
    test_control_retry();
    puts("PASS: service-owned initialization, config failure and notification/ACK validation");
    return 0;
}
