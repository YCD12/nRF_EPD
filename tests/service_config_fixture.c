/* Real config command/activation handlers with observable hardware ownership. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_protocol.h"
#include "epd_cmd_policy.h"
#include "epd_refresh_policy.h"
#include "epd_offline.h"
#include "calendar_annotations_tx.h"
#include "resource_protocol.h"
#include "resource_layout.h"
#define EPD_CMD_QUEUE_SIZE 2U
#define EPD_CMD_MAX_DATA_LEN 16U
#include "epd_command_queue.h"
#include "epd_display_queue.h"

#define EPD_UC8179_750_BWR 7U
typedef struct { uint16_t value_handle; } ble_gatts_char_handles_t;
typedef struct epd_model epd_model_t;
typedef struct { void (*sleep)(const epd_model_t *); } driver_t;
struct epd_model { const driver_t *drv; };

/* PRODUCTION_SERVICE_TYPES */

static epd_config_t saved, loaded;
static unsigned refs, writes, invalidations, requests, timer_requests, reloads;
static char hardware[8];
static unsigned hardware_length;
static ble_epd_t *current;
static bool read_waiting, read_failure, config_saving;
static bool annotation_rx, annotation_reload, annotation_saving;
static unsigned retry_reads;
static void trace(char op) { assert(hardware_length < sizeof(hardware)); hardware[hardware_length++] = op; }
void test_critical_enter(void) {}
void test_critical_exit(void) {}
static bool nrf_gpio_pin_present_check(unsigned pin) { return pin < 32U; }
static bool epd_model_supported(uint8_t id) { return id == EPD_UC8179_750_BWR; }
static void EPD_ClearBusyAbort(void) {}
static void EPD_GPIO_Uninit(void) { assert(refs == 1U); refs--; trace('U'); }
static void EPD_GPIO_Load(epd_config_t *cfg) { assert(refs == 0U); loaded = *cfg; trace('L'); }
static void panel_sleep(const epd_model_t *p) { (void)p; assert(refs == 1U); trace('S'); }
static const driver_t driver = {panel_sleep};
static const epd_model_t panel = {&driver};
static void epd_clock_refresh_invalidate(void) { invalidations++; }
void epd_config_write(epd_config_t *cfg) { saved = *cfg; writes++; read_waiting = false; }
void epd_config_read(epd_config_t *cfg) {
    *cfg = saved;
    read_waiting = read_failure;
    if (read_failure) memset(cfg, 0xFF, sizeof(*cfg));
}
bool epd_config_read_pending(void) { return read_waiting; }
bool epd_config_retry_read(epd_config_t *cfg) {
    if (!read_waiting) return false;
    assert(current->is_refreshing); /* IRQ image writes are excluded. */
    retry_reads++;
    epd_config_read(cfg);
    return !read_waiting;
}
bool epd_config_busy(void) { return config_saving; }
static bool calendar_annotations_store_maintenance_ready(void) {
    return !annotation_saving && (!annotation_rx || annotation_reload);
}
static unsigned timestamp(void) { return 1000U; }
static void ble_epd_on_timer(ble_epd_t *p, unsigned ts, bool force)
{
    (void)p;
    assert(ts == timestamp() && force);
    timer_requests++;
}
static void epd_display_request(ble_epd_t *p, unsigned ts) { (void)p; assert(ts == timestamp()); requests++; }
static void epd_resources_invalidate_partitions(uint16_t changed) { (void)changed; }
static bool epd_resources_ready(void) { return true; }
static rs_result_t epd_resources_apply(void) { reloads++; return RS_OK; }
static void calendar_annotations_store_init(void) {}
static bool calendar_annotations_store_valid(void) { return true; }
static bool todo_items_init(void) { return true; }
static bool todo_items_ready(void) { return true; }
static void todo_items_invalidate(void) {}
static uint8_t todo_items_font_slot(void) { return 1U; }
static uint8_t todo_items_font_size(void) { return 12U; }
static uint32_t todo_items_font_key(void) { return 0U; }
static void todo_items_suspend_display(void) { assert(!"default font must never suspend"); }
static bool GUI_TodoFontMatches(uint8_t size, uint32_t key)
{ (void)size; (void)key; assert(!"default font must not query custom dependency"); return false; }

/* PRODUCTION_HANDLERS */

static void reset(ble_epd_t *p)
{
    memset(p, 0, sizeof(*p));
    p->config = (epd_config_t){
        .mosi_pin=20, .sclk_pin=19, .cs_pin=6, .dc_pin=5, .rst_pin=4,
        .busy_pin=3, .bs_pin=2, .model_id=7, .wakeup_pin=255,
        .led_pin=18, .en_pin=7, .display_mode=MODE_CLOCK, .week_start=1,
        .full_refresh_minutes=30,
    };
    p->epd = &panel;
    p->gpio_connection_ref = true;
    refs = 1U;
    writes = invalidations = requests = timer_requests = reloads = hardware_length = 0U;
    saved = loaded = p->config;
    current = p;
    read_waiting = read_failure = config_saving = false;
    annotation_rx = annotation_reload = annotation_saving = false;
    retry_reads = 0U;
    memset(hardware, 0, sizeof(hardware));
}

static void send_config(ble_epd_t *p, epd_config_t cfg)
{
    uint8_t command[1U + sizeof(cfg)] = {EPD_CMD_SET_CONFIG};
    memcpy(command + 1, &cfg, sizeof(cfg));
    cmd_set_config(p, command, sizeof(command));
}

static void test_invalid_updates(void)
{
    ble_epd_t p;
    for (unsigned field = 0; field < 4U; field++) {
        reset(&p);
        epd_config_t candidate = p.config;
        if (field == 0U) candidate.display_mode = 6U;
        if (field == 1U) candidate.week_start = 7U;
        if (field == 2U) candidate.model_id = 8U;
        if (field == 3U) candidate.mosi_pin = 32U;
        send_config(&p, candidate);
        assert(!writes && !invalidations && !timer_requests && !hardware_length);
        assert(!memcmp(&p.config, &loaded, sizeof(p.config)) && p.epd == &panel);
        saved = candidate;
        assert(epd_resource_event(&p, RESOURCE_ACTIVATE, 1U << RS_PART_EPD_CONFIG) == RS_INVALID);
        assert(!reloads && !requests && refs == 1U && !hardware_length);
    }
}

static void test_display_only(void)
{
    ble_epd_t p;
    reset(&p);
    epd_config_t candidate = p.config;
    candidate.week_start = 6U;
    send_config(&p, candidate);
    assert(writes == 1U && timer_requests == 1U && invalidations == 1U);
    assert(!hardware_length && refs == 1U && p.epd == &panel && p.gpio_connection_ref);
    assert(p.force_full_refresh && !memcmp(&saved, &p.config, sizeof(saved)));

    saved.display_mode = 2U;
    assert(epd_resource_event(&p, RESOURCE_ACTIVATE, 1U << RS_PART_EPD_CONFIG) == RS_OK);
    assert(p.config.display_mode == 2U && requests == 1U && reloads == 1U);
    assert(!hardware_length && refs == 1U && p.epd == &panel);
    for (uint8_t mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; mode++) {
        saved.display_mode = mode;
        assert(epd_resource_event(&p, RESOURCE_ACTIVATE, 1U << RS_PART_EPD_CONFIG) == RS_OK);
        assert(p.config.display_mode == mode && requests == mode - 2U && reloads == mode - 2U);
        assert(!hardware_length && refs == 1U && p.epd == &panel);
    }
}

static void test_hardware_changes(void)
{
    ble_epd_t p;
    reset(&p);
    epd_config_t candidate = p.config;
    candidate.mosi_pin = 21U;
    send_config(&p, candidate);
    assert(hardware_length == 3U && !memcmp(hardware, "SUL", 3U));
    assert(!refs && !p.gpio_connection_ref && !p.epd);
    assert(loaded.mosi_pin == 21U && !memcmp(&p.config, &loaded, sizeof(loaded)));
    assert(writes == 1U && timer_requests == 1U && p.force_full_refresh);

    reset(&p);
    saved.led_pin = 0xFFU;
    assert(epd_resource_event(&p, RESOURCE_ACTIVATE, 1U << RS_PART_EPD_CONFIG) == RS_OK);
    assert(hardware_length == 3U && !memcmp(hardware, "SUL", 3U));
    assert(!refs && !p.epd && !p.gpio_connection_ref && loaded.led_pin == 0xFFU);

    reset(&p);
    candidate = p.config;
    candidate.model_id = 0xFFU;
    send_config(&p, candidate);
    assert(!p.epd && !refs && p.config.model_id == 0xFFU && saved.model_id == 0xFFU);
    assert(hardware_length == 3U && !memcmp(hardware, "SUL", 3U));

    reset(&p);
    uint8_t pins[] = {EPD_CMD_SET_PINS, 21, 19, 6, 5, 4, 3, 255, 255};
    cmd_set_pins(&p, pins, sizeof(pins));
    assert(hardware_length == 3U && !memcmp(hardware, "SUL", 3U));
    assert(!refs && !p.epd && !p.gpio_connection_ref && loaded.mosi_pin == 21U);
    assert(!timer_requests && writes == 1U && invalidations == 1U);
}

static void test_read_recovery(void)
{
    ble_epd_t p;
    reset(&p);
    saved.mosi_pin = 21U;
    saved.week_start = 4U;
    saved.display_mode = MODE_CALENDAR;
    read_waiting = read_failure = true;
    epd_config_restore(&p);
    assert(retry_reads == 1U && read_waiting && !writes && !requests);
    assert(!p.is_refreshing && refs == 1U && p.epd == &panel && !hardware_length);
    read_failure = false;
    p.cmd_queue.count = 1U;
    epd_config_restore(&p);
    p.cmd_queue.count = 0U;
    p.init_pending = true;
    epd_config_restore(&p);
    p.init_pending = false;
    p.transfer_ctx.transfer_active = true;
    epd_config_restore(&p);
    p.transfer_ctx.transfer_active = false;
    assert(retry_reads == 1U); /* Accepted commands and image work take precedence. */
    epd_config_restore(&p);
    assert(retry_reads == 2U && !read_waiting && !writes && requests == 1U);
    assert(!p.is_refreshing && !refs && !p.epd && !p.gpio_connection_ref);
    assert(hardware_length == 3U && !memcmp(hardware, "SUL", 3U));
    assert(p.config.mosi_pin == 21U && loaded.mosi_pin == 21U);
    assert(p.config.week_start == 4U && p.config.display_mode == MODE_CALENDAR);
    assert(p.force_full_refresh);
    epd_config_restore(&p);
    assert(retry_reads == 2U && requests == 1U);

    /* Explicitly selecting the temporary default is still a new user choice. */
    reset(&p);
    saved.display_mode = MODE_CALENDAR;
    read_waiting = true;
    epd_update_display_mode(&p, MODE_CLOCK);
    epd_config_restore(&p);
    assert(writes == 1U && !retry_reads && p.config.display_mode == MODE_CLOCK);
    assert(!read_waiting && saved.display_mode == MODE_CLOCK);
    reset(&p);
    saved.week_start = 4U;
    read_waiting = true;
    uint8_t week[] = {EPD_CMD_SET_WEEK_START, 1U};
    cmd_set_week_start(&p, week, sizeof(week));
    epd_config_restore(&p);
    assert(writes == 1U && !retry_reads && p.config.week_start == 1U);
    assert(!read_waiting && saved.week_start == 1U);

    /* Confirmed absence/corruption may install defaults after retry. */
    reset(&p);
    memset(&saved, 0xFF, sizeof(saved));
    read_waiting = true;
    epd_config_restore(&p);
    assert(writes == 1U && requests == 1U && !read_waiting);
    assert(epd_config_validate(&saved) && p.config.week_start == 0U);
    assert(p.config.display_mode == MODE_CLOCK_CALENDAR);

    /* Activation I/O errors preserve the current RAM config and later recover. */
    reset(&p);
    saved.week_start = 4U;
    read_failure = true;
    assert(epd_resource_event(&p, RESOURCE_ACTIVATE, 1U << RS_PART_EPD_CONFIG) == RS_IO);
    assert(read_waiting && !writes && !requests && p.config.week_start == 1U);
    read_failure = false;
    epd_config_restore(&p);
    assert(!read_waiting && !writes && requests == 1U && p.config.week_start == 4U);

    /* A failed annotation read must not lock out resource repair or config
     * recovery; actual incoming annotations and pending saves still do. */
    reset(&p);
    annotation_rx = annotation_reload = true;
    assert(epd_resource_event(&p, RESOURCE_ENTER, 0U) == RS_OK);
    read_waiting = true;
    saved.week_start = 5U;
    epd_config_restore(&p);
    assert(retry_reads == 1U && p.config.week_start == 5U);
    annotation_reload = false;
    assert(epd_resource_event(&p, RESOURCE_ENTER, 0U) == RS_BUSY);
    annotation_rx = false;
    annotation_saving = true;
    assert(epd_resource_event(&p, RESOURCE_ENTER, 0U) == RS_BUSY);
    puts("PASS: temporary config reads preserve Flash, recover GPIO/display, and yield to user settings/maintenance");
}

int main(void)
{
    test_invalid_updates();
    test_display_only();
    test_hardware_changes();
    test_read_recovery();
    puts("PASS: shared config validation, display-only edits and deferred GPIO/model replacement");
    return 0;
}
