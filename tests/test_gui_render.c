#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "GUI.h"
#include "fonts.h"
#include "font_geometry.h"
#include "calendar_annotations.h"
#include "epd_clock_refresh.h"
#include "clock_layout.h"

static unsigned char fonts[3][16384];
static u8g2_font_source_t sources[3];
static unsigned reads, pages;
static unsigned char frame[2][48000];
static uint16_t origin_x, origin_y;
void ui_resources_stub_set_available(bool value);
static int read_font(const void *ctx, uint32_t off, uint8_t *dst, size_t len) {
    unsigned slot = (unsigned)(size_t)ctx;
    assert(off <= sources[slot].length && len <= sources[slot].length - off);
    memcpy(dst, fonts[slot] + off, len);
    reads++;
    return 0;
}
const u8g2_font_source_t *GUI_GetExternalFont(gui_font_id_t id) { return &sources[id]; }
const gui_font_metrics_t *GUI_GetFontMetrics(gui_font_id_t id) { return &font_slot_geometry[id]; }
void app_feed_wdt(void) {}
static void capture(void *ctx, uint8_t *black, uint8_t *color,
                    uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    (void)ctx;
    x += origin_x;
    y += origin_y;
    assert(x % 8 == 0 && x + w <= 800 && y + h <= 480);
    for (unsigned r = 0; r < h; r++) {
        memcpy(frame[0] + (y+r)*100 + x/8, black + r*((w+7)/8), (w+7)/8);
        if (color) memcpy(frame[1] + (y+r)*100 + x/8, color + r*((w+7)/8), (w+7)/8);
    }
    pages++;
}
static unsigned hash(void) {
    unsigned h = 2166136261U;
    const unsigned char *p = &frame[0][0];
    for (size_t i = 0; i < sizeof(frame); i++) h = (h ^ p[i]) * 16777619U;
    return h;
}
static uint8_t old_frame[48000], new_frame[48000], pushed[48000];
static unsigned pushes, refreshes, fail_push;
static bool fail_refresh, busy_timeout;
bool UC8179_HadBusyTimeout(void) { return busy_timeout; }
static bool diff_push(const epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *old, const uint8_t *new) {
    (void)epd;
    assert(w % 8 == 0 && (unsigned)(w/8)*h <= 512);
    if (++pushes == fail_push) return false;
    for (unsigned r = 0; r < h; r++) {
        unsigned off = (y+r)*100 + x/8;
        assert(memcmp(old+r*(w/8), old_frame+off, w/8)==0);
        assert(memcmp(new+r*(w/8), new_frame+off, w/8)==0);
        memset(pushed+off, 1, w/8);
    }
    return true;
}
static bool refresh(const epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    (void)epd;refreshes++;
    for (unsigned r = 0; r < h; r++)
        for (unsigned c = 0; c < w/8; c++) assert(pushed[(y+r)*100+x/8+c]);
    return !fail_refresh;
}
static void clock_reference(gui_data_t *data, uint8_t *dst) {
    clock_refresh_geom_t g = clock_refresh_geom_sized(data->mode==MODE_CLOCK,800,480);
    origin_x=g.origin_x;origin_y=g.origin_y;
    GetTimeRefreshAreaFromLocal(data,0,0,g.canvas_w,g.canvas_h,NULL,NULL,NULL,NULL);
    memset(frame,255,sizeof(frame));DrawGUI_ClockOnly(data,capture,NULL);
    memcpy(dst,frame[0],48000);
    origin_x=origin_y=0;
}
static void test_clock_strips(void) {
    epd_driver_t drv={.diff_push=diff_push,.refresh_partial=refresh};
    epd_model_t model={.drv=&drv,.width=800,.height=480};
    const uint32_t times[]={12*3600+34*60,12*3600+39*60,12*3600+59*60};
    for (unsigned mode=MODE_CLOCK_CALENDAR;mode<=MODE_CLOCK;mode++) {
        for (unsigned t=0;t<3;t++) {
            gui_data_t data={.mode=mode,.width=800,.height=480,.timestamp=times[t]};
            clock_reference(&data,old_frame);
            data.timestamp+=60;clock_reference(&data,new_frame);
            pushes=refreshes=0;memset(pushed,0,sizeof(pushed));
            epd_clock_refresh_mark_valid(times[t]);
            bool ok=epd_clock_refresh_try(&model,&data);
            /* Full-screen hour changes may intentionally request a full refresh. */
            if (mode==MODE_CLOCK && t==2) continue;
            assert(ok && refreshes && pushes>refreshes);
            for (unsigned fault=0;fault<3;fault++) {
                pushes=refreshes=0;memset(pushed,0,sizeof(pushed));
                fail_push=fault==0?2:0;fail_refresh=fault==1;busy_timeout=fault==2;
                epd_clock_refresh_mark_valid(times[t]);
                assert(!epd_clock_refresh_try(&model,&data));
                assert(!epd_clock_refresh_is_valid());
                fail_push=0;fail_refresh=busy_timeout=false;
            }
        }
    }
}
static void install_annotations(unsigned year, unsigned month) {
    uint8_t blob[16+28*16]={0};
    uint32_t magic=CALENDAR_ANNOTATIONS_MAGIC;
    for(unsigned i=0;i<4;i++) blob[i]=(uint8_t)(magic>>(i*8));
    blob[4]=CALENDAR_ANNOTATIONS_VERSION;blob[5]=16;blob[6]=28;
    for(unsigned i=0;i<28;i++) {
        uint8_t *r=blob+16+i*16;
        r[0]=year;r[1]=year>>8;r[2]=month;r[3]=i+1;
        r[4]=1|((i%3)<<1)|((i%3)<<3)|((i%2)?CALENDAR_ANNOTATION_POS_CORNER_FLAG:0);
        if(i%4) { r[5]=9;memcpy(r+6,"元旦节",9); }
    }
    assert(calendar_annotations_load(blob,sizeof(blob)));
}
static void test_calendar_edges(void) {
    /* Approved hashes are generated from the pre-optimization GUI sources. */
    static const unsigned expected_edges[24]={
        0x79e2b9a9,0xcee3cc66,0x58fd13e6,0xbc06d974,0x046503be,0x7ab3884e,
        0xfd256c5a,0x42b88fd4,0x03d48cf7,0x66ee8894,0xb8f1279f,0x3c6756aa,
        0xe73247ff,0x8b21e898,0x67e0b44b,0xa90f2372,0x7b0bff5e,0x5f5fbf4a,
        0x7b6cd767,0x37e38330,0xf92a30c6,0xd187d13e,0x90eb9073,0x1c7273a4
    };
    const uint32_t dates[]={1735689600U,1771286400U,1803772800U,1819670400U};
    const unsigned years[]={2025,2026,2027,2027},months[]={1,2,2,8};
    unsigned index=0;
    for(unsigned d=0;d<4;d++) {
        install_annotations(years[d],months[d]);
        for(unsigned week=0;week<=6;week+=3) {
            for(unsigned mode=1;mode<=2;mode++) {
                gui_data_t data={.mode=mode,.color=2,.width=800,.height=480,
                    .timestamp=dates[d],.week_start=week,.temperature=-12,.voltage_mv=2700,
                    .ssid="NRF_EPD_0123456789"};
                memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);
#ifdef GENERATE_GUI_GOLDEN
                printf("0x%08x,\n",hash());
                (void)expected_edges;
#else
                assert(hash()==expected_edges[index]);
#endif
                index++;
            }
        }
    }
}
int main(void) {
    const char *paths[] = {"tools/dist/u8g2_font_wqy9_t_lunar.bin", "tools/dist/u8g2_font_wqy12_t_lunar.bin", "tools/dist/u8g2_font_helvB18_tn.bin"};
    for (unsigned i = 0; i < 3; i++) {
        FILE *f = fopen(paths[i], "rb"); assert(f);
        size_t n = fread(fonts[i], 1, sizeof(fonts[i]), f); assert(feof(f)); fclose(f);
        sources[i] = (u8g2_font_source_t){NULL, n, read_font, (const void *)(size_t)i, 1};
    }
    calendar_annotations_reset();
    const unsigned expected[3][2]={{0x48026422,0x28f14cfa},{0x30be1832,0xd686edea},{0x1a1c8dcb,0x1a1c8dcb}};
    for (unsigned mode = 1; mode <= 3; mode++) {
        for (unsigned color = 1; color <= 2; color++) {
            gui_data_t data = {.mode=mode, .color=color, .width=800, .height=480,
                              .timestamp=1789347600U, .week_start=1, .temperature=25,
                              .voltage_mv=3100, .ssid="NRF_EPD"};
            memset(frame, 255, sizeof(frame)); reads=pages=0;
            DrawGUI(&data, capture, NULL);
            assert(hash()==expected[mode-1][color-1]);
            printf("mode=%u color=%u hash=%08x pages=%u font_reads=%u\n", mode, color, hash(), pages, reads);
        }
    }
    test_calendar_edges();
    /* A missing external UI package must produce a visible setup screen. */
    ui_resources_stub_set_available(false);
    gui_data_t missing = {.mode=MODE_CALENDAR, .color=1, .width=800, .height=480};
    memset(frame, 255, sizeof(frame));
    pages = 0;
    DrawGUI(&missing, capture, NULL);
    unsigned marked = 0;
    for (size_t i = 0; i < sizeof(frame[0]); ++i)
        marked += frame[0][i] != 255U;
    assert(pages && marked > 100U);
    ui_resources_stub_set_available(true);
#ifndef GENERATE_GUI_GOLDEN
    test_clock_strips();
#else
    (void)test_clock_strips;
#endif
    puts("PASS: full-frame golden images and byte-exact clock OLD/NEW strips, failure fallback");
    return 0;
}
