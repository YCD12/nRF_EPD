/* Real mode/timer/display handlers, with scheduler and panel hardware shims. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_cmd_policy.h"
#include "epd_protocol.h"
#include "resource_protocol.h"
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
#define EPD_LED_BLINK() (led_blinks++)

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
    uint32_t connection_generation;
    epd_config_t config;
    epd_display_queue_t display_queue;
    struct { bool transfer_active; } transfer_ctx;
    const epd_model_t *epd;
    bool init_pending, init_ack_pending, disconnect_pending, gpio_connection_ref;
    bool is_refreshing, force_full_refresh, low_battery_screen_active;
    bool battery_poll_pending, battery_blink_pending;
    bool annotations_rx_abort_requested;
    bool mode_ack_pending;
    uint8_t initial_notify;
    uint8_t mode_ack[4];
    uint32_t annotations_rx_last_activity, last_refresh_start, last_full_clear;
} ble_epd_t;
typedef struct { uint16_t voltage_mv; bool critical, skip; } battery_guard_result_t;

static unsigned refs, full_refreshes, partial_refreshes, saves;
static unsigned critical_depth;
static unsigned adc_reads, led_blinks;
static uint16_t adc_voltage = 3300U;
static uint16_t refresh_voltage = 3300U;
static bool timer_active;
static uint32_t now = 1001U;
static bool entered, resources_ready = true, clock_valid, fail_full_refresh;
static bool resource_reply_pending;
static bool todo_loaded = true, todo_reload_succeeds = true;
static unsigned todo_reloads;
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
bool resource_protocol_entered(void) { return entered; }
bool resource_transfer_reply_pending(void) { return resource_reply_pending; }
bool epd_resources_ready(void) { return resources_ready; }
static void epd_clock_refresh_invalidate(void) { clock_valid = false; }
static bool epd_clock_refresh_is_valid(void) { return clock_valid; }
static void app_feed_wdt(void) {}
static void nrf_delay_ms(unsigned ms) { (void)ms; }
static void sleep_mode_enter(void) { assert(!"unexpected system sleep"); }
static uint32_t timestamp(void) { return now; }
static void set_timestamp(uint32_t value) { now = value; }
static uint32_t app_uptime_seconds(void) { return now; }
static uint16_t EPD_ReadVoltageAndCache(void)
{ assert(!timer_active); adc_reads++; return adc_voltage; }
static bool calendar_annotations_rx_active(void) { return false; }
static bool todo_items_ready(void) { return todo_loaded; }
static bool todo_items_init(void) { todo_reloads++; todo_loaded = todo_reload_succeeds; return todo_loaded; }
static bool todo_items_selection_changed(uint32_t ts) { (void)ts; return false; }
static uint8_t todo_font_slot = 1U;
static bool todo_font_matches = true;
static unsigned todo_font_checks;
static uint8_t todo_items_font_slot(void) { return todo_font_slot; }
static uint8_t todo_items_font_size(void) { return 12U; }
static uint32_t todo_items_font_key(void) { return 123U; }
static void todo_items_suspend_display(void) { todo_loaded = false; }
static bool GUI_TodoFontMatches(uint8_t size, uint32_t key)
{ assert(size == 12U && key == 123U); todo_font_checks++; return todo_font_matches; }
void epd_config_write(epd_config_t *cfg) { (void)cfg; saves++; }
bool epd_config_read_pending(void) { return false; }
void transformTime(uint32_t ts, tm_t *tm) { memset(tm, 0, sizeof(*tm)); tm->tm_hour = ts / 3600U % 24U; }
static battery_guard_result_t epd_battery_guard(ble_epd_t *p)
{
    p->low_battery_screen_active = epd_battery_is_low(refresh_voltage);
    return (battery_guard_result_t){refresh_voltage, false, false};
}
static void epd_fill_device_name(gui_data_t *data) { strcpy(data->ssid, "test"); }
static bool epd_perform_full_refresh(ble_epd_t *p, const epd_model_t *epd, gui_data_t *data)
{
    assert(p->is_refreshing && refs && epd == &panel);
    displayed = *data;
    full_refreshes++;
    if (fail_full_refresh) {
        p->force_full_refresh = true;
        return false;
    }
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
    uint8_t command[] = {EPD_CMD_SET_MODE, mode, 1, p->config.week_start,
        (uint8_t)(now >> 24), (uint8_t)(now >> 16),
        (uint8_t)(now >> 8), (uint8_t)now, 0};
    cmd_set_mode(p, command, sizeof(command));
    /* Main sends the reliable control reply before dispatching the panel. */
    p->mode_ack_pending = false;
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
    uint8_t atomic_mode[] = {EPD_CMD_SET_MODE, MODE_CLOCK, 37, 2, 0, 0, 7, 208, 0};
    cmd_set_mode(&p, atomic_mode, sizeof(atomic_mode));
    assert(now == 2000U && p.config.week_start == 2U && saves == 4U);
    assert(p.mode_ack_pending && p.mode_ack[1] == 37U && p.mode_ack[2] == RS_OK);
    p.mode_ack_pending = false;
    drain();
    uint8_t obsolete_mode[] = {EPD_CMD_SET_MODE, MODE_CALENDAR};
    cmd_set_mode(&p, obsolete_mode, sizeof(obsolete_mode));
    assert(p.config.display_mode == MODE_CLOCK && now == 2000U);
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
    resources_ready = true;
    for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; mode++) {
        before = full_refreshes;
        unsigned partial_before = partial_refreshes;
        select_mode(&p, mode);
        assert(p.config.display_mode == mode && displayed.mode == mode);
        assert(full_refreshes == before + 1U && partial_refreshes == partial_before && refs == 0U);
        now = 1080U;
        ble_epd_on_timer(&p, now, false);
        drain();
        assert(full_refreshes == before + 1U && partial_refreshes == partial_before);
        assert(!p.force_full_refresh); /* Successful TODO sleep keeps its screen valid. */
        for (unsigned i = 0U; i < 3U; i++) {
            now++;
            ble_epd_on_timer(&p, now, false);
            drain();
            assert(full_refreshes == before + 1U && !p.force_full_refresh);
        }
        select_mode(&p, MODE_CALENDAR);
        fail_full_refresh = true;
        select_mode(&p, mode); /* Same tasks, but a different screen is required. */
        unsigned failed_attempts = full_refreshes;
        assert(p.force_full_refresh && p.epd == NULL && refs == 0U);
        fail_full_refresh = false;
        now++;
        ble_epd_on_timer(&p, now, false);
        drain();
        assert(full_refreshes == failed_attempts + 1U && !p.force_full_refresh && refs == 0U);
        now++;
        ble_epd_on_timer(&p, now, false);
        drain();
        assert(full_refreshes == failed_attempts + 1U && partial_refreshes == partial_before);
        /* A transient optional-package failure remains recoverable from either mode. */
        select_mode(&p, MODE_CLOCK);
        todo_loaded = false;
        todo_reload_succeeds = false;
        unsigned reloads_before = todo_reloads;
        unsigned before_recovery = full_refreshes + partial_refreshes;
        select_mode(&p, mode);
        assert(todo_reloads == reloads_before + 1U && p.config.display_mode == MODE_CLOCK);
        assert(p.mode_ack[2] == RS_INVALID && full_refreshes + partial_refreshes == before_recovery);
        todo_reload_succeeds = true;
        select_mode(&p, mode);
        assert(todo_reloads == reloads_before + 2U && todo_loaded && p.config.display_mode == mode);
        assert(p.mode_ack[2] == RS_OK && full_refreshes + partial_refreshes == before_recovery + 1U);
        select_mode(&p, mode);
        assert(todo_reloads == reloads_before + 2U); /* Cached view avoids further Flash scans. */
    }
    /* Switching directly between the two layouts redraws without reloading TODO. */
    before = full_refreshes;
    unsigned reloads_before = todo_reloads;
    select_mode(&p, MODE_TODO_CALENDAR);
    select_mode(&p, MODE_TODO);
    assert(full_refreshes == before + 2U && todo_reloads == reloads_before);
    assert(todo_font_checks == 0U); /* Base fonts do not touch the optional slot. */
    todo_font_slot = 3U;
    todo_font_matches = false;
    for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; mode++) {
        select_mode(&p, MODE_CLOCK);
        before = full_refreshes + partial_refreshes;
        select_mode(&p, mode);
        assert(p.mode_ack[2] == RS_INVALID && p.config.display_mode == MODE_CLOCK);
        assert(!todo_loaded && full_refreshes + partial_refreshes == before);
    }
    todo_font_matches = true;
    select_mode(&p, MODE_TODO);
    assert(todo_loaded && p.config.display_mode == MODE_TODO && p.mode_ack[2] == RS_OK);
    assert(todo_font_checks == 3U);
    /* Erased/invalid resources must stay idle across minute/day boundaries,
     * including when the last explicit fallback failed and old TODO metadata
     * is still cached. Connecting does not install resources. */
    for (unsigned connected = 0U; connected < 2U; ++connected) {
        p.conn_handle = connected ? 1U : BLE_CONN_HANDLE_INVALID;
        for (unsigned mode = MODE_CALENDAR; mode <= MODE_TODO; ++mode) {
            p.config.display_mode = mode;
            resources_ready = false;
            fail_full_refresh = true;
            epd_clock_refresh_invalidate();
            now = (now / 86400U + 1U) * 86400U - 60U;
            ble_epd_on_timer(&p, now, true); drain();
            unsigned attempts = full_refreshes + partial_refreshes;
            for (unsigned i = 0U; i < 180U; ++i) {
                now++;
                ble_epd_on_timer(&p, now, false); drain();
                assert(full_refreshes + partial_refreshes == attempts);
                assert(!p.display_queue.pending && !scheduled_worker && refs == 0U);
            }
            /* A successful resource activation still redraws immediately. */
            resources_ready = true;
            fail_full_refresh = false;
            ble_epd_on_timer(&p, now, true); drain();
            assert(full_refreshes + partial_refreshes == attempts + 1U);
        }
    }
    /* A midnight failure is outstanding work, even after its rollover tick
     * has been consumed. Healthy calendar sleep must not cause more redraws. */
    for (unsigned mode = MODE_CALENDAR; mode <= MODE_CLOCK; ++mode) {
        p.config.display_mode = mode;
        p.conn_handle = BLE_CONN_HANDLE_INVALID;
        p.gpio_connection_ref = false;
        now = (now / 86400U + 1U) * 86400U - 1U;
        ble_epd_on_timer(&p, now, true); drain();
        uint32_t previous_frame = p.last_full_clear;
        fail_full_refresh = true;
        now++;
        ble_epd_on_timer(&p, now, false); drain();
        unsigned attempts = full_refreshes;
        assert(p.last_full_clear == previous_frame && s_display_retry_step == 1U);
        now++;
        ble_epd_on_timer(&p, now, false); drain();
        assert(full_refreshes == attempts + 1U && s_display_retry_step == 2U);
        fail_full_refresh = false;
        for (unsigned i = 0U; i < 4U; i++) {
            now++; ble_epd_on_timer(&p, now, false); drain();
            assert(full_refreshes == attempts + 1U);
        }
        now++; ble_epd_on_timer(&p, now, false); drain();
        assert(full_refreshes == attempts + 2U && !s_display_retry_step);
        assert(p.last_full_clear == now && refs == 0U);
        for (unsigned i = 0U; i < 40U; i++) {
            now++; ble_epd_on_timer(&p, now, false); drain();
            assert(full_refreshes == attempts + 2U);
        }
    }
    /* Low voltage holds a successfully displayed frame, but must not consume
     * a failed last-frame request. Retry with backoff, then stop across minute
     * boundaries once one full frame succeeds at the same low voltage. */
    refresh_voltage = 2750U;
    for (unsigned mode = MODE_CALENDAR; mode <= MODE_TODO; ++mode) {
        p.config.display_mode = mode;
        p.force_full_refresh = true;
        now = (now / 86400U + 1U) * 86400U + 60U;
        fail_full_refresh = true;
        ble_epd_on_timer(&p, now, true); drain();
        unsigned attempts = full_refreshes;
        assert(p.low_battery_screen_active && s_display_retry_step == 1U);
        now++; ble_epd_on_timer(&p, now, false); drain();
        assert(full_refreshes == attempts + 1U && s_display_retry_step == 2U);
        fail_full_refresh = false;
        for (unsigned i = 0U; i < 4U; ++i) {
            now++; ble_epd_on_timer(&p, now, false); drain();
            assert(full_refreshes == attempts + 1U);
        }
        now++; ble_epd_on_timer(&p, now, false); drain();
        assert(full_refreshes == attempts + 2U && s_display_retry_step == 0U);
        assert(p.last_full_clear == now && p.low_battery_screen_active && refs == 0U);
        for (unsigned i = 0U; i < 130U; ++i) {
            now++; ble_epd_on_timer(&p, now, false); drain();
            assert(full_refreshes == attempts + 2U);
        }
    }
    refresh_voltage = 3300U;
    p.battery_poll_pending = p.battery_blink_pending = false;
    /* RTC only posts battery work. Main retries it after exclusive access;
     * an unavailable ADC does not manufacture a critical-battery update. */
    p.config.display_mode = MODE_CALENDAR;
    p.low_battery_screen_active = true;
    adc_voltage = 2800U;
    now = (now / 30U + 1U) * 30U;
    timer_active = true;
    ble_epd_on_timer(&p, now, false);
    timer_active = false;
    assert(!adc_reads && !led_blinks && p.battery_poll_pending && p.battery_blink_pending);
    entered = true;
    epd_battery_process(&p);
    assert(!adc_reads && p.battery_poll_pending);
    entered = false;
    epd_battery_process(&p);
    assert(adc_reads == 1U && led_blinks == 1U && !p.battery_poll_pending);
    assert(p.low_battery_screen_active && !p.display_queue.pending);
    adc_voltage = 0U;
    p.battery_poll_pending = true;
    epd_battery_process(&p);
    assert(adc_reads == 2U && p.low_battery_screen_active && !p.display_queue.pending);
    adc_voltage = 3300U;
    p.battery_poll_pending = true;
    epd_battery_process(&p);
    assert(adc_reads == 3U && !p.low_battery_screen_active && p.display_queue.pending);
    drain();
    p.low_battery_screen_active = true;
    adc_voltage = 2600U;
    p.battery_poll_pending = true;
    epd_battery_process(&p);
    assert(adc_reads == 4U && p.low_battery_screen_active && p.display_queue.pending);
    drain();
    /* An accepted same-day calendar request survives transient main-loop
     * blockers. Draining the scheduler while blocked must neither consume it
     * nor reschedule itself forever; clearing the blocker applies it once. */
    p.low_battery_screen_active = false;
    p.config.display_mode = MODE_CALENDAR;
    p.force_full_refresh = true;
    bool *blockers[] = {&entered, &p.init_pending, &p.init_ack_pending,
                       &p.disconnect_pending, &p.mode_ack_pending,
                       &resource_reply_pending};
    for (unsigned blocker = 0U; blocker < sizeof(blockers) / sizeof(blockers[0]); ++blocker) {
        unsigned attempts = full_refreshes;
        epd_display_request(&p, now);
        *blockers[blocker] = true;
        drain();
        assert(p.display_queue.pending && !p.display_queue.processing);
        assert(!scheduled_worker && full_refreshes == attempts);
        for (unsigned turn = 0U; turn < 3U; ++turn) {
            epd_display_queue_schedule(&p.display_queue, &p, epd_gui_update);
            drain();
            assert(p.display_queue.pending && !scheduled_worker && full_refreshes == attempts);
        }
        *blockers[blocker] = false;
        epd_display_queue_schedule(&p.display_queue, &p, epd_gui_update);
        drain();
        assert(full_refreshes == attempts + 1U && !p.display_queue.pending);
        epd_display_queue_schedule(&p.display_queue, &p, epd_gui_update);
        drain();
        assert(full_refreshes == attempts + 1U && !scheduled_worker && refs == 0U);
    }
    puts("PASS: deferred calendar requests survive INIT/resource/disconnect blockers and resume once");
    puts("PASS: all display modes retry failed frames with backoff; ADC/LED polling runs only in main");
    puts("PASS: low-voltage failed last frames retry with backoff and stop after one successful frame");
    puts("PASS: missing resources stay idle while connected/disconnected across minutes and midnight; reinstall resumes refresh");
    puts("PASS: connected mode changes reach panel refresh, periodic ticks and exclusive resource/image access");
}
