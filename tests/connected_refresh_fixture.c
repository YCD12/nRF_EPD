/* Real mode/timer/display handlers, with scheduler and panel hardware shims. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_cmd_policy.h"
#include "epd_display_queue.h"
#include "epd_refresh_policy.h"
#include "epd_battery_policy.h"
#include "Lunar.h"

#define BLE_CONN_HANDLE_INVALID 65535U
#define EPD_DRIVER_IC_UC8179 1U
#define EPD_ANNOTATIONS_RX_TIMEOUT_S 10U
#define LOW_BATTERY_POLL_INTERVAL_S 10U
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
#define NRF_LOG_WARNING(...) ((void)0)
#define ble_log(...) ((void)0)
#define EPD_LED_OFF() ((void)0)
#define EPD_LED_BLINK() ((void)0)

typedef unsigned epd_model_id_t;
typedef struct epd_model epd_model_t;
typedef struct {
    unsigned ic;
    void (*sleep)(const epd_model_t *), (*init)(const epd_model_t *);
    void (*init_partial)(const epd_model_t *), (*diff_push)(void), (*refresh_partial)(void);
} driver_t;
struct epd_model { const driver_t *drv; uint16_t color, width, height; };
typedef struct {
    uint16_t conn_handle;
    epd_config_t config;
    epd_display_queue_t display_queue;
    struct { bool transfer_active; } transfer_ctx;
    const epd_model_t *epd;
    bool init_pending, init_ack_pending, disconnect_pending, gpio_connection_ref;
    bool is_refreshing, force_full_refresh, low_battery_screen_active;
    bool annotations_rx_abort_requested;
    uint32_t annotations_rx_last_activity, last_refresh_start, last_full_clear;
} ble_epd_t;
typedef struct { uint16_t voltage_mv; bool critical, skip; } battery_guard_result_t;

static unsigned refs, full_refreshes, partial_refreshes, saves;
static unsigned critical_depth;
static uint32_t now = 1001U;
static bool entered, resources_ready = true, clock_valid;
static gui_data_t displayed;
static app_sched_event_handler_t scheduled_worker;
static void *scheduled_context;
void test_critical_enter(void) { critical_depth++; }
void test_critical_exit(void) { assert(critical_depth); critical_depth--; }
uint32_t app_sched_event_put(void *data, uint16_t length, app_sched_event_handler_t worker)
{
    assert(!critical_depth && !scheduled_worker && length == sizeof(void *));
    memcpy(&scheduled_context, data, length);
    scheduled_worker = worker;
    return 0;
}
static void drain(void)
{
    unsigned steps = 0;
    while (scheduled_worker) {
        assert(++steps < 10);
        app_sched_event_handler_t worker = scheduled_worker;
        void *context = scheduled_context;
        scheduled_worker = NULL;
        worker(&context, sizeof(context));
    }
}
static void panel_step(const epd_model_t *p) { (void)p; assert(refs); }
static void partial_marker(void) {}
static const driver_t driver = {EPD_DRIVER_IC_UC8179, panel_step, panel_step,
                               panel_step, partial_marker, partial_marker};
static const epd_model_t panel = {&driver, 2, 800, 480};
static const epd_model_t *epd_get_model(epd_model_id_t id) { (void)id; return &panel; }
static void EPD_GPIO_Init(void) { refs++; }
static void EPD_GPIO_Uninit(void) { assert(refs); refs--; }
static void EPD_ClearBusyAbort(void) {}
static bool EPD_BusyAbortRequested(void) { return false; }
static void UC8179_ClearBusyTimeout(void) {}
static bool UC8179_HadBusyTimeout(void) { return false; }
static bool resource_protocol_entered(void) { return entered; }
bool epd_resources_ready(void) { return resources_ready; }
static void epd_clock_refresh_invalidate(void) { clock_valid = false; }
static bool epd_clock_refresh_is_valid(void) { return clock_valid; }
static void app_feed_wdt(void) {}
static void nrf_delay_ms(unsigned ms) { (void)ms; }
static void sleep_mode_enter(void) { assert(!"unexpected system sleep"); }
static uint32_t timestamp(void) { return now; }
static uint32_t app_uptime_seconds(void) { return now; }
static uint16_t EPD_ReadVoltageAndCache(void) { return 3300U; }
static bool calendar_annotations_rx_active(void) { return false; }
void epd_config_write(epd_config_t *cfg) { (void)cfg; saves++; }
void transformTime(uint32_t ts, tm_t *tm) { memset(tm, 0, sizeof(*tm)); tm->tm_hour = ts / 3600U % 24U; }
static battery_guard_result_t epd_battery_guard(ble_epd_t *p)
{ (void)p; return (battery_guard_result_t){3300, false, false}; }
static void epd_fill_device_name(gui_data_t *data) { strcpy(data->ssid, "test"); }
static bool epd_perform_full_refresh(ble_epd_t *p, const epd_model_t *epd, gui_data_t *data)
{
    assert(p->is_refreshing && refs && epd == &panel);
    displayed = *data;
    full_refreshes++;
    p->last_full_clear = data->timestamp;
    p->force_full_refresh = false;
    clock_valid = true;
    return true;
}
static bool epd_clock_refresh_try(const epd_model_t *epd, gui_data_t *data)
{
    assert(refs && epd == &panel);
    displayed = *data;
    partial_refreshes++;
    return true;
}
static void epd_gui_update(void *, uint16_t);
void ble_epd_on_timer(ble_epd_t *, uint32_t, bool);

/* PRODUCTION_HANDLERS */

static void select_mode(ble_epd_t *p, uint8_t mode)
{
    uint8_t command[] = {EPD_CMD_SET_MODE, mode};
    cmd_set_mode(p, command, sizeof(command));
    drain();
}
int main(void)
{
    ble_epd_t p = {.conn_handle = 1, .gpio_connection_ref = true};
    epd_display_queue_init(&p.display_queue);
    refs = 1; /* Connection owns one GPIO reference throughout these refreshes. */
    for (unsigned mode = MODE_CALENDAR; mode <= MODE_CLOCK; ++mode) {
        unsigned before = full_refreshes;
        select_mode(&p, mode);
        assert(full_refreshes == before + 1 && displayed.mode == mode);
        assert(displayed.timestamp == now && p.conn_handle == 1);
        assert(!p.is_refreshing && !p.display_queue.processing && refs == 1);
    }
    assert(saves == 3);
    /* Re-selecting a mode must refresh even without a config change. */
    unsigned before = full_refreshes + partial_refreshes;
    select_mode(&p, MODE_CLOCK);
    assert(full_refreshes + partial_refreshes == before + 1 && saves == 3);
    now = 1020;
    before = partial_refreshes;
    ble_epd_on_timer(&p, now, false);
    drain();
    assert(partial_refreshes == before + 1 && displayed.timestamp == now);

    before = full_refreshes + partial_refreshes;
    /* A refresh already queued before ENTER must not read resources during upload. */
    epd_display_request(&p, now);
    entered = true;
    drain();
    assert(full_refreshes + partial_refreshes == before && refs == 1);
    entered = false;
    resources_ready = false;
    epd_display_request(&p, now); drain();
    assert(full_refreshes + partial_refreshes == before + 1);
    before++;
    resources_ready = true;
    p.transfer_ctx.transfer_active = true;
    epd_display_request(&p, now); drain();
    assert(full_refreshes + partial_refreshes == before);
    p.transfer_ctx.transfer_active = false;
    p.init_pending = true;
    epd_display_request(&p, now); drain();
    assert(full_refreshes + partial_refreshes == before);
    p.init_pending = false;
    p.init_ack_pending = true;
    epd_display_request(&p, now); drain();
    assert(full_refreshes + partial_refreshes == before);
    p.init_ack_pending = false;
    p.disconnect_pending = true;
    epd_display_request(&p, now); drain();
    assert(full_refreshes + partial_refreshes == before);
    p.disconnect_pending = false;
    select_mode(&p, MODE_PICTURE);
    assert(full_refreshes + partial_refreshes == before); /* Direct picture RAM stays separate. */
    select_mode(&p, MODE_CALENDAR);
    assert(full_refreshes + partial_refreshes == before + 1 && refs == 1);
    /* Calendar also refreshes with no connection, using its own GPIO reference. */
    p.conn_handle = BLE_CONN_HANDLE_INVALID;
    p.gpio_connection_ref = false;
    refs = 0;
    select_mode(&p, MODE_CALENDAR);
    assert(full_refreshes + partial_refreshes == before + 2 && refs == 0);
    /* Boot takes the same forced-timer path, even with missing external data. */
    resources_ready = false;
    before = full_refreshes + partial_refreshes;
    ble_epd_on_timer(&p, now, true);
    drain();
    assert(full_refreshes + partial_refreshes == before + 1 && refs == 0);
    p.config.display_mode = MODE_PICTURE;
    ble_epd_on_timer(&p, now, true);
    drain();
    assert(full_refreshes + partial_refreshes == before + 1);
    puts("PASS: connected mode changes reach panel refresh, periodic ticks and exclusive resource/image access");
}
