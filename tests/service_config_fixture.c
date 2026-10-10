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
static void trace(char op) { assert(hardware_length < sizeof(hardware)); hardware[hardware_length++] = op; }
void test_critical_enter(void) {}
void test_critical_exit(void) {}
static bool nrf_gpio_pin_present_check(unsigned pin) { return pin < 32U; }
static void EPD_ClearBusyAbort(void) {}
static void EPD_GPIO_Uninit(void) { assert(refs == 1U); refs--; trace('U'); }
static void EPD_GPIO_Load(epd_config_t *cfg) { assert(refs == 0U); loaded = *cfg; trace('L'); }
static void panel_sleep(const epd_model_t *p) { (void)p; assert(refs == 1U); trace('S'); }
static const driver_t driver = {panel_sleep};
static const epd_model_t panel = {&driver};
static void epd_clock_refresh_invalidate(void) { invalidations++; }
void epd_config_write(epd_config_t *cfg) { saved = *cfg; writes++; }
void epd_config_read(epd_config_t *cfg) { *cfg = saved; }
static unsigned timestamp(void) { return 1000U; }
static void ble_epd_on_timer(ble_epd_t *p, unsigned ts, bool force)
{
    (void)p;
    assert(ts == timestamp() && force);
    timer_requests++;
}
static void epd_display_request(ble_epd_t *p, unsigned ts) { (void)p; assert(ts == timestamp()); requests++; }
static bool epd_resource_idle(const ble_epd_t *p) { (void)p; return true; }
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

int main(void)
{
    test_invalid_updates();
    test_display_only();
    test_hardware_changes();
    puts("PASS: shared config validation, display-only edits and deferred GPIO/model replacement");
    return 0;
}
