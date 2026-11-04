/* Real worker, refresh, timer, mode/activation handlers and resource transport.
 * Only panel hardware, resource storage and BLE HVX are replaced. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_cmd_policy.h"
#include "epd_protocol.h"
#include "epd_display_queue.h"
#include "epd_refresh_policy.h"
#include "epd_battery_policy.h"
#include "epd_offline.h"
#include "resource_protocol.h"
#include "resource_transfer.h"
#include "resource_layout.h"
#include "todo_items.h"
#include "Lunar.h"
#include "sdk_errors.h"

#define BLE_CONN_HANDLE_INVALID UINT16_MAX
#define EPD_DRIVER_IC_UC8179 1U
#define EPD_UC8179_750_BWR 7U
#define EPD_ANNOTATIONS_RX_TIMEOUT_S 10U
#define LOW_BATTERY_POLL_INTERVAL_S 10U
#define NRF_ERROR_RESOURCES 19U
#define NRF_LOG_INFO(...) test_log(__VA_ARGS__)
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
    buffer_callback write_image;
    void (*refresh)(const epd_model_t *);
    int8_t (*read_temp)(const epd_model_t *);
} driver_t;
struct epd_model { const driver_t *drv; uint16_t color, width, height; };
typedef struct {
    uint16_t conn_handle, max_data_len;
    uint32_t connection_generation;
    epd_config_t config;
    epd_display_queue_t display_queue;
    epd_offline_t offline;
    struct { bool transfer_active; } transfer_ctx;
    const epd_model_t *epd;
    bool init_pending, init_ack_pending, disconnect_pending, gpio_connection_ref;
    bool is_refreshing, force_full_refresh, low_battery_screen_active;
    bool battery_poll_pending, battery_blink_pending;
    bool annotations_rx_abort_requested, is_notification_enabled;
    bool mode_ack_pending;
    uint8_t mode_ack[4], initial_notify;
    uint32_t annotations_rx_last_activity, last_refresh_start, last_full_clear;
} ble_epd_t;
typedef struct { uint16_t voltage_mv; bool critical, skip; } battery_guard_result_t;

static ble_epd_t *current;
static unsigned refs, full_refreshes, resource_acks, mode_acks, resource_applies, draw_attempts;
static unsigned critical_depth, scheduled_steps;
static uint32_t now = 1001U;
static uint32_t uptime = 1001U;
static uint16_t changed;
static bool entered, notifications_blocked, timer_active, render_valid, clock_valid;
static bool fail_panel, panel_timeout;
static bool fail_font_frame;
static todo_selection_t rendered_selection;
static uint32_t rendered_timestamp;
static bool custom_font_matches = true;
static unsigned custom_font_checks;
static uint8_t last_resource_result;
static bool resources_ready = true, flash_erased;
static uint8_t payload[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE + TODO_ITEMS_TEXT_MAX + 5U];
static uint32_t payload_length = TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE;
static app_sched_event_handler_t scheduled_worker;
static void *scheduled_context;
static void epd_gui_update(void *, uint16_t);
void ble_epd_on_timer(ble_epd_t *, uint32_t, bool);
static rs_result_t epd_resource_event(void *, resource_event_t, uint16_t);
static void test_log(const char *format, ...) { (void)format; }

void test_critical_enter(void) { critical_depth++; }
void test_critical_exit(void) { assert(critical_depth); critical_depth--; }
uint32_t app_sched_event_put(void *data, uint16_t length, app_sched_event_handler_t worker)
{
    assert(!critical_depth && !scheduled_worker && length == sizeof(void *));
    memcpy(&scheduled_context, data, length);
    scheduled_worker = worker;
    return NRF_SUCCESS;
}
static void drain(void)
{
    unsigned steps = 0U;
    while (scheduled_worker) {
        assert(++steps < 10U);
        app_sched_event_handler_t worker = scheduled_worker;
        void *context = scheduled_context;
        scheduled_worker = NULL;
        scheduled_steps++;
        worker(&context, sizeof(context));
    }
}
static void tick(void)
{
    now++;
    uptime++;
    timer_active = true;
    ble_epd_on_timer(current, now, false);
    timer_active = false;
}
static void put32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0U; i < 4U; i++) p[i] = (uint8_t)(value >> (8U * i)); }
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    assert(!timer_active && id == RS_PART_TODO_ITEMS);
    if (flash_erased) return RS_NOT_FOUND;
    *length = payload_length;
    return RS_OK;
}
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    assert(!timer_active && id == RS_PART_TODO_ITEMS);
    assert(offset <= payload_length && length <= payload_length - offset);
    memcpy(data, payload + offset, length);
    return RS_OK;
}
static void panel_init(const epd_model_t *p) { (void)p; assert(refs); tick(); }
static void panel_sleep(const epd_model_t *p) { (void)p; assert(refs); tick(); }
static void panel_refresh(const epd_model_t *p)
{ (void)p; assert(refs); full_refreshes++; panel_timeout=fail_panel; for (unsigned i = 0; i < 15U; i++) tick(); }
static void panel_write(void *ctx, uint8_t *black, uint8_t *color, uint16_t x,
                        uint16_t y, uint16_t width, uint16_t height)
{ (void)ctx;(void)black;(void)color;(void)x;(void)y;(void)width;(void)height; }
static const driver_t driver = {.ic=EPD_DRIVER_IC_UC8179, .sleep=panel_sleep,
    .init=panel_init, .write_image=panel_write, .refresh=panel_refresh};
static const epd_model_t panel = {&driver, 2U, 800U, 480U};
static const epd_model_t *epd_get_model(epd_model_id_t id) { (void)id; tick(); return &panel; }
static void EPD_GPIO_Init(void) { refs++; }
static void EPD_GPIO_Uninit(void) { assert(refs); refs--; }
static void EPD_GPIO_Load(epd_config_t *cfg) { (void)cfg; }
static void EPD_ClearBusyAbort(void) {}
static bool EPD_BusyAbortRequested(void) { return false; }
static void UC8179_ClearBusyTimeout(void) { panel_timeout=false; }
static bool UC8179_HadBusyTimeout(void) { return panel_timeout; }
bool resource_protocol_entered(void) { return entered; }
void resource_protocol_reset(void) { entered = false; }
uint8_t resource_protocol_execute(const uint8_t *request, uint8_t length, uint8_t *reply)
{
    assert(length == 3U && request[1] == 7U);
    entered = true;
    rs_result_t result = epd_resource_event(current, RESOURCE_ACTIVATE, changed);
    entered = false;
    memcpy(reply, (uint8_t[]){EPD_RSP_RESOURCE, 7U, request[2], result}, 4U);
    return 4U;
}
static uint32_t resource_notification_send(void *context, uint32_t generation,
                                           uint8_t *data, uint16_t length)
{
    assert(context == current && generation == current->connection_generation);
    assert(length == 4U && !current->is_refreshing);
    if (notifications_blocked) return NRF_ERROR_RESOURCES;
    if (data[0] == EPD_RSP_RESOURCE) { resource_acks++; last_resource_result = data[3]; }
    else { assert(data[0] == EPD_RSP_MODE); mode_acks++; }
    return NRF_SUCCESS;
}
static bool epd_resources_ready(void) { return resources_ready; }
static rs_result_t epd_resources_apply(void) { resource_applies++; resources_ready = !flash_erased; return flash_erased ? RS_NOT_FOUND : RS_OK; }
static void epd_resources_invalidate_partitions(uint16_t mask) { (void)mask; resources_ready = false; }
static bool epd_resource_idle(const ble_epd_t *p) { (void)p; return true; }
static bool nrf_gpio_pin_present_check(unsigned pin) { return pin < 32U; }
static void epd_clock_refresh_invalidate(void) { clock_valid = false; }
static bool epd_clock_refresh_is_valid(void) { return clock_valid; }
static void epd_clock_refresh_mark_valid(uint32_t ts) { (void)ts; clock_valid = true; }
static bool epd_clock_refresh_try(const epd_model_t *epd, gui_data_t *data,
                                  bool prepare_only)
{ (void)epd;(void)data;(void)prepare_only;assert(!"TODO must never use clock partial refresh");return false; }
static void app_feed_wdt(void) {}
static void nrf_delay_ms(unsigned ms) { (void)ms; tick(); }
static void sleep_mode_enter(void) { assert(!"unexpected system sleep"); }
static uint32_t timestamp(void) { return now; }
static void set_timestamp(uint32_t value) { now = value; }
static uint32_t app_uptime_seconds(void) { return uptime; }
static uint32_t app_timer_cnt_get(void) { return now; }
static uint32_t epd_elapsed_ms(uint32_t start) { return (now - start) * 1000U; }
static uint16_t EPD_ReadVoltageAndCache(void) { return 3300U; }
static bool calendar_annotations_rx_active(void) { return false; }
static void calendar_annotations_store_init(void) {}
static bool calendar_annotations_store_valid(void) { return true; }
static bool GUI_TodoFontMatches(uint8_t size, uint32_t key)
{
    assert(!timer_active && size == 12U && key == 123U);
    custom_font_checks++;
    return custom_font_matches;
}
void epd_config_write(epd_config_t *cfg) { (void)cfg; }
bool epd_config_read_pending(void) { return false; }
void epd_config_read(epd_config_t *cfg) { if (flash_erased) memset(cfg, 255, sizeof(*cfg)); else *cfg = current->config; }
void transformTime(uint32_t ts, tm_t *tm) { memset(tm, 0, sizeof(*tm)); tm->tm_hour = ts / 3600U % 24U; }
static battery_guard_result_t epd_battery_guard(ble_epd_t *p)
{ (void)p; return (battery_guard_result_t){3300U, false, false}; }
static void epd_fill_device_name(gui_data_t *data) { strcpy(data->ssid, "test"); }
void DrawGUI(gui_data_t *data, buffer_callback callback, void *ctx)
{
    draw_attempts++;
    rendered_timestamp = data->timestamp;
    assert(epd_is_todo_mode(data->mode) && callback == panel_write && ctx == &panel);
    if (!resources_ready) { render_valid = true; tick(); return; } /* NO DATA */
    todo_selection_t selection;
    todo_item_t item;
    render_valid = todo_items_select(data->timestamp, &selection);
    rendered_selection = selection;
    for (unsigned i = 0U; i < selection.count; i++) {
        if (!todo_items_read(selection.index[i], &item)) render_valid = false;
    }
    if (fail_font_frame) render_valid = false;
    tick();
}
bool GUI_RenderValid(void) { return render_valid; }

/* PRODUCTION_HANDLERS */

static void main_turn(void)
{
    resource_transfer_process(current, current->connection_generation, true, true,
                              now, resource_notification_send);
    epd_control_notifications_process(current);
    epd_display_queue_schedule(&current->display_queue, current, epd_gui_update);
    drain();
}
static void activate(uint16_t mask, uint8_t token)
{
    changed = mask;
    uint8_t request[] = {EPD_CMD_RESOURCE, 7U, token};
    resource_transfer_receive(request, sizeof(request), current->connection_generation);
}
int main(int argc, char **argv)
{
    assert(argc == 2 && strlen(argv[1]) == 1U);
    display_mode_t mode = (display_mode_t)(argv[1][0] - '0');
    assert(epd_is_todo_mode(mode));
    ble_epd_t p = {.conn_handle=1U, .connection_generation=1U,
        .is_notification_enabled=true, .max_data_len=244U,
        .config={.model_id=7U, .display_mode=mode}};
    current = &p;
    put32(payload, TODO_ITEMS_MAGIC);
    payload[4]=1U; payload[5]=80U; payload[6]=1U; payload[8]=5U; payload[9]=12U;
    put32(payload+16U, 1U); payload[28]=TODO_ITEM_RESIDENT; payload[29]=3U;
    memcpy(payload+30U, "pin", 3U);
    assert(todo_items_init());
    epd_display_queue_init(&p.display_queue);

    /* A font LEAVE queues a redraw, but HVX backpressure must leave both reply
     * and GUI request pending. No blocking panel update may start meanwhile. */
    notifications_blocked = true;
    activate(1U << RS_PART_FONTS, 41U);
    main_turn();
    assert(resource_transfer_reply_pending() && p.display_queue.pending);
    assert(!full_refreshes && resource_applies == 1U && !scheduled_worker);
    main_turn();
    assert(!full_refreshes && !resource_acks && !scheduled_worker);
    notifications_blocked = false;
    main_turn();
    assert(resource_acks == 1U && full_refreshes == 1U && !p.force_full_refresh);
    assert(p.epd == NULL && !refs && !p.display_queue.pending);
    for (unsigned i = 0U; i < 30U; i++) { tick(); main_turn(); }
    assert(full_refreshes == 1U); /* Model recreation/sleep never leaves a retry flag. */

    activate(1U << RS_PART_TODO_ITEMS, 42U);
    main_turn();
    assert(resource_acks == 2U && full_refreshes == 2U && !p.force_full_refresh);

    /* SET_MODE arrives through the scheduler. Its reliable reply remains
     * pending until the next main turn; GUI must yield within this drain. */
    uint8_t command[] = {EPD_CMD_SET_MODE, mode, 43U, 0U,
        (uint8_t)(now >> 24), (uint8_t)(now >> 16), (uint8_t)(now >> 8), (uint8_t)now, 0U};
    notifications_blocked = true;
    cmd_set_mode(&p, command, sizeof(command));
    assert(p.mode_ack_pending);
    drain();
    assert(p.display_queue.pending && full_refreshes == 2U && !scheduled_worker);
    main_turn();
    assert(p.mode_ack_pending && full_refreshes == 2U && !mode_acks);
    notifications_blocked = false;
    main_turn();
    assert(!p.mode_ack_pending && mode_acks == 1U && full_refreshes == 3U);
    for (unsigned i = 0U; i < 90U; i++) { tick(); main_turn(); }
    assert(full_refreshes == 3U && !p.force_full_refresh && !scheduled_worker);
    assert(!p.display_queue.pending && !p.is_refreshing && p.epd == NULL && !refs);
    assert(scheduled_steps < 12U);
    assert(custom_font_checks == 0U);

    /* A persisted custom TODO must be checked after applying the font pack.
     * Missing/wrong slot 3 is a dependency error, not a one-second draw loop. */
    payload[4] = TODO_ITEMS_CUSTOM_VERSION; payload[10] = 3U; payload[11] = 3U;
    put32(payload + 12U, 123U);
    memcpy(payload + TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE, "Foo", 3U);
    payload_length += 3U;
    custom_font_matches = false;
    activate((1U << RS_PART_FONTS) | (1U << RS_PART_TODO_ITEMS), 44U);
    main_turn();
    assert(last_resource_result == RS_INVALID && !todo_items_ready() && full_refreshes == 3U);
    for (unsigned i = 0U; i < 90U; i++) { tick(); main_turn(); }
    assert(full_refreshes == 3U && !p.display_queue.pending);
    command[2] = 45U;
    cmd_set_mode(&p, command, sizeof(command));
    assert(p.mode_ack_pending && p.mode_ack[2] == RS_INVALID && !todo_items_ready());
    main_turn();
    assert(mode_acks == 2U && full_refreshes == 3U);
    custom_font_matches = true;
    activate(1U << RS_PART_FONTS, 46U);
    main_turn();
    assert(last_resource_result == RS_OK && todo_items_ready() && full_refreshes == 4U);
    for (unsigned i = 0U; i < 90U; i++) { tick(); main_turn(); }
    assert(full_refreshes == 4U && !p.force_full_refresh && custom_font_checks == 3U);

    /* V4 scheduling uses actual sleeping GUI workers. The trigger and deadline
     * change only the selected record's phase; IDs/counts stay unchanged. */
    memset(payload,0,sizeof(payload));
    put32(payload,TODO_ITEMS_MAGIC);
    payload[4]=TODO_ITEMS_TIMING_VERSION; payload[5]=80U; payload[6]=1U;
    payload[8]=5U; payload[9]=12U;
    put32(payload+16U,9U); put32(payload+20U,2000U); put32(payload+24U,2200U);
    payload[29]=5U; memcpy(payload+30U,"timed",5U);
    payload_length=TODO_ITEMS_HEADER_SIZE+TODO_ITEMS_RECORD_SIZE+TODO_ITEMS_TIMING_TAIL_SIZE;
    payload[payload_length-4U]=1U; payload[payload_length-2U]=1U;
    now=1800U;
    activate(1U << RS_PART_TODO_ITEMS,47U);
    main_turn();
    assert(resource_acks==5U && full_refreshes==5U && rendered_selection.count==0U);
    assert(todo_items_lead_minutes()==1U && todo_items_retain_minutes()==1U);
    now=1938U; tick(); main_turn(); assert(full_refreshes==5U);
    tick(); main_turn();
    assert(full_refreshes==6U && rendered_selection.count==1U && rendered_selection.id[0]==9U &&
           rendered_selection.phase[0]==TODO_PHASE_UPCOMING);
    for (unsigned i=0U; i<20U; i++) { tick(); main_turn(); }
    assert(full_refreshes==6U && !p.force_full_refresh && p.epd==NULL && !refs);
    now=1999U; fail_panel=true; tick(); main_turn();
    assert(full_refreshes==7U && p.force_full_refresh && todo_items_selection_changed(now));
    fail_panel=false; tick(); main_turn();
    assert(full_refreshes==8U && rendered_selection.count==1U && rendered_selection.id[0]==9U &&
           rendered_selection.phase[0]==TODO_PHASE_ACTIVE && !p.force_full_refresh);
    for (unsigned i=0U; i<30U; i++) { tick(); main_turn(); }
    assert(full_refreshes==8U && !p.display_queue.pending);
    now=2199U; tick(); main_turn();
    assert(full_refreshes==9U && rendered_selection.count==1U && rendered_selection.id[0]==9U &&
           rendered_selection.phase[0]==TODO_PHASE_EXPIRED);
    for (unsigned i=0U; i<20U; i++) { tick(); main_turn(); }
    assert(full_refreshes==9U && !p.force_full_refresh);
    now=2259U; tick(); main_turn();
    assert(full_refreshes==10U && rendered_selection.count==0U && rendered_selection.daily_count==1U);
    for (unsigned i=0U; i<90U; i++) { tick(); main_turn(); }
    assert(full_refreshes==10U && !p.force_full_refresh && !p.display_queue.pending && !scheduled_worker);
    assert(p.epd==NULL && !refs && custom_font_checks==3U);
    /* The real GUI latches font I/O faults, even when the final glyph succeeds.
     * Its invalid-frame result must prevent the panel update and retry without
     * requiring a task-selection change or a second explicit command. */
    fail_font_frame = true;
    ble_epd_on_timer(&p, now, true); main_turn();
    assert(full_refreshes==10U && p.force_full_refresh && !todo_items_selection_changed(now));
    fail_font_frame = false;
    tick(); main_turn();
    assert(full_refreshes==11U && !p.force_full_refresh && !todo_items_selection_changed(now));
    for (unsigned i=0U; i<30U; i++) { tick(); main_turn(); }
    assert(full_refreshes==11U && !p.display_queue.pending && !scheduled_worker && !refs);

    unsigned before_draw=draw_attempts, before_panel=full_refreshes;
    fail_font_frame=true;
    ble_epd_on_timer(&p,now,true); main_turn();
    for(unsigned i=0U;i<30U;i++) {tick();main_turn();}
    assert(draw_attempts-before_draw <= 6U && draw_attempts-before_draw >= 3U);
    assert(full_refreshes==before_panel && p.force_full_refresh && s_display_retry_step==4U);
    printf("persistent I/O: %u failed draws across 30 timer/main turns, zero panel refreshes\n",draw_attempts-before_draw);
    /* A new font activation bypasses the pending retry deadline and resets it. */
    fail_font_frame=false;
    activate(1U<<RS_PART_FONTS,48U); main_turn();
    assert(full_refreshes==before_panel+1U && !s_display_retry_step && !p.force_full_refresh);

    /* Backoff follows uptime, not a small backward wall-clock correction. */
    p.force_full_refresh=true;
    for(unsigned i=0U;i<4U;i++) epd_display_retry_failed();
    uint32_t deadline=s_display_retry_at;
    now-=30U; ble_epd_on_timer(&p,now,false); main_turn();
    assert(full_refreshes==before_panel+1U && s_display_retry_step==4U);
    uptime=deadline; tick(); main_turn();
    assert(full_refreshes==before_panel+2U && !s_display_retry_step);

    /* Explicit requests and day rollover start immediately despite backoff. */
    p.force_full_refresh=true;
    for(unsigned i=0U;i<4U;i++) epd_display_retry_failed();
    ble_epd_on_timer(&p,now,true); main_turn();
    assert(full_refreshes==before_panel+3U && !s_display_retry_step);
    p.force_full_refresh=true;
    for(unsigned i=0U;i<4U;i++) epd_display_retry_failed();
    now+=86400U; ble_epd_on_timer(&p,now,false); main_turn();
    assert(full_refreshes==before_panel+4U && !s_display_retry_step);

    /* Picture mode has no redrawable frame and clears pending retries. */
    for(unsigned i=0U;i<4U;i++) epd_display_retry_failed();
    p.config.display_mode=MODE_PICTURE; ble_epd_on_timer(&p,now,false);
    assert(!s_display_retry_step && !p.display_queue.pending);
    p.config.display_mode=mode;

    /* Uptime wrapping and disconnected operation still reach the retry. */
    uptime=UINT32_MAX-2U;
    p.force_full_refresh=true;
    for(unsigned i=0U;i<4U;i++) epd_display_retry_failed();
    assert(s_display_retry_at==27U);
    ble_epd_on_timer(&p,now,false); assert(!p.display_queue.pending);
    uptime=26U; ble_epd_on_timer(&p,now,false); assert(!p.display_queue.pending);
    p.conn_handle=BLE_CONN_HANDLE_INVALID;
    uptime=27U; ble_epd_on_timer(&p,now,false); main_turn();
    assert(full_refreshes==before_panel+5U && !s_display_retry_step && !p.force_full_refresh && !refs);

    /* The queued wall clock must not survive a long ACK/maintenance hold.
     * Keep a resident-only list so its selection and daily total stay equal
     * across midnight; the right calendar still has to move to the new day. */
    payload[28]=TODO_ITEM_RESIDENT;
    put32(payload+20U,0U); put32(payload+24U,0U);
    assert(todo_items_init());
    p.conn_handle=1U;
    now=1791503999U;
    command[4]=(uint8_t)(now>>24); command[5]=(uint8_t)(now>>16);
    command[6]=(uint8_t)(now>>8); command[7]=(uint8_t)now;
    notifications_blocked=true;
    cmd_set_mode(&p,command,sizeof(command)); main_turn();
    assert(p.mode_ack_pending && p.display_queue.pending && !p.is_refreshing);
    unsigned before_midnight=full_refreshes;
    entered=true; now=1791504006U; tick();
    entered=false; notifications_blocked=false; main_turn();
    assert(full_refreshes==before_midnight+1U && rendered_timestamp==1791504007U);
    assert(p.last_full_clear==rendered_timestamp && !p.force_full_refresh);
    for(unsigned i=0U;i<90U;i++) {tick();main_turn();}
    assert(full_refreshes==before_midnight+1U && !p.display_queue.pending && !scheduled_worker);

    /* A redraw already in progress at midnight commits its original date.
     * One subsequent timer/main turn repairs the date and then stays idle. */
    now=1791590395U;
    ble_epd_on_timer(&p,now,true); main_turn();
    assert(rendered_timestamp==1791590395U && now>=1791590400U);
    before_midnight=full_refreshes;
    tick(); main_turn();
    assert(full_refreshes==before_midnight+1U && rendered_timestamp>=1791590400U);
    for(unsigned i=0U;i<90U;i++) {tick();main_turn();}
    assert(full_refreshes==before_midnight+1U && !p.force_full_refresh && !p.display_queue.pending);
    /* Simulate full-chip erase followed by LEAVE: missing config returns before
     * consumer activation, but stale TODO metadata must already be suspended.
     * A connection-triggered NO DATA frame is allowed; timer retries are not. */
    assert(epd_resource_event(&p, RESOURCE_INVALIDATE, 1U << RS_PART_PARTITION_TABLE) == RS_OK);
    flash_erased = true;
    assert(!todo_items_ready() && !epd_resources_ready());
    before_panel = full_refreshes;
    unsigned before_ack = resource_acks;
    activate(1U << RS_PART_PARTITION_TABLE, 49U); main_turn();
    assert(last_resource_result == RS_INVALID && resource_acks == before_ack + 1U);
    assert(full_refreshes == before_panel && !p.display_queue.pending);
    ble_epd_on_timer(&p, now, true); main_turn();
    assert(full_refreshes == before_panel + 1U);
    for (unsigned i = 0U; i < 180U; ++i) { tick(); main_turn(); }
    assert(full_refreshes == before_panel + 1U && !p.display_queue.pending && !scheduled_worker && !refs);
    /* Reinstall all packages, then receive the reliable activation ACK before
     * the first restored frame. Subsequent ticks settle back to idle. */
    flash_erased = false;
    activate(1U << RS_PART_PARTITION_TABLE, 50U); main_turn();
    assert(last_resource_result == RS_OK && resource_acks == before_ack + 2U);
    assert(epd_resources_ready() && todo_items_ready() && full_refreshes == before_panel + 2U);
    for (unsigned i = 0U; i < 180U; ++i) { tick(); main_turn(); }
    assert(full_refreshes == before_panel + 2U && !p.display_queue.pending && !scheduled_worker && !refs);
    puts("PASS: erased Flash rejects activation, idles after fallback, and accepts reinstall ACK before the restored TODO frame");
    puts("PASS: TODO uptime backoff, resource/user/day/mode resets, wraparound and disconnected recovery");
    puts("PASS: delayed TODO worker uses current time; resident-only calendar updates across midnight exactly once");
    puts("PASS: reliable ACK before TODO workers, v4 lead/trigger/deadline/removal, phase failure retry and no repeated redraw");
    return 0;
}
