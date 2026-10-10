"""Exercise real TODO scheduling and service handlers with hardware shims."""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source, name):
    match = re.search(r"(?:static )?(?:void|bool) " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "epd_refresh_policy.h"
#include "epd_battery_policy.h"
#include "resource_store.h"
#include "resource_layout.h"
#include "todo_items.h"

#define NRF_LOG_INFO(...) test_log(__VA_ARGS__)
#define NRF_LOG_WARNING(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
#define ble_log(...) ((void)0)
#define EPD_LED_OFF() ((void)0)
#define EPD_LED_BLINK() ((void)0)
#define EPD_ANNOTATIONS_RX_TIMEOUT_S 10U
#define LOW_BATTERY_POLL_INTERVAL_S 10U

typedef struct epd_model epd_model_t;
typedef struct {
    void (*init)(const epd_model_t *);
    buffer_callback write_image;
    void (*refresh)(const epd_model_t *);
    int8_t (*read_temp)(const epd_model_t *);
} driver_t;
struct epd_model { const driver_t *drv; };
typedef struct {
    epd_config_t config;
    struct { bool transfer_active; } transfer_ctx;
    bool is_refreshing, low_battery_screen_active, force_full_refresh;
    bool annotations_rx_abort_requested;
    uint32_t annotations_rx_last_activity, last_refresh_start, last_full_clear;
} ble_epd_t;

static uint8_t payload[TODO_ITEMS_HEADER_SIZE + 2U * TODO_ITEMS_RECORD_SIZE + 5U];
static uint32_t payload_length = TODO_ITEMS_HEADER_SIZE + 2U * TODO_ITEMS_RECORD_SIZE;
static bool entered, timer_active, fail_read, fail_panel, panel_timeout, render_valid;
static bool interrupt_during_panel;
static unsigned requests, store_reads, store_opens, draws, panel_refreshes;
static uint32_t now, requested_timestamp;
static ble_epd_t *current;
static todo_selection_t rendered_selection;

static void put32(uint8_t *p, uint32_t value)
{ for (unsigned i=0U; i<4U; i++) p[i]=(uint8_t)(value>>(8U*i)); }
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    assert(!timer_active && id == RS_PART_TODO_ITEMS);
    store_opens++;
    *length=payload_length;
    return RS_OK;
}
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    assert(!timer_active && id == RS_PART_TODO_ITEMS);
    store_reads++;
    if (fail_read) return RS_IO;
    assert(offset<=payload_length && length<=payload_length-offset);
    memcpy(data,payload+offset,length);
    return RS_OK;
}
static void make_payload(void)
{
    memset(payload,0,sizeof(payload));
    put32(payload,TODO_ITEMS_MAGIC);
    payload[4]=1U; payload[5]=80U; payload[6]=2U; payload[8]=1U; payload[9]=9U;
    uint8_t *first=payload+16U, *second=first+80U;
    put32(first,1U); first[12]=TODO_ITEM_RESIDENT; first[13]=3U;
    memcpy(first+14U,"pin",3U);
    put32(second,2U); put32(second+4U,1027U); put32(second+8U,1030U);
    second[13]=5U; memcpy(second+14U,"timed",5U);
}
bool resource_protocol_entered(void) { return entered; }
static bool calendar_annotations_rx_active(void) { return false; }
static uint32_t app_uptime_seconds(void) { return now; }
static uint16_t EPD_ReadVoltageAndCache(void) { return 3300U; }
static void epd_display_request(ble_epd_t *p, uint32_t ts)
{ assert(p==current); requests++; requested_timestamp=ts; }
static void app_feed_wdt(void) {}
static void test_log(const char *format, ...) { (void)format; }
static uint32_t app_timer_cnt_get(void) { return 0U; }
static uint32_t epd_elapsed_ms(uint32_t ticks) { return ticks; }
static void epd_clock_refresh_invalidate(void) {}
static void epd_clock_refresh_mark_valid(uint32_t ts) { (void)ts; assert(0); }
static void UC8179_ClearBusyTimeout(void) { panel_timeout=false; }
static bool UC8179_HadBusyTimeout(void) { return panel_timeout; }
static bool EPD_BusyAbortRequested(void) { return false; }
static void tick(ble_epd_t *p, uint32_t ts, bool force);
static void panel_init(const epd_model_t *p) { (void)p; }
static void panel_write(void *ctx,uint8_t *black,uint8_t *color,uint16_t x,
                        uint16_t y,uint16_t width,uint16_t height)
{ (void)ctx;(void)black;(void)color;(void)x;(void)y;(void)width;(void)height; }
static void panel_refresh(const epd_model_t *p)
{
    (void)p;
    panel_refreshes++;
    panel_timeout=fail_panel;
    if (interrupt_during_panel) tick(current,now+1U,false);
}
static const driver_t driver={panel_init,panel_write,panel_refresh,NULL};
static const epd_model_t panel={&driver};
void DrawGUI(gui_data_t *data, buffer_callback callback, void *ctx)
{
    todo_selection_t selection;
    todo_item_t item;
    assert(callback==panel_write && ctx==&panel);
    draws++;
    render_valid=todo_items_select(data->timestamp,&selection);
    rendered_selection=selection;
    for (unsigned i=0U; i<selection.count; i++) {
        if (!todo_items_read(selection.index[i],&item)) render_valid=false;
    }
}
bool GUI_TodoRenderValid(void) { return render_valid; }

/* PRODUCTION_HANDLERS */

static void tick(ble_epd_t *p, uint32_t ts, bool force)
{
    unsigned reads_before=store_reads, opens_before=store_opens;
    now=ts;
    timer_active=true;
    ble_epd_on_timer(p,ts,force);
    timer_active=false;
    assert(store_reads==reads_before && store_opens==opens_before);
}
static bool refresh_at(ble_epd_t *p, uint32_t ts)
{
    gui_data_t data={.mode=(display_mode_t)p->config.display_mode,.timestamp=ts,.voltage_mv=3300U};
    p->is_refreshing=true;
    now=ts;
    bool result=epd_perform_full_refresh(p,&panel,&data);
    p->is_refreshing=false;
    return result;
}
int main(int argc, char **argv)
{
    assert(argc == 2 && strlen(argv[1]) == 1U);
    display_mode_t mode = (display_mode_t)(argv[1][0] - '0');
    assert(epd_is_todo_mode(mode));
    ble_epd_t p={.config={.display_mode=mode}};
    current=&p;
    make_payload(); assert(todo_items_init());
    tick(&p,1001U,false);
    assert(requests==1U && requested_timestamp==1001U);
    assert(refresh_at(&p,1001U));
    assert(!todo_items_selection_changed(1001U));
    tick(&p,1020U,false); /* No minute refresh for an unchanged TODO pane. */
    assert(requests==1U);
    tick(&p,1027U,false); /* The queued item becomes active behind the pin. */
    assert(requests==1U); /* No visible row, phase or header count changed. */
    tick(&p,1027U,true); /* Explicit redraw still works with unchanged pixels. */
    assert(requests==2U && requested_timestamp==1027U);
    /* A timer interrupt inside the long panel refresh cannot queue the same
     * uncommitted selection for another complete screen refresh. */
    interrupt_during_panel=true;
    assert(refresh_at(&p,1027U));
    interrupt_during_panel=false;
    assert(requests==2U && !todo_items_selection_changed(1028U));
    tick(&p,1030U,false);
    assert(requests==2U); /* Hidden queue expiry does not redraw the screen. */
    tick(&p,1030U,true);
    assert(requests==3U); /* Exercise a forced refresh's failure/retry below. */
    fail_read=true;
    unsigned before_panel=panel_refreshes;
    assert(!refresh_at(&p,1030U));
    assert(panel_refreshes==before_panel && p.force_full_refresh && !todo_items_selection_changed(1030U));
    fail_read=false;
    tick(&p,1031U,false); assert(requests==4U);
    fail_panel=true;
    assert(!refresh_at(&p,1031U) && p.force_full_refresh && !todo_items_selection_changed(1031U));
    fail_panel=false;
    tick(&p,1032U,false); assert(requests==5U);
    assert(refresh_at(&p,1032U));
    tick(&p,1033U,false); assert(requests==5U);
    tick(&p,1034U,true); assert(requests==6U); /* Explicit forced redraw. */
    assert(refresh_at(&p,1034U));

    p.transfer_ctx.transfer_active=true;
    tick(&p,86401U,true);
    assert(requests==6U); /* A transfer must also block forced timer work. */
    p.transfer_ctx.transfer_active=false;
    tick(&p,86402U,false);
    assert(requests==7U && p.force_full_refresh); /* Catch day rollover after transfer. */
    assert(refresh_at(&p,86402U));
    tick(&p,86460U,false); assert(requests==7U);

    entered=true;
    tick(&p,172801U,true); assert(requests==7U);
    entered=false;
    tick(&p,172802U,false);
    assert(requests==8U && p.force_full_refresh); /* Resource upload preserved day state. */
    assert(refresh_at(&p,172802U));

    p.is_refreshing=true;
    tick(&p,259201U,false); assert(requests==8U);
    p.is_refreshing=false;
    tick(&p,259202U,false);
    assert(requests==9U && p.force_full_refresh); /* A busy TODO refresh preserves rollover. */
    assert(refresh_at(&p,259202U));
    tick(&p,259260U,false); assert(requests==9U);

    /* A failed rollover must retry even when the active TODO set is identical
     * to the last successful screen and the timer has already seen this day. */
    tick(&p,345601U,false);
    assert(requests==10U && p.force_full_refresh);
    fail_panel=true;
    assert(!refresh_at(&p,345601U));
    fail_panel=false;
    assert(!todo_items_selection_changed(345602U));
    tick(&p,345602U,false);
    assert(requests==11U && p.force_full_refresh);
    assert(refresh_at(&p,345602U));
    tick(&p,345603U,false); assert(requests==11U);

    tick(&p,345604U,true); assert(requests==12U);
    fail_read=true;
    assert(!refresh_at(&p,345604U));
    fail_read=false;
    assert(!todo_items_selection_changed(345605U));
    tick(&p,345605U,false); assert(requests==13U);
    assert(refresh_at(&p,345605U));
    tick(&p,345606U,false); assert(requests==13U);

    /* Time corrections in both directions update the visible calendar day. */
    tick(&p,1027U,false);
    assert(requests==14U && p.force_full_refresh);
    assert(refresh_at(&p,1027U));
    p.transfer_ctx.transfer_active=true;
    tick(&p,1030U,false); assert(requests==14U);
    p.transfer_ctx.transfer_active=false;
    tick(&p,1031U,false); assert(requests==14U); /* Only the hidden queue expired. */
    tick(&p,1031U,true); assert(requests==15U);
    assert(refresh_at(&p,1031U));
    entered=true;
    todo_items_invalidate();
    tick(&p,1032U,false); assert(requests==15U);
    entered=false;
    tick(&p,1033U,false); assert(requests==16U);
    assert(refresh_at(&p,1033U));
    todo_items_reset(); /* Corrupt/unloaded metadata cannot be used by the IRQ. */
    tick(&p,1034U,false); assert(requests==16U);
    assert(draws>0U && store_reads>0U && store_opens==1U);

    /* Version 4 changes the appearance of the SAME selected record at the
     * trigger/deadline. Counts and IDs alone cannot detect these transitions.
     * Every timer call above and below checks that scheduling never reads Flash. */
    make_payload();
    payload[4]=TODO_ITEMS_TIMING_VERSION; payload[8]=2U;
    put32(payload+TODO_ITEMS_HEADER_SIZE+TODO_ITEMS_RECORD_SIZE+4U,2000U);
    put32(payload+TODO_ITEMS_HEADER_SIZE+TODO_ITEMS_RECORD_SIZE+8U,2200U);
    payload_length=TODO_ITEMS_HEADER_SIZE+2U*TODO_ITEMS_RECORD_SIZE+TODO_ITEMS_TIMING_TAIL_SIZE;
    payload[payload_length-4U]=1U; /* one-minute lead */
    payload[payload_length-2U]=1U; /* one-minute retention */
    assert(todo_items_init() && todo_items_lead_minutes()==1U && todo_items_retain_minutes()==1U);
    tick(&p,1850U,false); assert(requests==17U && refresh_at(&p,1850U));
    assert(rendered_selection.count==1U && rendered_selection.phase[0]==TODO_PHASE_RESIDENT);
    tick(&p,1939U,false); assert(requests==17U);
    tick(&p,1940U,false); assert(requests==18U && requested_timestamp==1940U);
    assert(refresh_at(&p,1940U));
    assert(rendered_selection.count==2U && rendered_selection.id[1]==2U &&
           rendered_selection.phase[1]==TODO_PHASE_UPCOMING);
    tick(&p,1999U,false); assert(requests==18U);
    tick(&p,2000U,false); assert(requests==19U && requested_timestamp==2000U);
    fail_panel=true;
    assert(!refresh_at(&p,2000U) && todo_items_selection_changed(2000U));
    fail_panel=false;
    tick(&p,2001U,false); assert(requests==20U && refresh_at(&p,2001U));
    assert(rendered_selection.count==2U && rendered_selection.id[1]==2U &&
           rendered_selection.phase[1]==TODO_PHASE_ACTIVE);
    tick(&p,2199U,false); assert(requests==20U);
    tick(&p,2200U,false); assert(requests==21U && requested_timestamp==2200U);
    fail_read=true; before_panel=panel_refreshes;
    assert(!refresh_at(&p,2200U) && panel_refreshes==before_panel && todo_items_selection_changed(2200U));
    fail_read=false;
    tick(&p,2201U,false); assert(requests==22U && refresh_at(&p,2201U));
    assert(rendered_selection.count==2U && rendered_selection.id[1]==2U &&
           rendered_selection.phase[1]==TODO_PHASE_EXPIRED);
    tick(&p,2259U,false); assert(requests==22U);
    tick(&p,2260U,false); assert(requests==23U && requested_timestamp==2260U);
    assert(refresh_at(&p,2260U) && rendered_selection.count==1U);
    for (uint32_t ts=2261U; ts<2350U; ts++) tick(&p,ts,false);
    assert(requests==23U && !todo_items_selection_changed(2349U));
    tick(&p,2200U,false); assert(requests==24U && refresh_at(&p,2200U));
    assert(rendered_selection.phase[1]==TODO_PHASE_EXPIRED);
    tick(&p,1999U,false); assert(requests==25U && refresh_at(&p,1999U));
    assert(rendered_selection.phase[1]==TODO_PHASE_UPCOMING);
    assert(store_opens==2U);

    /* A delayed old frame can complete after the timer has already observed
     * the new day. The physical screen, rather than the last timer tick, must
     * decide whether the split calendar still needs a daily update. Resident
     * rows and the daily total are identical on both days in this fixture. */
    const uint32_t old_day = 1791503999U; /* 2026-10-08 23:59:59 local epoch. */
    tick(&p,old_day,true);
    assert(refresh_at(&p,old_day));
    unsigned before_rollover=requests;
    tick(&p,old_day+1U,false);
    assert(requests==before_rollover+1U && p.force_full_refresh);
    assert(refresh_at(&p,old_day)); /* A stale queued frame cleared the flag. */
    assert(!p.force_full_refresh && !todo_items_selection_changed(old_day+2U));
    before_rollover=requests;
    tick(&p,old_day+2U,false);
    if (mode==MODE_TODO_CALENDAR) {
        assert(requests==before_rollover+1U && requested_timestamp==old_day+2U);
        assert(refresh_at(&p,old_day+2U));
    } else {
        assert(requests==before_rollover); /* Full TODO has no calendar date. */
    }
    before_rollover=requests;
    tick(&p,old_day+3U,false);
    assert(requests==before_rollover);
    puts("PASS: real TODO timer/full-refresh retry, RAM-only IRQ, rollover and v4 lead/trigger/deadline/removal phases");
    return 0;
}
'''

source = (ROOT / 'EPD/EPD_service.c').read_text(encoding='utf-8')
retry_state = re.search(r'static volatile uint32_t s_todo_retry_at;\s*'
                        r'static volatile uint8_t s_todo_retry_step;', source).group(0)
handlers = retry_state + '\n' + '\n'.join(extract_function(source, name) for name in
                     ('epd_todo_retry_reset', 'epd_todo_retry_failed',
                      'ble_epd_on_timer', 'epd_perform_full_refresh'))
out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
out.mkdir(parents=True, exist_ok=True)
generated = out / 'todo_integration_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', handlers), encoding='utf-8')
exe = out / ('test_todo_integration.exe' if os.name == 'nt' else 'test_todo_integration')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-IEPD', '-IGUI', str(generated),
                'GUI/todo_items.c', 'GUI/font_geometry.c', 'EPD/epd_refresh_policy.c', '-o', str(exe)],
               cwd=ROOT, check=True)
for mode in (4, 5):
    subprocess.run([str(exe.resolve()), str(mode)], check=True)
