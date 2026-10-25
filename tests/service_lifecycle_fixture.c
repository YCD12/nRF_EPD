#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "epd_refresh_policy.h"
#include "epd_protocol.h"
#include "epd_cmd_policy.h"
#include "EPD_config.h"
#include "epd_offline.h"
#include "resource_protocol.h"
#include "resource_layout.h"
#define EPD_CMD_QUEUE_SIZE 2U
#define EPD_CMD_MAX_DATA_LEN 16U
#include "epd_command_queue.h"
void test_critical_enter(void) {}
void test_critical_exit(void) {}
uint32_t app_sched_event_put(void *data, uint16_t length, app_sched_event_handler_t worker) {
    (void)data; (void)length; (void)worker; return 0;
}

#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_WARNING(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
#define UNUSED_PARAMETER(x) ((void)(x))
#define BLE_CONN_HANDLE_INVALID 65535U
#define BLE_GATT_ATT_MTU_DEFAULT 23U
#define NRF_ERROR_RESOURCES 19U
#define NRF_ERROR_INVALID_STATE 8U
#define EPD_IMAGE_RX_TIMEOUT_S 30U
typedef unsigned epd_model_id_t;
#define EPD_UC8179_750_BWR 7U
typedef struct epd_model epd_model_t;
typedef struct { void (*sleep)(const epd_model_t *); } driver_t;
struct epd_model { unsigned id; const driver_t *drv; };
typedef struct { bool transfer_active; uint8_t session_id; } image_transfer_ctx_t;
typedef struct {
    unsigned conn_handle, connection_generation, max_data_len;
    epd_offline_t offline;
    bool is_notification_enabled, gpio_connection_ref, disconnect_pending;
    bool init_pending, init_running, init_ack_pending, is_refreshing, force_full_refresh;
    volatile uint8_t initial_notify;
    volatile bool mode_ack_pending;
    uint8_t mode_ack[4];
    uint8_t init_model, init_token, init_ack_token, init_status, init_prior_commands;
    unsigned image_last_activity;
    const epd_model_t *epd;
    epd_config_t config;
    epd_cmd_queue_t cmd_queue;
    image_transfer_ctx_t transfer_ctx;
    unsigned annotations_tx;
    bool annotations_rx_abort_requested;
    bool config_read_pending;
    uint8_t config_read_token;
} ble_epd_t;
typedef struct { struct { struct { unsigned conn_handle; } gap_evt; } evt; } ble_evt_t;
static unsigned refs, init_calls, sleeps, requests, now, ack_error, ack_calls, gpio_calls;
static bool busy_timeout, interrupt_init, busy_abort;
static uint8_t last_ack[5];
static ble_epd_t *current;
static unsigned commands;
static void epd_execute_command(ble_epd_t *p, uint8_t *data, uint16_t length) {
    (void)p; assert(data[0] == 0 && length == 1); commands++;
}
static void on_disconnect(ble_epd_t *, ble_evt_t *);
static void cmd_init(ble_epd_t *, uint8_t *, uint16_t);
static void panel_sleep(const epd_model_t *p) { (void)p; assert(refs); sleeps++; }
static const driver_t driver = {panel_sleep};
static const epd_model_t panel = {7, &driver};
static const epd_model_t *epd_init(epd_model_id_t model) {
    (void)model;
    assert(refs && current->init_running && current->is_refreshing);
    init_calls++;
    if (interrupt_init) {
        interrupt_init = false;
        on_disconnect(current, NULL);
        current->conn_handle = 2;
        uint8_t request[] = {1, 9, 43};
        cmd_init(current, request, sizeof(request));
    }
    return &panel;
}
static void EPD_GPIO_Init(void) { refs++; }
static void EPD_GPIO_Load(epd_config_t *cfg) { (void)cfg; }
static void EPD_GPIO_Uninit(void) { assert(refs); refs--; }
#define EPD_LED_BLINK() ((void)0)
static void EPD_ClearBusyAbort(void) { busy_abort = false; }
static void EPD_RequestBusyAbort(void) { busy_abort = true; }
static void UC8179_ClearBusyTimeout(void) {}
static bool UC8179_HadBusyTimeout(void) { return busy_timeout; }
static void epd_clock_refresh_invalidate(void) {}
static void epd_display_retry_failed(void) { assert(!"unexpected refresh failure"); }
#define epd_config_write(p) ((void)(p))
static unsigned app_uptime_seconds(void) { return now; }
static unsigned timestamp(void) { return 1000U + now; }
static void epd_display_request(ble_epd_t *p, unsigned ts) { (void)p; (void)ts; requests++; }
static void calendar_annotations_tx_cancel(unsigned *p) { *p = 0; }
static bool calendar_annotations_rx_active(void) { return true; }
static unsigned ble_epd_notification_try_send(ble_epd_t *p, uint8_t *data, unsigned len) {
    assert(p->conn_handle != BLE_CONN_HANDLE_INVALID && len == sizeof(last_ack));
    memcpy(last_ack, data, len); ack_calls++; return ack_error;
}
static char last_text[16];
static uint16_t voltage_mv = 3300U;
static uint16_t EPD_ReadVoltageAndCache(void) { return voltage_mv; }
static unsigned ble_epd_string_send(ble_epd_t *p, uint8_t *data, uint16_t length) {
    (void)p;
    assert(length < sizeof(last_text));
    memcpy(last_text, data, length);
    last_text[length] = '\0';
    return 0;
}
static void app_feed_wdt(void) {}
static bool resources_ready;
static unsigned resource_reloads;
static rs_result_t reload_result = RS_OK;
static bool epd_resource_idle(const ble_epd_t *p) { (void)p; return true; }
static void epd_resources_invalidate_partitions(uint16_t changed) { (void)changed; resources_ready = false; }
static bool epd_resources_ready(void) { return resources_ready; }
static rs_result_t epd_resources_apply(void) {
    resource_reloads++;
    resources_ready = reload_result == RS_OK;
    return reload_result;
}
void epd_config_read(epd_config_t *cfg) { *cfg = current->config; }
bool epd_config_read_pending(void) { return false; }
static void calendar_annotations_store_init(void) {}
static bool calendar_annotations_store_valid(void) { return true; }
static bool todo_items_available = true;
static bool todo_items_loaded = true;
static unsigned todo_reloads, todo_invalidations;
static bool todo_items_init(void) { todo_reloads++; todo_items_loaded = todo_items_available; return todo_items_loaded; }
static bool todo_items_ready(void) { return todo_items_loaded; }
static void todo_items_invalidate(void) { todo_invalidations++; }
static uint8_t todo_font_slot = 1U;
static bool todo_font_matches = true;
static unsigned todo_font_checks;
static uint8_t todo_items_font_slot(void) { return todo_font_slot; }
static uint8_t todo_items_font_size(void) { return 12U; }
static uint32_t todo_items_font_key(void) { return 123U; }
static void todo_items_suspend_display(void) { todo_items_loaded = false; todo_items_invalidate(); }
static bool GUI_TodoFontMatches(uint8_t size, uint32_t key)
{ assert(size == 12U && key == 123U); todo_font_checks++; return todo_font_matches; }
static void epd_display_complete(ble_epd_t *p) { (void)p; }
static void nrf_delay_ms(unsigned ms) { (void)ms; }
static void sleep_mode_enter(void) { assert(0); }
enum { INPUT, INPUT_PULLUP, INPUT_PULLDOWN, OUTPUT, DEFAULT };
enum { NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_PULLDOWN };
static bool nrf_gpio_pin_present_check(unsigned pin) { return pin < 32U; }
static void nrf_gpio_cfg_default(unsigned pin) { assert(pin < 32U); gpio_calls++; }
static void nrf_gpio_cfg_output(unsigned pin) { nrf_gpio_cfg_default(pin); }
static void nrf_gpio_cfg_input(unsigned pin, unsigned pull) { (void)pull; nrf_gpio_cfg_default(pin); }

/* PRODUCTION_HANDLERS */

int main(void) {
    ble_epd_t service = {.conn_handle = BLE_CONN_HANDLE_INVALID, .max_data_len = 244,
                        .config = {.model_id = 9, .display_mode = MODE_CLOCK}};
    current = &service;
    /* Exact legacy text protocol, including the largest field values. */
    now = 0U;
    epd_send_time(current); assert(strcmp(last_text, "t=1000") == 0);
    now = UINT32_MAX - 1000U;
    epd_send_time(current); assert(strcmp(last_text, "t=4294967295") == 0);
    now = UINT32_MAX - 999U;
    epd_send_time(current); assert(strcmp(last_text, "t=0") == 0);
    now = 0U;
    voltage_mv = UINT16_MAX;
    epd_send_voltage(current); assert(strcmp(last_text, "v=65535") == 0);
    voltage_mv = 3300U;
    epd_send_voltage(current); assert(strcmp(last_text, "v=3300") == 0);
    current->max_data_len = UINT16_MAX;
    epd_send_mtu(current); assert(strcmp(last_text, "mtu=65535") == 0);
    current->max_data_len = 244U;
    epd_send_mtu(current); assert(strcmp(last_text, "mtu=244") == 0);
    ble_evt_t event = {.evt = {.gap_evt = {.conn_handle = 1}}};
    on_connect(current, &event);
    assert(current->max_data_len == 20U); /* Never reuse a previous connection's MTU. */
    assert(refs == 0); /* The IRQ does no hardware work. */
    epd_connection_process(current);
    assert(refs == 1 && current->gpio_connection_ref);
    current->config = (epd_config_t){.mosi_pin=1, .sclk_pin=2, .cs_pin=3,
        .dc_pin=4, .rst_pin=5, .busy_pin=6, .bs_pin=255, .en_pin=255,
        .led_pin=255, .wakeup_pin=255, .model_id=7, .display_mode=MODE_CLOCK};
    assert(epd_config_pins_valid(&current->config));
    epd_config_t original = current->config;
    uint8_t pins[] = {0, 255, 2, 3, 4, 5, 6, 255};
    cmd_set_pins(current, pins, sizeof(pins));
    assert(memcmp(&original, &current->config, sizeof(original)) == 0 && refs == 1);
    pins[1] = 31;
    cmd_set_pins(current, pins, sizeof(pins));
    assert(current->config.mosi_pin == 31 && refs == 0 && !current->gpio_connection_ref);
    epd_connection_process(current);
    assert(refs == 1 && current->gpio_connection_ref);
    current->config.led_pin = 32;
    assert(!epd_config_pins_valid(&current->config));
    current->config = original;
    uint8_t init[] = {1, 9, 42};
    cmd_init(current, init, sizeof(init));
    assert(init_calls == 0 && current->init_pending);
    current->is_refreshing = true;
    epd_init_process(current);
    assert(init_calls == 0);
    current->is_refreshing = false;
    ack_error = NRF_ERROR_RESOURCES;
    epd_init_process(current);
    assert(init_calls == 1 && current->init_ack_pending && !current->is_refreshing);
    ack_error = 0;
    epd_init_process(current);
    assert(init_calls == 1 && !current->init_ack_pending && ack_calls == 2);
    assert(last_ack[0] == EPD_RSP_INIT && last_ack[1] == 42 && last_ack[2] == 0 && last_ack[3] == 20);
    uint8_t queued[] = {0};
    assert(epd_cmd_queue_push(&current->cmd_queue, queued, sizeof(queued)));
    cmd_init(current, init, sizeof(init));
    assert(current->init_prior_commands == 1);
    assert(epd_cmd_queue_push(&current->cmd_queue, queued, sizeof(queued)));
    epd_init_process(current);
    assert(init_calls == 1);
    epd_process_next_command(&current, sizeof(current));
    assert(commands == 1 && current->init_prior_commands == 0);
    epd_process_next_command(&current, sizeof(current));
    assert(commands == 1); /* Commands after INIT stay behind it. */
    epd_init_process(current);
    assert(init_calls == 2);
    epd_process_next_command(&current, sizeof(current));
    assert(commands == 2 && current->cmd_queue.count == 0);

    EPD_GPIO_Init(); /* A GUI refresh holds its own reference. */
    current->is_refreshing = true;
    uint8_t reset[] = {EPD_CMD_RESET_TRANSFER, 7};
    cmd_reset_transfer(current, reset, sizeof(reset));
    assert(busy_abort && current->transfer_ctx.transfer_active);
    finish_refresh(current);
    assert(refs == 1 && sleeps == 0 && !current->is_refreshing);
    current->cmd_queue.count = 2;
    on_disconnect(current, NULL);
    assert(refs == 1 && sleeps == 0 && current->disconnect_pending && current->cmd_queue.count == 0);
    epd_connection_process(current);
    assert(refs == 0 && sleeps == 1 && !current->transfer_ctx.transfer_active && requests == 1);
    assert(current->force_full_refresh && !busy_abort);

    on_connect(current, &event);
    epd_connection_process(current);
    now = UINT32_MAX - 10U;
    cmd_reset_transfer(current, reset, sizeof(reset));
    now += 29U;
    epd_connection_process(current);
    assert(current->transfer_ctx.transfer_active);
    now++;
    epd_connection_process(current);
    assert(!current->transfer_ctx.transfer_active && requests == 2);

    interrupt_init = true;
    cmd_init(current, init, sizeof(init));
    unsigned sent = ack_calls;
    epd_init_process(current);
    assert(ack_calls == sent && current->init_pending && current->disconnect_pending);
    epd_connection_process(current);
    busy_timeout = true;
    epd_init_process(current);
    assert(last_ack[1] == 43 && last_ack[2] == 1 && refs == 1);
    on_disconnect(current, NULL);
    epd_connection_process(current);
    assert(refs == 0);

    for (unsigned mode = INPUT; mode <= DEFAULT; mode++) {
        pinMode(255, mode);
        pinMode(32, mode);
    }
    assert(gpio_calls == 0);
    pinMode(31, DEFAULT);
    assert(gpio_calls == 1);
    /* Connect + RESET + disconnect can all interrupt an offline GUI refresh,
     * before main has ever acquired the connection's GPIO reference. */
    current->epd = &panel;
    current->is_refreshing = true;
    EPD_GPIO_Init();
    on_connect(current, &event);
    cmd_reset_transfer(current, reset, sizeof(reset));
    on_disconnect(current, NULL);
    finish_refresh(current);
    assert(refs == 0 && current->epd == NULL);
    epd_connection_process(current); /* Must not issue SPI after GPIO release. */
    assert(refs == 0 && !current->transfer_ctx.transfer_active);
    /* An activation that outlives its connection cannot lock the new peer in ACK. */
    on_connect(current, &event);
    current->offline.state = EPD_OFFLINE_ACK;
    sent = ack_calls;
    assert(resource_notification_send(current, current->connection_generation - 1,
                                     last_ack, sizeof(last_ack)) == NRF_ERROR_INVALID_STATE);
    assert(ack_calls == sent && current->offline.state == EPD_ONLINE);
    assert(resource_notification_send(current, current->connection_generation,
                                     last_ack, sizeof(last_ack)) == 0);
    assert(ack_calls == sent + 1);

    /* No second full read between successful LEAVE and RUN_OFFLINE. */
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_UI_RESOURCES) == RS_OK);
    assert(resource_reloads == 1 && resources_ready);
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0) == RS_OK);
    assert(resource_reloads == 1);
    assert(epd_resource_event(current, RESOURCE_INVALIDATE, 1U << RS_PART_UI_RESOURCES) == RS_OK);
    reload_result = RS_INVALID;
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_UI_RESOURCES) == RS_INVALID);
    assert(!resources_ready && resource_reloads == 2);
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0) == RS_INVALID);
    assert(resource_reloads == 3); /* Failure never enables the cached success path. */
    assert(RS_PART_TODO_ITEMS == 9U);
    reload_result = RS_OK;
    unsigned before_todo_reload = todo_reloads;
    unsigned before_todo_invalidation = todo_invalidations;
    assert(epd_resource_event(current, RESOURCE_INVALIDATE, 1U << RS_PART_TODO_ITEMS) == RS_OK);
    assert(todo_invalidations == before_todo_invalidation + 1U);
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_TODO_ITEMS) == RS_OK);
    assert(todo_reloads == before_todo_reload + 1U && resources_ready);
    todo_items_available = false;
    unsigned before_resource_reload = resource_reloads;
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_TODO_ITEMS) == RS_INVALID);
    assert(todo_reloads == before_todo_reload + 2U && resource_reloads == before_resource_reload);
    assert(!todo_items_ready());
    /* An optional damaged TODO package does not block ordinary modes. */
    current->config.display_mode = MODE_CLOCK;
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0U) == RS_OK);
    assert(todo_reloads == before_todo_reload + 2U);
    for (uint8_t mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; mode++) {
        current->config.display_mode = mode;
        todo_items_loaded = todo_items_available = false;
        unsigned reloads_before = todo_reloads;
        assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0U) == RS_INVALID);
        assert(todo_reloads == reloads_before + 1U);
        /* Normal activation recovers an already valid persisted TODO package. */
        todo_items_available = true;
        assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0U) == RS_OK);
        assert(todo_items_loaded && todo_reloads == reloads_before + 2U);
        assert(epd_resource_event(current, RESOURCE_ACTIVATE, 0U) == RS_OK);
        assert(todo_reloads == reloads_before + 2U); /* Successful view is cached. */
    }
    assert(todo_font_checks == 0U);
    todo_font_slot = 3U;
    todo_font_matches = false;
    unsigned before_requests = requests;
    before_resource_reload = resource_reloads;
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_FONTS) == RS_INVALID);
    assert(resource_reloads == before_resource_reload + 1U && !todo_items_ready());
    assert(requests == before_requests && todo_font_checks == 1U);
    todo_font_matches = true;
    assert(epd_resource_event(current, RESOURCE_ACTIVATE, 1U << RS_PART_FONTS) == RS_OK);
    assert(todo_items_ready() && requests == before_requests + 1U && todo_font_checks == 2U);
    /* Full-chip/table erasure invalidates the RAM list before activation can
     * fail on the missing config. Layout-only edits keep its metadata cached. */
    assert(epd_resource_event(current, RESOURCE_INVALIDATE, 1U << RS_PART_UI_RESOURCES) == RS_OK);
    assert(todo_items_ready());
    assert(epd_resource_event(current, RESOURCE_INVALIDATE, 1U << RS_PART_PARTITION_TABLE) == RS_OK);
    assert(!todo_items_ready() && !resources_ready);
    puts("PASS: real INIT/disconnect/timeout/refresh cleanup and optional GPIO handlers");
    return 0;
}
