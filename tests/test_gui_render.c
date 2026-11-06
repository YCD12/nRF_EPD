#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "GUI.h"
#include "fonts.h"
#include "font_geometry.h"
#include "calendar_annotations.h"
#include "epd_clock_refresh.h"
#include "clock_layout.h"
#include "ui_resources.h"
#include "calendar_grid.h"
#include "calendar_resources.h"
#include "Lunar.h"
#include "todo_items.h"

#ifdef GUI_LAYOUT_REFERENCE
void DrawGUI_Unclipped(gui_data_t *data, buffer_callback callback, void *context);
bool GUI_TodoRenderValid_Unclipped(void);
#endif

static unsigned char fonts[4][16384];
static u8g2_font_source_t sources[4];
static unsigned reads, pages;
static unsigned char frame[2][48000];
static uint16_t origin_x, origin_y;
static bool missing_fonts;
static bool use_runtime_metrics;
static gui_font_metrics_t runtime_metrics[3];
static bool missing_todo_font;
static bool use_todo_font;
static uint8_t todo_font_size;
static uint32_t todo_font_key;
static bool extended_todo_fixture;
static unsigned fail_font_reads;
static unsigned failed_font_reads;
static unsigned font_reads_until_fault;
static gui_font_metrics_t extended_todo_metrics;
#define TODO_FIXTURE_FONT_KEY 0x12345678UL
void ui_resources_stub_set_available(bool value);
void ui_resources_stub_fail_read(unsigned nth);
unsigned ui_resources_stub_read_count(void);
unsigned ui_resources_stub_fault_count(void);
void todo_resources_stub_load(const uint8_t *data, size_t length);
void todo_resources_stub_fail_reads(bool fail);
void ui_resources_stub_set_component(ui_component_id_t id, const ui_component_t *value);
static int read_font(const void *ctx, uint32_t off, uint8_t *dst, size_t len) {
    unsigned slot = (unsigned)(size_t)ctx;
    assert(off <= sources[slot].length && len <= sources[slot].length - off);
    if (fail_font_reads != 0U && font_reads_until_fault == 0U) {
        fail_font_reads--;
        failed_font_reads++;
        return -1;
    }
    if (font_reads_until_fault != 0U) font_reads_until_fault--;
    memcpy(dst, fonts[slot] + off, len);
    reads++;
    return 0;
}
const u8g2_font_source_t *GUI_GetExternalFont(gui_font_id_t id) {
    if (id == GUI_FONT_TODO) {
        if (missing_todo_font) return NULL;
        if (extended_todo_fixture) return &sources[3];
        id = todo_font_size == 9U ? GUI_FONT_WQY9 : GUI_FONT_WQY12;
    }
    return missing_fonts || (unsigned)id >= 3U ? NULL : &sources[id];
}
const gui_font_metrics_t *GUI_GetFontMetrics(gui_font_id_t id) {
    if (id == GUI_FONT_TODO && extended_todo_fixture) return &extended_todo_metrics;
    if (id == GUI_FONT_TODO)
        id = todo_font_size == 9U ? GUI_FONT_WQY9 : GUI_FONT_WQY12;
    return (unsigned)id < 3U ? (use_runtime_metrics?&runtime_metrics[id]:&font_slot_geometry[id]) : NULL;
}
bool GUI_TodoFontMatches(uint8_t size, uint32_t key) {
    return GUI_GetExternalFont(GUI_FONT_TODO) != NULL && size == todo_font_size && key == todo_font_key;
}
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
/* epd_clock_refresh 经 vtable 可选能力判断局刷/忙状态。 */
static void init_partial(const epd_model_t *epd) { (void)epd; }
static bool busy_error(const epd_model_t *epd) { (void)epd; return busy_timeout; }
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
    epd_driver_t drv={.diff_push=diff_push,.refresh_partial=refresh,
                      .init_partial=init_partial,.busy_error=busy_error};
    epd_model_t model={.drv=&drv,.width=800,.height=480};
    const uint32_t times[]={12*3600+34*60,12*3600+39*60,12*3600+59*60};
    gui_data_t setup={.mode=MODE_CLOCK,.width=800,.height=480,.timestamp=1735689660U};
    epd_clock_refresh_mark_valid(1735689600U);pushes=refreshes=0;
    assert(!epd_clock_refresh_try(&model,&setup,false)&&pushes==0&&refreshes==0);
    for (unsigned mode=MODE_CLOCK_CALENDAR;mode<=MODE_CLOCK;mode++) {
        for (unsigned t=0;t<3;t++) {
            gui_data_t data={.mode=mode,.width=800,.height=480,.timestamp=times[t]};
            clock_reference(&data,old_frame);
            data.timestamp+=60;clock_reference(&data,new_frame);
            pushes=refreshes=0;memset(pushed,0,sizeof(pushed));
            epd_clock_refresh_mark_valid(times[t]);
            bool ok=epd_clock_refresh_try(&model,&data,false);
            /* Full-screen hour changes may intentionally request a full refresh. */
            if (mode==MODE_CLOCK && t==2) continue;
            assert(ok && refreshes && pushes>refreshes);
            for (unsigned fault=0;fault<3;fault++) {
                pushes=refreshes=0;memset(pushed,0,sizeof(pushed));
                fail_push=fault==0?2:0;fail_refresh=fault==1;busy_timeout=fault==2;
                epd_clock_refresh_mark_valid(times[t]);
                assert(!epd_clock_refresh_try(&model,&data,false));
                assert(!epd_clock_refresh_is_valid());
                fail_push=0;fail_refresh=busy_timeout=false;
            }
        }
    }
}

static void test_clock_render_io_failure(void)
{
    epd_driver_t drv = {.diff_push=diff_push, .refresh_partial=refresh,
                        .init_partial=init_partial, .busy_error=busy_error};
    epd_model_t model = {.drv=&drv, .width=800U, .height=480U};
    for (unsigned mode = MODE_CLOCK_CALENDAR; mode <= MODE_CLOCK; ++mode) {
        gui_data_t data = {.mode=mode, .width=800U, .height=480U,
                          .timestamp=12U*3600U+34U*60U};
        clock_reference(&data, old_frame);
        uint32_t previous = data.timestamp;
        data.timestamp += 60U;
        clock_reference(&data, new_frame);
        epd_clock_refresh_mark_valid(previous);
        ui_resources_stub_fail_read(0U);
        pushes = refreshes = 0U;
        memset(pushed, 0, sizeof(pushed));
        assert(epd_clock_refresh_try(&model, &data, false));
        assert(refreshes == 1U);
        unsigned total_reads = ui_resources_stub_read_count();
        assert(total_reads > 10U);
        /* Exercise every geometry/OLD/NEW read, including failures after
         * earlier good strips have already reached the controller's RAM. */
        for (unsigned nth = 1U; nth <= total_reads; ++nth) {
            epd_clock_refresh_mark_valid(previous);
            ui_resources_stub_fail_read(nth);
            pushes = refreshes = 0U;
            memset(pushed, 0, sizeof(pushed));
            assert(!epd_clock_refresh_try(&model, &data, false));
            assert(ui_resources_stub_fault_count() == 1U);
            assert(refreshes == 0U && !epd_clock_refresh_is_valid());
            ui_resources_stub_fail_read(0U);
            /* Failed partial refreshes need a new full-frame baseline. */
            assert(!epd_clock_refresh_try(&model, &data, false));
            epd_clock_refresh_mark_valid(previous);
            pushes = refreshes = 0U;
            memset(pushed, 0, sizeof(pushed));
            assert(epd_clock_refresh_try(&model, &data, false) && refreshes == 1U);
        }
    }
    puts("PASS: every clock geometry/OLD/NEW read fault withholds physical refresh and invalidates baseline");
}

static void test_scene_render_io_failure(void)
{
    static uint8_t healthy[2][48000];
    for (unsigned mode = MODE_CALENDAR; mode <= MODE_CLOCK; ++mode) {
        gui_data_t data = {.mode=mode, .color=2U, .width=800U, .height=480U,
                          .timestamp=1789347600U, .week_start=1U, .temperature=25};
        ui_resources_stub_fail_read(0U);
        pages = reads = 0U;
        memset(frame, 255, sizeof(frame));
        DrawGUI(&data, capture, NULL);
        assert(GUI_RenderValid());
        memcpy(healthy, frame, sizeof(healthy));
        unsigned total_ui_reads = ui_resources_stub_read_count();
        unsigned total_font_reads = reads, total_pages = pages;
        assert(total_ui_reads > 2U && total_font_reads > 2U);
        const unsigned positions[] = {1U, total_ui_reads / 2U, total_ui_reads};
        for (unsigned fault = 0U; fault < 3U; ++fault) {
            pages = 0U;
            ui_resources_stub_fail_read(positions[fault]);
            memset(frame, 255, sizeof(frame));
            DrawGUI(&data, capture, NULL);
            assert(ui_resources_stub_fault_count() == 1U);
            assert(!GUI_RenderValid() && pages < total_pages);
            ui_resources_stub_fail_read(0U);
            memset(frame, 255, sizeof(frame));
            DrawGUI(&data, capture, NULL);
            assert(GUI_RenderValid() && memcmp(healthy, frame, sizeof(frame)) == 0);
        }
        for (unsigned fault = 0U; fault < 2U; ++fault) {
            pages = 0U;
            unsigned failures_before = failed_font_reads;
            fail_font_reads = UINT_MAX;
            font_reads_until_fault = fault ? total_font_reads / 2U : 0U;
            memset(frame, 255, sizeof(frame));
            DrawGUI(&data, capture, NULL);
            assert(failed_font_reads == failures_before + 1U);
            assert(!GUI_RenderValid() && pages < total_pages);
            fail_font_reads = font_reads_until_fault = 0U;
            memset(frame, 255, sizeof(frame));
            DrawGUI(&data, capture, NULL);
            assert(GUI_RenderValid() && memcmp(healthy, frame, sizeof(frame)) == 0);
        }
    }
    puts("PASS: calendar/clock scene faults stop page output and recover with byte-exact frames");
}

static void test_scene_initialization_io_failure(void)
{
    gui_data_t data = {.mode=MODE_CALENDAR, .color=2U, .width=800U, .height=480U,
                      .timestamp=1789347600U, .week_start=1U};
    const unsigned positions[] = {1U, 2U, 8U};
    for (unsigned i = 0U; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        ui_resources_invalidate();
        ui_resources_stub_fail_read(positions[i]);
        pages = 0U;
        DrawGUI(&data, capture, NULL);
        /* A transient initialization fault must not commit a NO DATA screen.
         * The next frame retries failed initialization without a resource
         * rewrite or an explicit activation command. */
        assert(ui_resources_stub_fault_count() == 1U);
        assert(!GUI_RenderValid() && pages == 0U);
        ui_resources_stub_fail_read(0U);
        DrawGUI(&data, capture, NULL);
        assert(GUI_RenderValid() && pages != 0U);
    }
    puts("PASS: failed UI initialization withholds NO DATA and retries successfully next frame");
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

static bool frame_pixel(const unsigned char *plane, int x, int y)
{
    assert(x >= 0 && x < 800 && y >= 0 && y < 480);
    return (plane[y * 100 + x / 8] & (0x80U >> (x & 7))) != 0;
}

static void todo_fixture_put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8U));
}

static void install_todos(uint8_t count, uint8_t limit, uint8_t font_size, uint32_t now)
{
    uint8_t blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_MAX_VISIBLE * TODO_ITEMS_RECORD_SIZE + 64U] = {0};
    assert(count <= TODO_ITEMS_MAX_VISIBLE);
    todo_fixture_put32(blob, TODO_ITEMS_MAGIC);
    blob[4] = TODO_ITEMS_VERSION;
    blob[5] = TODO_ITEMS_RECORD_SIZE;
    blob[6] = count;
    blob[8] = limit;
    blob[9] = font_size;
    if (use_todo_font) {
        blob[4] = TODO_ITEMS_CUSTOM_VERSION;
        blob[10] = GUI_FONT_TODO;
        blob[11] = 13U;
        todo_fixture_put32(blob + 12, TODO_FIXTURE_FONT_KEY);
        memcpy(blob + TODO_ITEMS_HEADER_SIZE + count * TODO_ITEMS_RECORD_SIZE, "FixtureCustom", 13U);
    }
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t *record = blob + TODO_ITEMS_HEADER_SIZE + i * TODO_ITEMS_RECORD_SIZE;
        todo_fixture_put32(record, i + 1U);
        if (i == 0U) record[12] = TODO_ITEM_PERMANENT;
        else {
            todo_fixture_put32(record + 4, now - (i + 1U) * 60U);
            todo_fixture_put32(record + 8, now + (i + 1U) * 3600U);
        }
        /* A full 63-byte UTF-8 title and a 64-byte ASCII title exercise glyph
         * boundaries, wrapping/ellipsis, and the maximum record text size. */
        if ((i & 1U) == 0U) {
            record[13] = 63U;
            for (unsigned word = 0; word < 21U; ++word) {
                record[14 + word * 3U] = 0xE6U;
                record[15 + word * 3U] = 0x97U;
                record[16 + word * 3U] = 0xA5U; /* U+65E5, present in the lunar fonts. */
            }
        } else {
            record[13] = TODO_ITEMS_TEXT_MAX;
            for (unsigned letter = 0; letter < TODO_ITEMS_TEXT_MAX; ++letter)
                record[14 + letter] = (uint8_t)('A' + (letter + i) % 26U);
        }
    }
    todo_resources_stub_load(blob, TODO_ITEMS_HEADER_SIZE + count * TODO_ITEMS_RECORD_SIZE + (use_todo_font ? 13U : 0U));
    assert(todo_items_init());
    assert(todo_items_ready() && todo_items_count() == count);
}

static void install_todo_title(const char *title, uint8_t font, uint8_t limit)
{
    uint8_t blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE] = {0};
    size_t length = strlen(title);
    assert(length && length <= TODO_ITEMS_TEXT_MAX);
    todo_fixture_put32(blob, TODO_ITEMS_MAGIC);
    blob[4] = TODO_ITEMS_VERSION; blob[5] = TODO_ITEMS_RECORD_SIZE;
    blob[6] = 1U; blob[8] = limit; blob[9] = font;
    todo_fixture_put32(blob + 16, 1U);
    blob[28] = TODO_ITEM_PERMANENT; blob[29] = (uint8_t)length;
    memcpy(blob + 30, title, length);
    todo_resources_stub_load(blob, sizeof(blob));
    assert(todo_items_init());
}

static void test_todo_custom_pane_bounds(void)
{
    static unsigned char empty[2][48000],actual[2][48000],text[48000];
    ui_component_t saved=*ui_resource_component(UI_COMP_TODO_SPLIT),pane=saved;
    gui_data_t data={.mode=MODE_TODO_CALENDAR,.color=2,.width=800,.height=480,
                    .timestamp=1789347600U,.week_start=1};
    const char *title="AAAI";
    Adafruit_GFX gfx;GFX_begin(&gfx,800,480,0);
    GFX_setExternalFont(&gfx,&sources[GUI_FONT_WQY12]);
    GFX_setFontPlaceholder(&gfx,&font_slot_geometry[GUI_FONT_WQY12]);
    int16_t width=GFX_getUTF8Width(&gfx,title);pane.w=14+width;
    ui_resources_stub_set_component(UI_COMP_TODO_SPLIT,&pane);
    install_todo_title(title,12U,8U);memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);
    assert(GUI_TodoRenderValid());memcpy(actual,frame,sizeof(frame));
    memset(frame,255,sizeof(frame));GFX_setFontMode(&gfx,1);GFX_setTextColor(&gfx,GFX_BLACK,GFX_WHITE);
    GFX_firstPage(&gfx);do {GFX_fillScreen(&gfx,GFX_WHITE);GFX_drawUTF8(&gfx,37,77,title);}
    while(GFX_nextPage(&gfx,capture,NULL));GFX_end(&gfx);memcpy(text,frame[0],sizeof(text));
    for(int y=62;y<87;y++)for(int x=37;x<37+width;x++)
        assert(frame_pixel(actual[0],x,y)==frame_pixel(text,x,y));
    pane.w=60;ui_resources_stub_set_component(UI_COMP_TODO_SPLIT,&pane);
    for(unsigned missing=0;missing<2;missing++) {
        missing_fonts=missing!=0;
        install_todos(0,5,12,data.timestamp);memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);memcpy(empty,frame,sizeof(frame));
        install_todos(8,8,12,data.timestamp);memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);assert(GUI_TodoRenderValid());
        for(int y=pane.y;y<pane.y+pane.h;y++)for(int x=pane.x+pane.w;x<800;x++)for(unsigned plane=0;plane<2;plane++)
            assert(frame_pixel(frame[plane],x,y)==frame_pixel(empty[plane],x,y));
    }
    missing_fonts=false;pane.h=1;ui_resources_stub_set_component(UI_COMP_TODO_SPLIT,&pane);
    install_todos(8,8,12,data.timestamp);memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);
    assert(!GUI_TodoRenderValid()&&!todo_items_ready());
    ui_resources_stub_set_component(UI_COMP_TODO_SPLIT,&saved);assert(todo_items_init());
    memset(frame,255,sizeof(frame));DrawGUI(&data,capture,NULL);assert(GUI_TodoRenderValid());
    puts("PASS: resource-owned TODO rectangle, exact-fit text, clipping and insufficient-height rejection");
}

static void test_todo_header(void)
{
    static uint8_t actual[48000], expected[48000];
    static const int8_t temperatures[] = {-128, 25};
    static const char *labels[] = {"-128", "25"};
    const uint32_t now = 1789347600U;
    ui_component_t saved_top = *ui_resource_component(UI_COMP_SPLIT_FRAME_TOP);
    for (unsigned full = 0; full < 2; ++full) {
        gui_data_t data = {.mode=full ? MODE_TODO : MODE_TODO_CALENDAR, .color=2, .width=800, .height=480,
                          .timestamp=now, .week_start=1};
        install_todos(8U, 1U, 12U, now);
        todo_selection_t selected;
        assert(todo_items_select(now, &selected) && selected.count == 1U && selected.daily_count == 8U);
        int right = 775;
        for (unsigned t = 0; t < 2; ++t) {
            data.temperature = temperatures[t];
            memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
            assert(GUI_TodoRenderValid()); memcpy(actual, frame[0], sizeof(actual));
            /* Measure independently rasterized temperature pixels at zero. */
            Adafruit_GFX gfx; GFX_begin(&gfx, 800, 480, 0);
            GFX_setFontMode(&gfx, 1U); GFX_setTextColor(&gfx, GFX_BLACK, GFX_WHITE);
            memset(frame, 255, sizeof(frame)); GFX_firstPage(&gfx);
            int advance = 0,unit_advance=0;
            do {
                GFX_fillScreen(&gfx, GFX_WHITE);
                GFX_setExternalFont(&gfx, &sources[GUI_FONT_HELVB18]);
                advance = GFX_drawUTF8(&gfx, 0, 36, labels[t]);
                GFX_setExternalFont(&gfx, &sources[GUI_FONT_WQY12]);
                unit_advance=GFX_drawUTF8(&gfx, advance, 36, "℃");
            }
            while (GFX_nextPage(&gfx, capture, NULL));
            int width = 0;
            for (int y = 12; y < 45; ++y)
                for (int x = 0; x < 100; ++x)
                    if (!frame_pixel(frame[0], x, y) && x + 1 > width) width = x + 1;
            assert(width > 0);
            memset(frame, 255, sizeof(frame)); GFX_firstPage(&gfx);
            do {
                GFX_fillScreen(&gfx, GFX_WHITE);
                GFX_setExternalFont(&gfx, &sources[GUI_FONT_WQY9]);
                GFX_drawUTF8(&gfx, 23, 36, "TODO 1/8");
                GFX_setExternalFont(&gfx, &sources[GUI_FONT_HELVB18]);
                GFX_drawUTF8(&gfx, right - advance - unit_advance, 36, labels[t]);
                GFX_setExternalFont(&gfx, &sources[GUI_FONT_WQY12]);
                GFX_drawUTF8(&gfx, right - unit_advance, 36, "℃");
            } while (GFX_nextPage(&gfx, capture, NULL));
            GFX_end(&gfx); memcpy(expected, frame[0], sizeof(expected));
            for (int y = 12; y < 45; ++y)
                for (int x = 23; x < right; ++x) {
                    if (frame_pixel(actual, x, y) != frame_pixel(expected, x, y))
                        fprintf(stderr, "TODO header mode=%u temp=%d x=%d y=%d width=%d actual=%d expected=%d\n",
                                data.mode, data.temperature, x, y, width,
                                frame_pixel(actual, x, y), frame_pixel(expected, x, y));
                    assert(frame_pixel(actual, x, y) == frame_pixel(expected, x, y));
                }
            for (int y = saved_top.y; y < saved_top.y + saved_top.h; ++y)
                for (int x = saved_top.x; x < saved_top.x + saved_top.w; ++x)
                    assert(!frame_pixel(actual, x, y));
            for (int x = 23; x < 367; ++x) assert(frame_pixel(actual, x, 50));
        }
        ui_component_t moved_top = saved_top;
        moved_top.y = 48; moved_top.h = 3;
        ui_resources_stub_set_component(UI_COMP_SPLIT_FRAME_TOP, &moved_top);
        memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
        for (int y = 48; y < 51; ++y)
            for (int x = moved_top.x; x < moved_top.x + moved_top.w; ++x)
                assert(!frame_pixel(frame[0], x, y));
        ui_resources_stub_set_component(UI_COMP_SPLIT_FRAME_TOP, &saved_top);
    }
    puts("PASS: TODO visible/daily total independent of limit, calendar-sized signed temperature and connected frame");
}

static void test_todo_font_io_failure_does_not_commit_frame(void)
{
    const char *title = "\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5";
    for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; ++mode) {
        gui_data_t data = {.mode=mode, .color=2U, .width=800U, .height=480U,
                          .timestamp=1789347600U, .week_start=1U, .temperature=25};
        install_todo_title(title,12U,5U);
        unsigned warmup=reads;DrawGUI(&data,capture,NULL);
        unsigned late_budget=(reads-warmup)/2U;if(!late_budget)late_budget=1;
        for (unsigned fault = 0U; fault < 3U; ++fault) {
            install_todo_title(title, 12U, 5U);
            unsigned failed_before = failed_font_reads, reads_before = reads, pages_before = pages;
            fail_font_reads = fault == 0U ? 3U : UINT_MAX;
            font_reads_until_fault = fault == 2U ? late_budget : 0U;
            memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
            assert(failed_font_reads == failed_before + 1U && !GUI_TodoRenderValid());
            assert(reads == reads_before + (fault == 2U ? late_budget : 0U));
            if (fault != 2U) assert(pages-pages_before <= 2U); /* Blank pages before the first text are harmless; the failed page is withheld. */
            else assert(pages > pages_before && pages - pages_before < 69U);
            assert(todo_items_selection_changed(data.timestamp + 1U));
            fail_font_reads = 0U;
            font_reads_until_fault = 0U;
            memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
            assert(GUI_TodoRenderValid()); /* A new frame clears the sticky fault. */
            todo_items_successful_refresh(data.timestamp);
            assert(!todo_items_selection_changed(data.timestamp + 1U));
        }
        /* A valid font lacking a printable code point uses its normal box. */
        install_todo_title("\xee\x80\x80", 12U, 5U); /* U+E000 is absent. */
        memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
        assert(GUI_TodoRenderValid());
        todo_items_successful_refresh(data.timestamp);
        assert(!todo_items_selection_changed(data.timestamp + 1U));
    }
    puts("PASS: TODO frame rejects early and persistent font I/O faults, recovers next frame, and accepts missing glyphs");
}

static void test_todo_calendar_render(void)
{
    static unsigned char clock_calendar[2][48000];
#ifdef GUI_LAYOUT_REFERENCE
    static unsigned char production[2][48000];
#endif
    const uint32_t now = 1789347600U;
    static const uint8_t counts[] = {0, 1, 8, 8};
    static const uint8_t limits[] = {5, 1, 8, 3};
    unsigned cases = 0;
    calendar_annotations_reset();
    for (unsigned font = 0; font < 2; ++font) {
        for (unsigned absent = 0; absent < 2; ++absent) {
            missing_fonts = absent != 0U;
            for (unsigned color = 1; color <= 2; ++color) {
                gui_data_t data = {.mode=MODE_CLOCK_CALENDAR, .color=color, .width=800, .height=480,
                    .timestamp=now, .week_start=1, .temperature=25, .voltage_mv=3100,
                    .ssid="NRF_EPD"};
                memset(frame, 255, sizeof(frame));
                DrawGUI(&data, capture, NULL);
                memcpy(clock_calendar, frame, sizeof(frame));
                data.mode = MODE_TODO_CALENDAR;
                for (unsigned variant = 0; variant < sizeof(counts); ++variant) {
                    install_todos(counts[variant], limits[variant], font ? 12U : 9U, now);
                    memset(frame, 255, sizeof(frame));
                    pages = 0;
                    DrawGUI(&data, capture, NULL);
                    assert(pages && GUI_TodoRenderValid());
                    /* The entire right calendar stays byte-exact in both
                     * planes, including the clock/calendar frame decoration. */
                    unsigned changed_left = 0, marked_left = 0;
                    for (unsigned y = 0; y < 480; ++y) {
                        for (unsigned plane = 0; plane < 2 && y>=45; ++plane)
                            assert(memcmp(frame[plane] + y * 100U + 49U,
                                          clock_calendar[plane] + y * 100U + 49U, 51U) == 0);
                        if (y < 20U || y >= 450U) continue;
                        for (unsigned byte = 2; byte < 47; ++byte) {
                            marked_left += frame[0][y * 100U + byte] != 255U;
                            changed_left += frame[0][y * 100U + byte] != clock_calendar[0][y * 100U + byte];
                        }
                    }
                    assert(marked_left > 20U && changed_left > 20U);
                    todo_selection_t selection;
                    assert(todo_items_select(now, &selection));
                    assert(selection.count == (counts[variant] < limits[variant]
                        ? counts[variant] : limits[variant]));
                    assert(selection.queued_count == counts[variant] - selection.count);
                    /* Every visible item contributes its own checkbox inside
                     * the former clock region, including all eight rows. */
                    int row_height = 384 / limits[variant];
                    for (unsigned row = 0; row < selection.count; ++row) {
                        int y = 62 + row * row_height;
                        unsigned marker_plane = color > 1 && selection.phase[row] == TODO_PHASE_ACTIVE ? 1U : 0U;
                        assert(!frame_pixel(frame[marker_plane], 23, y + 4));
                        int baseline = y + (row_height >= (font ? 57 : 56) ? 53 : 35);
                        unsigned metadata = 0;
                        for (int py = baseline - 10; py < baseline + 5; ++py)
                            for (int px = 37; px < 160; ++px) metadata += !frame_pixel(frame[0], px, py);
                        assert(metadata == 0);
                    }
                    /* Empty TODO leaves the former date/header/footer and
                     * clock decoration completely blank on the left. */
                    if (variant == 0)
                        for (int y = 110; y < 480; ++y)
                            for (int x = 15; x < 377; ++x)
                                for (unsigned plane = 0; plane < 2; ++plane)
                                    assert(frame_pixel(frame[plane], x, y));
#ifdef GUI_LAYOUT_REFERENCE
                    memcpy(production, frame, sizeof(frame));
                    memset(frame, 255, sizeof(frame));
                    DrawGUI_Unclipped(&data, capture, NULL);
                    assert(GUI_TodoRenderValid_Unclipped());
                    assert(memcmp(production, frame, sizeof(frame)) == 0);
#endif
                    ++cases;
                }
            }
        }
    }
    missing_fonts = false;
    /* An item read failure must prevent a successful scheduling snapshot. */
    gui_data_t data = {.mode=MODE_TODO_CALENDAR, .color=2, .width=800, .height=480, .timestamp=now};
    install_todos(8U, 8U, 12U, now);
    todo_resources_stub_fail_reads(true);
    memset(frame, 255, sizeof(frame));
    DrawGUI(&data, capture, NULL);
    assert(!GUI_TodoRenderValid());
    todo_resources_stub_fail_reads(false);
    memset(frame, 255, sizeof(frame));
    DrawGUI(&data, capture, NULL);
    assert(GUI_TodoRenderValid());
    printf("PASS: %u TODO renders preserve calendar pixels, empty/long titles/eight rows/queue/fonts\n", cases);
}

static void test_todo_calendar_date_rollover(void)
{
    static unsigned char previous[2][48000], todo_frame[2][48000];
    static const uint16_t dates[][3] = {
        {2026, 10, 9}, {2026, 10, 31}, {2026, 12, 31}, {2024, 2, 28},
    };
    calendar_annotations_reset();
    const ui_component_t *layout = ui_resource_component(UI_COMP_SPLIT_GRID);
    calendar_grid_rect_t rect = {layout->x, layout->y, layout->w,
                                (int16_t)(layout->y + layout->h)};
    unsigned cases = 0U;
    for (unsigned date = 0U; date < sizeof(dates) / sizeof(dates[0]); date++) {
        tm_t end = {.tm_year=dates[date][0], .tm_mon=dates[date][1],
                    .tm_mday=dates[date][2], .tm_hour=23, .tm_min=59, .tm_sec=59};
        uint32_t midnight = transformTimeStruct(&end) + 1U;
        for (unsigned resident = 0U; resident < 2U; resident++) {
            install_todos(resident, 5U, 12U, midnight);
            for (unsigned week = 0U; week <= 1U; week++) {
                gui_data_t data = {.mode=MODE_TODO_CALENDAR, .color=2, .width=800,
                    .height=480, .timestamp=midnight-1U, .week_start=week,
                    .temperature=25, .voltage_mv=3100, .ssid="NRF_EPD"};
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(GUI_TodoRenderValid());
                memcpy(previous, frame, sizeof(frame));
                data.timestamp = midnight;
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(GUI_TodoRenderValid());
                memcpy(todo_frame, frame, sizeof(frame));
                unsigned changed_right = 0U;
                for (unsigned y = 0U; y < 480U; y++) {
                    for (unsigned plane = 0U; plane < 2U; plane++) {
                        assert(memcmp(previous[plane] + y*100U,
                                      frame[plane] + y*100U, 48U) == 0);
                        for (unsigned x = 49U; x < 100U; x++)
                            changed_right += previous[plane][y*100U+x] != frame[plane][y*100U+x];
                    }
                }
                assert(changed_right > 0U);
                tm_t today;
                transformTime(midnight, &today);
                calendar_grid_input_t input = {
                    get_first_day_week(today.tm_year+YEAR0, today.tm_mon+1U), week,
                    thisMonthMaxDays(today.tm_year+YEAR0, today.tm_mon+1U), 480,
                };
                calendar_grid_metrics_t metrics = calendar_grid_compute(&rect, &input);
                for (unsigned offset = 0U; offset < 2U; offset++) {
                    unsigned day = offset == 0U ? today.tm_mday :
                        today.tm_mday == 1U ? 2U : today.tm_mday-1U;
                    calendar_cell_pos_t cell = calendar_grid_cell(&rect, &metrics, day-1U);
                    /* Circle interior above the date glyph: yesterday's red
                     * background is cleared and only today is highlighted. */
                    int sample_y=cell.by-cell.cr-1;if(sample_y<rect.y)sample_y=rect.y;
                    assert(frame_pixel(frame[1], cell.bx+cell.cr, sample_y) == (offset != 0U));
                }
                data.mode = MODE_CLOCK_CALENDAR;
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                for (unsigned y = 45U; y < 480U; y++)
                    for (unsigned plane = 0U; plane < 2U; plane++)
                        assert(memcmp(todo_frame[plane]+y*100U+49U,
                                      frame[plane]+y*100U+49U, 51U) == 0);
                cases++;
            }
        }
    }
    printf("PASS: %u empty/resident TODO calendars move today's red mark at daily/month/year/leap-day boundaries\n", cases);
}

static void test_fullscreen_todo_render(void)
{
    static unsigned char standard[2][48000];
#ifdef GUI_LAYOUT_REFERENCE
    static unsigned char actual[2][48000];
#endif
    const uint32_t now = 1789347600U;
    static const uint8_t counts[] = {0, 1, 8, 8};
    static const uint8_t limits[] = {5, 1, 8, 3};
    unsigned cases = 0;
    const ui_component_t *top = ui_resource_component(UI_COMP_CLOCK_TOP_LINE);
    const ui_component_t *bottom = ui_resource_component(UI_COMP_CLOCK_BOTTOM_LINE);
    for (unsigned font = 0; font < 2; ++font) {
        for (unsigned absent = 0; absent < 2; ++absent) {
            missing_fonts = absent != 0U;
            for (unsigned color = 1; color <= 2; ++color) {
                gui_data_t data = {.mode=MODE_TODO, .color=color, .width=800, .height=480,
                    .timestamp=now, .week_start=1, .temperature=25, .voltage_mv=3100, .ssid="NRF_EPD"};
                for (unsigned variant = 0; variant < sizeof(counts); ++variant) {
                    install_todos(counts[variant], limits[variant], font ? 12U : 9U, now);
                    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                    assert(GUI_TodoRenderValid());
                    todo_selection_t selection;
                    assert(todo_items_select(now, &selection));
                    /* TODO owns both halves, with only active boxes on the red plane. */
                    unsigned right_title = 0;
                    for (int y = 0; y < 480; ++y) {
                        for (int x = 0; x < 800; ++x) {
                            bool red_box = false;
                            for (unsigned row = 0; row < selection.count; ++row) {
                                int marker_y = 66 + row * (384 / limits[variant]);
                                if (color > 1 && selection.phase[row] == TODO_PHASE_ACTIVE &&
                                    x >= 23 && x < 30 && y >= marker_y && y < marker_y + 7 &&
                                    (x == 23 || x == 29 || y == marker_y || y == marker_y + 6)) red_box = true;
                            }
                            assert(frame_pixel(frame[1], x, y) == !red_box);
                            const ui_component_t *line = ui_resource_component(UI_COMP_SPLIT_FRAME_TOP);
                            bool frame_line = y >= line->y && y < line->y + line->h && x >= line->x && x < line->x + line->w;
                            if (!frame_line && (y < 12 || y >= 450 || x < 23 || x >= 775))
                                assert(frame_pixel(frame[0], x, y));
                            if (y > 110 && x > 400) right_title += !frame_pixel(frame[0], x, y);
                            if (variant == 0 && y > 110) assert(frame_pixel(frame[0], x, y));
                        }
                    }
                    assert(selection.count == (counts[variant] < limits[variant] ? counts[variant] : limits[variant]));
                    assert(selection.queued_count == counts[variant] - selection.count);
                    int row_height = 384 / limits[variant];
                    for (unsigned row = 0; row < selection.count; ++row) {
                        int y = 62 + row * row_height;
                        unsigned marker_plane = color > 1 && selection.phase[row] == TODO_PHASE_ACTIVE ? 1U : 0U;
                        assert(!frame_pixel(frame[marker_plane], 23, y + 4));
                        /* No deadline or PIN text remains below the title. */
                        assert(row_height >= 35);
                        unsigned metadata = 0;
                        int baseline = y + (row_height >= (font ? 57 : 56) ? 53 : 35);
                        for (int py = baseline - 10; py < baseline + 5; ++py)
                            for (int px = 37; px < 160; ++px) metadata += !frame_pixel(frame[0], px, py);
                        assert(metadata == 0);
                    }
                    if (counts[variant] == 8 && limits[variant] == 8) assert(right_title > 20);
#ifdef GUI_LAYOUT_REFERENCE
                    memcpy(actual, frame, sizeof(frame));
                    memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
                    assert(GUI_TodoRenderValid_Unclipped());
                    assert(memcmp(actual, frame, sizeof(frame)) == 0);
#endif
                    ++cases;
                }
            }
        }
    }
    missing_fonts = false;
    gui_data_t data = {.mode=MODE_TODO, .color=2, .width=800, .height=480,
        .timestamp=now, .week_start=1, .temperature=25, .voltage_mv=3100, .ssid="NRF_EPD"};
    install_todos(8, 8, 12, now);
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    memcpy(standard, frame, sizeof(frame));
    todo_selection_t selected;
    assert(todo_items_select(now, &selected));
    /* Mode switches use the same list and return to an identical full frame. */
    data.mode = MODE_TODO_CALENDAR;
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    todo_selection_t split;
    assert(todo_items_select(now, &split) && memcmp(&selected, &split, sizeof(split)) == 0);
    data.mode = MODE_TODO;
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    assert(memcmp(standard, frame, sizeof(frame)) == 0);

    ui_component_t saved_top = *top, saved_bottom = *bottom;
    ui_component_t narrow = saved_bottom;
    narrow.y = saved_top.y + 60;
    ui_resources_stub_set_component(UI_COMP_CLOCK_BOTTOM_LINE, &narrow);
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    assert(GUI_TodoRenderValid() && memcmp(standard, frame, sizeof(frame)) == 0);
#ifdef GUI_LAYOUT_REFERENCE
    memcpy(actual, frame, sizeof(frame));
    memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
    assert(memcmp(actual, frame, sizeof(frame)) == 0);
#endif
    ui_resources_stub_set_component(UI_COMP_CLOCK_TOP_LINE, &saved_top);
    ui_resources_stub_set_component(UI_COMP_CLOCK_BOTTOM_LINE, &saved_bottom);
    todo_resources_stub_fail_reads(true);
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    assert(!GUI_TodoRenderValid());
    todo_resources_stub_fail_reads(false);
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    assert(GUI_TodoRenderValid() && memcmp(standard, frame, sizeof(frame)) == 0);
    printf("PASS: %u full-screen TODO renders, both halves/no deadline text/queue/mode switching/failure fallback\n", cases);
}

static void install_single_dot(unsigned month, unsigned day, bool corner)
{
    uint8_t blob[CALENDAR_ANNOTATIONS_HEADER_SIZE + CALENDAR_ANNOTATIONS_RECORD_SIZE] = {0};
    uint32_t magic = CALENDAR_ANNOTATIONS_MAGIC;
    for (unsigned i = 0; i < 4; ++i) blob[i] = (uint8_t)(magic >> (i * 8));
    blob[4] = CALENDAR_ANNOTATIONS_VERSION;
    blob[5] = CALENDAR_ANNOTATIONS_RECORD_SIZE;
    blob[6] = 1;
    blob[16] = (uint8_t)2026;
    blob[17] = 2026 >> 8;
    blob[18] = month;
    blob[19] = day;
    blob[20] = 1U | (corner ? CALENDAR_ANNOTATION_POS_CORNER_FLAG : 0U);
    assert(calendar_annotations_load(blob, sizeof(blob)));
}

static void test_todo_custom_fonts(void)
{
    static unsigned char standard[2][48000];
#ifdef GUI_LAYOUT_REFERENCE
    static unsigned char actual[2][48000];
#endif
    const uint32_t now = 1789347600U;
    unsigned cases = 0;
    for (unsigned size = 9; size <= 12; size += 3) {
        todo_font_size = (uint8_t)size;
        todo_font_key = TODO_FIXTURE_FONT_KEY;
        for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; ++mode) {
            gui_data_t data = {.mode=mode, .color=2, .width=800, .height=480,
                              .timestamp=now, .week_start=1};
            use_todo_font = false;
            install_todos(8, 8, (uint8_t)size, now);
            memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
            assert(GUI_TodoRenderValid());
            memcpy(standard, frame, sizeof(frame));
            use_todo_font = true;
            install_todos(8, 8, (uint8_t)size, now);
            assert(todo_items_font_slot() == GUI_FONT_TODO && todo_items_font_key() == TODO_FIXTURE_FONT_KEY);
            memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
            assert(GUI_TodoRenderValid() && memcmp(standard, frame, sizeof(frame)) == 0);
#ifdef GUI_LAYOUT_REFERENCE
            memcpy(actual, frame, sizeof(frame));
            memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
            assert(GUI_TodoRenderValid_Unclipped() && memcmp(actual, frame, sizeof(frame)) == 0);
#endif
            for (unsigned fault = 0; fault < 3; ++fault) {
                install_todos(8, 8, (uint8_t)size, now);
                todo_items_successful_refresh(now);
                assert(!todo_items_selection_changed(now));
                missing_todo_font = fault == 0;
                todo_font_size = fault == 1 ? (size == 9U ? 12U : 9U) : (uint8_t)size;
                todo_font_key = fault == 2 ? TODO_FIXTURE_FONT_KEY + 1U : TODO_FIXTURE_FONT_KEY;
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(!GUI_TodoRenderValid() && !todo_items_ready());
                assert(todo_items_selection_changed(now));
#ifdef GUI_LAYOUT_REFERENCE
                memcpy(actual, frame, sizeof(frame));
                memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
                /* Culling can submit earlier valid pages before discovering the
                 * unavailable font. Both attempts must reject the frame;
                 * uncommitted partial RAM is not a displayed image. */
                assert(!GUI_TodoRenderValid_Unclipped() && !todo_items_ready());
#endif
                missing_todo_font = false;
                todo_font_size = (uint8_t)size;
                todo_font_key = TODO_FIXTURE_FONT_KEY;
                assert(todo_items_init());
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(GUI_TodoRenderValid() && todo_items_ready());
                assert(memcmp(standard, frame, sizeof(frame)) == 0);
                todo_items_successful_refresh(now);
                assert(!todo_items_selection_changed(now));
            }
            ++cases;
        }
    }
    /* Empty custom lists also require the declared font dependency. */
    gui_data_t data = {.mode=MODE_TODO, .color=2, .width=800, .height=480, .timestamp=now};
    install_todos(0, 5, 12, now);
    missing_todo_font = true;
    memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
    assert(!GUI_TodoRenderValid() && !todo_items_ready());
    missing_todo_font = false;
    use_todo_font = false;
    install_todos(8, 8, 12, now);
    printf("PASS: %u custom TODO font renders, missing/size/key suspension and resource recovery\n", cases);
}

static void fixture_bits(uint8_t *bytes, unsigned *bit, unsigned value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i, ++*bit)
        bytes[*bit / 8U] |= (uint8_t)(((value >> i) & 1U) << (*bit % 8U));
}

/* A dense glyph exercises the complete advertised bounding box, including
 * negative italic bearings, rather than relying on a font's Latin ascent. */
static void extended_fixture_font(uint8_t size)
{
    int px = (size * 4 + 2) / 3, desc = (px + 2) / 3, overhang = (px + 3) / 4;
    extended_todo_metrics = (gui_font_metrics_t){px + overhang, px + desc, -overhang,
        -desc, px, -desc, px, -desc, px + 1};
    uint8_t *bytes = fonts[3];
    memset(bytes, 0, sizeof(fonts[3]));
    const uint8_t bits[] = {4, 4, 7, 7, 8, 8, 8};
    bytes[0] = 1U; memcpy(bytes + 2, bits, sizeof(bits));
    memcpy(bytes + 9, &extended_todo_metrics, 8U);
    bytes[22] = 2U; /* two-byte empty ASCII table */
    bytes[26] = 4U; bytes[27] = bytes[28] = 255U;
    bytes[29] = 0x65U; bytes[30] = 0xE5U; /* 日 */
    unsigned bit = 32U * 8U;
    fixture_bits(bytes, &bit, px + overhang, 7U);
    fixture_bits(bytes, &bit, px + desc, 7U);
    fixture_bits(bytes, &bit, 128 - overhang, 8U);
    fixture_bits(bytes, &bit, 128 - desc, 8U);
    fixture_bits(bytes, &bit, 128 + px + 1, 8U);
    unsigned pixels = (px + overhang) * (px + desc);
    while (pixels != 0U) {
        unsigned run = pixels > 15U ? 15U : pixels;
        fixture_bits(bytes, &bit, 0U, 4U);
        fixture_bits(bytes, &bit, run, 4U);
        fixture_bits(bytes, &bit, 0U, 1U);
        pixels -= run;
    }
    unsigned end = (bit + 7U) / 8U;
    assert(end - 29U <= 255U);
    bytes[31] = end - 29U;
    sources[3] = (u8g2_font_source_t){NULL, end + 2U, read_font, (const void *)3U, size};
    todo_font_size = size; todo_font_key = TODO_FIXTURE_FONT_KEY;
    extended_todo_fixture = true;
}

static void extended_fixture_tasks(uint8_t count, uint8_t limit, uint8_t size)
{
    uint8_t blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_MAX_VISIBLE * TODO_ITEMS_RECORD_SIZE + 1U] = {0};
    todo_fixture_put32(blob, TODO_ITEMS_MAGIC);
    blob[4] = 3U; blob[5] = TODO_ITEMS_RECORD_SIZE; blob[6] = count;
    blob[8] = limit; blob[9] = size; blob[10] = GUI_FONT_TODO;
    todo_fixture_put32(blob + 12U, TODO_FIXTURE_FONT_KEY);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *record = blob + TODO_ITEMS_HEADER_SIZE + i * TODO_ITEMS_RECORD_SIZE;
        todo_fixture_put32(record, i + 1U); record[12] = TODO_ITEM_PERMANENT;
        record[13] = 3U; memcpy(record + 14U, "日", 3U);
    }
    blob[TODO_ITEMS_HEADER_SIZE + count * TODO_ITEMS_RECORD_SIZE] = 3U; /* bold italic */
    todo_resources_stub_load(blob, TODO_ITEMS_HEADER_SIZE + count * TODO_ITEMS_RECORD_SIZE + 1U);
    assert(todo_items_init() && todo_items_extended_font() && todo_items_count() == count);
}

static void test_todo_extended_fonts(void)
{
    static uint8_t actual[2][48000], empty[2][48000];
    const uint8_t sizes[] = {9, 12, 16, 20, 24, 32};
    unsigned cases = 0;
    for (unsigned s = 0; s < sizeof(sizes); ++s) {
        extended_fixture_font(sizes[s]);
        int h = extended_todo_metrics.max_char_height, w = extended_todo_metrics.max_char_width;
        uint8_t max_count = 384 / (h + 6);
        if (max_count > 8U) max_count = 8U;
        for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; ++mode) {
            gui_data_t data = {.mode=mode, .color=2, .width=800, .height=480,
                              .timestamp=1789347600U, .week_start=1, .temperature=-5};
            for (unsigned crowded = 0; crowded < 2; ++crowded) {
                uint8_t count = crowded ? max_count : 1U;
                extended_fixture_tasks(0U, count, sizes[s]);
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                memcpy(empty, frame, sizeof(frame));
                extended_fixture_tasks(count, count, sizes[s]);
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(GUI_TodoRenderValid()); memcpy(actual, frame, sizeof(frame));
                int row_height = 384 / count;
                for (unsigned row = 0; row < count; ++row) {
                    int top = 65 + row * row_height;
                    for (int y = top; y < top + h; ++y)
                        for (int x = 37; x < 37 + w; ++x)
                            assert(!frame_pixel(actual[0], x, y));
                    for (int y = top + h; y < 62 + (row + 1) * row_height; ++y)
                        for (int x = 37; x < 37 + w; ++x) {
                            /* Auxiliary text is allowed only when its own line fits. */
                            if (row_height < h + 19) assert(frame_pixel(actual[0], x, y));
                        }
                }
                for (int y = 62; y < 450; ++y) {
                    for (int x = 0; x < 23; ++x) assert(frame_pixel(actual[0], x, y));
                    if (mode == MODE_TODO_CALENDAR)
                        assert(memcmp(actual[0] + y * 100 + 49, empty[0] + y * 100 + 49, 51) == 0);
                }
#ifdef GUI_LAYOUT_REFERENCE
                memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
                assert(GUI_TodoRenderValid_Unclipped() && memcmp(actual, frame, sizeof(frame)) == 0);
#endif
                ++cases;
            }
        }
    }
    extended_todo_fixture = false;
    puts("PASS: larger TODO glyphs preserve full negative bearings, row bounds and calendar");
    assert(cases == 24U);
}

static void test_todo_phase_render(void)
{
    static uint8_t active[2][48000], expired[2][48000];
    const uint32_t start = 1789347600U, end = start + 1200U;
    const uint32_t times[] = {start - 601U, start - 600U, start, end, end + 299U, end + 300U};
    const uint8_t phases[] = {TODO_PHASE_HIDDEN, TODO_PHASE_UPCOMING, TODO_PHASE_ACTIVE,
        TODO_PHASE_EXPIRED, TODO_PHASE_EXPIRED, TODO_PHASE_HIDDEN};
    uint8_t blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE + TODO_ITEMS_TIMING_TAIL_SIZE] = {0};
    todo_fixture_put32(blob, TODO_ITEMS_MAGIC);
    blob[4] = TODO_ITEMS_TIMING_VERSION; blob[5] = TODO_ITEMS_RECORD_SIZE;
    blob[6] = 1U; blob[8] = 5U; blob[9] = 9U;
    uint8_t *record = blob + TODO_ITEMS_HEADER_SIZE;
    todo_fixture_put32(record, 1U); todo_fixture_put32(record + 4U, start);
    todo_fixture_put32(record + 8U, end); record[13] = 64U;
    for (unsigned i = 0U; i < 64U; ++i) record[14U + i] = 'A' + i % 26U;
    blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE + 1U] = 10U;
    blob[TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_RECORD_SIZE + 3U] = 5U;
    for (unsigned mode = MODE_TODO_CALENDAR; mode <= MODE_TODO; ++mode) {
        for (unsigned color = 1; color <= 2; ++color) {
            todo_resources_stub_load(blob, sizeof(blob)); assert(todo_items_init());
            gui_data_t data = {.mode=mode, .color=color, .width=800, .height=480,
                              .week_start=1, .temperature=25};
            for (unsigned t = 0U; t < sizeof(phases); ++t) {
                data.timestamp = times[t];
                memset(frame, 255, sizeof(frame)); DrawGUI(&data, capture, NULL);
                assert(GUI_TodoRenderValid() && todo_items_phase(0U, times[t]) == phases[t]);
                bool visible = phases[t] != TODO_PHASE_HIDDEN;
                bool red = color == 2U && (phases[t] == TODO_PHASE_ACTIVE || phases[t] == TODO_PHASE_EXPIRED);
                assert(frame_pixel(frame[0], 23, 66) == (!visible || red));
                assert(frame_pixel(frame[1], 23, 66) == !red);
                assert(frame_pixel(frame[0], 24, 67) && frame_pixel(frame[1], 24, 67)); /* empty box */
                if (t == 2U) memcpy(active, frame, sizeof(active));
                if (t == 3U) {
                    memcpy(expired, frame, sizeof(expired));
                    int right = mode == MODE_TODO ? 775 : 367;
                    /* Each wrapped line is crossed at half the base-font ascent. */
                    for (unsigned line = 0; line < (mode == MODE_TODO_CALENDAR ? 2U : 1U); ++line) {
                        int baseline = line == 0U ? 77 : 95, edge = 37;
                        for (int y = baseline - 10; y < baseline + 5; ++y)
                            for (int x = 37; x < right; ++x)
                                if (!frame_pixel(active[0], x, y) && x + 1 > edge) edge = x + 1;
                        assert(edge > 37);
                        for (int x = 37; x < edge; ++x) {
                            if (frame_pixel(expired[0], x, baseline - 4))
                                fprintf(stderr, "strike mode=%u color=%u line=%u x=%d edge=%d y=%d\n", mode, color, line, x, edge, baseline - 4);
                            assert(!frame_pixel(expired[0], x, baseline - 4));
                        }
                    }
                    unsigned added = 0U;
                    for (int y = 62; y < 138; ++y)
                        for (int x = 37; x < right; ++x) {
                            assert(frame_pixel(expired[1], x, y));
                            if (frame_pixel(active[0], x, y) && !frame_pixel(expired[0], x, y)) {
                                assert(y == 73 || y == 91); ++added;
                            }
                        }
                    assert(added > 0U);
                }
                for (int y = 102; y < 138; ++y)
                    for (int x = 37; x < 160; ++x) assert(frame_pixel(frame[0], x, y));
#ifdef GUI_LAYOUT_REFERENCE
                static uint8_t actual[2][48000]; memcpy(actual, frame, sizeof(actual));
                memset(frame, 255, sizeof(frame)); DrawGUI_Unclipped(&data, capture, NULL);
                assert(GUI_TodoRenderValid_Unclipped() && memcmp(actual, frame, sizeof(frame)) == 0);
#endif
            }
        }
    }
    puts("PASS: exact upcoming/active/retained-expired render, red/BW boxes and both strike lines without deadline text");
}

static void test_annotation_dots_preserve_holiday_and_lunar_pixels(void)
{
    static unsigned char unannotated[2][48000];
#ifdef GUI_LAYOUT_REFERENCE
    static unsigned char annotated[2][48000];
#endif
    /* Both holiday kinds, a six-row May, last holiday rows, and a normal day. */
    static const unsigned dates[][2] = {
        {1, 1}, {1, 4}, {5, 1}, {5, 9}, {10, 7}, {10, 10}, {5, 6},
    };
    static const uint8_t week_starts[] = {0, 1, 6};
    unsigned cases = 0;
    for (unsigned date = 0; date < sizeof(dates) / sizeof(dates[0]); ++date) {
        unsigned month = dates[date][0], day = dates[date][1];
        bool work = false;
        bool holiday = calendar_resource_holiday(2026, month, day, &work);
        for (unsigned mode = MODE_CALENDAR; mode <= MODE_CLOCK_CALENDAR; ++mode) {
            const ui_component_t *layout = ui_resource_component(mode == MODE_CALENDAR
                ? UI_COMP_CAL_GRID : UI_COMP_SPLIT_GRID);
            calendar_grid_rect_t rect = {layout->x, layout->y, layout->w,
                                        (int16_t)(layout->y + layout->h)};
            for (unsigned week = 0; week < sizeof(week_starts); ++week) {
                calendar_grid_input_t input = {
                    get_first_day_week(2026, month), week_starts[week],
                    thisMonthMaxDays((uint8_t)2026, month), 480,
                };
                calendar_grid_metrics_t metrics = calendar_grid_compute(&rect, &input);
                int16_t dot_x, dot_y;
                uint8_t dot_r;
                calendar_annotation_dot_center(&rect, &metrics, day - 1, holiday,
                                               &dot_x, &dot_y, &dot_r);
                for (unsigned today = 0; today < 2; ++today) {
                    tm_t time = {.tm_year=2026, .tm_mon=month,
                        .tm_mday=today ? day : day == 1 ? 2 : day - 1};
                    gui_data_t data = {.mode=mode, .color=2, .width=800, .height=480,
                        .timestamp=transformTimeStruct(&time), .week_start=week_starts[week]};
                    calendar_annotations_reset();
                    memset(frame, 255, sizeof(frame));
                    DrawGUI(&data, capture, NULL);
                    memcpy(unannotated, frame, sizeof(frame));
                    for (unsigned corner = 0; corner < 2; ++corner) {
                        install_single_dot(month, day, corner != 0);
                        memset(frame, 255, sizeof(frame));
                        DrawGUI(&data, capture, NULL);
#ifdef GUI_LAYOUT_REFERENCE
                        memcpy(annotated, frame, sizeof(frame));
                        memset(frame, 255, sizeof(frame));
                        DrawGUI_Unclipped(&data, capture, NULL);
                        assert(memcmp(annotated, frame, sizeof(frame)) == 0);
#endif
                        /* Check a full red dot, including all four extreme pixels. */
                        static const int8_t axes[][2] = {{0,0}, {-1,0}, {1,0}, {0,-1}, {0,1}};
                        for (unsigned axis = 0; axis < 5; ++axis) {
                            int x = dot_x + axes[axis][0] * dot_r;
                            int y = dot_y + axes[axis][1] * dot_r;
                            assert(frame_pixel(frame[0], x, y));
                            assert(!frame_pixel(frame[1], x, y));
                        }
                        if (today) {
                            for (unsigned axis = 1; axis < 5; ++axis) {
                                int x = dot_x + axes[axis][0] * (dot_r + 1);
                                int y = dot_y + axes[axis][1] * (dot_r + 1);
                                assert(frame_pixel(frame[0], x, y));
                                assert(frame_pixel(frame[1], x, y));
                            }
                        }
                        int extent = dot_r + (today ? 1 : 0);
                        /* The holiday glyph/circle, lunar text, other dates and pages
                         * must remain byte-for-byte unchanged outside this dot. */
                        for (int y = 0; y < 480; ++y) {
                            for (int x = 0; x < 800; ++x) {
                                if (x >= dot_x - extent && x <= dot_x + extent &&
                                    y >= dot_y - extent && y <= dot_y + extent) continue;
                                for (unsigned plane = 0; plane < 2; ++plane)
                                    assert(frame_pixel(frame[plane], x, y) ==
                                           frame_pixel(unannotated[plane], x, y));
                            }
                        }
                        ++cases;
                    }
                }
            }
        }
    }
    calendar_annotations_reset();
    printf("PASS: %u full-frame dot/holiday/today/lunar preservation cases\n", cases);
}
static void test_calendar_edges(void) {
#ifdef GUI_LAYOUT_REFERENCE
    static unsigned char production[2][48000];
#endif
    /* Holiday dots now use the left corner; today's dots have a white halo.
     * All other approved calendar pixels remain unchanged. */
    static const unsigned expected_edges[24]={
        0xdd56dfd1,
        0x8bb122cb,
        0x46ae7d86,
        0x3deed299,
        0xa13f088e,
        0xd8794609,
        0x4366b2a8,
        0x4a6efde9,
        0x08068133,
        0xed7024d8,
        0xfb5b858a,
        0xdc1242bc,
        0xff7a6a56,
        0x2c51a2d7,
        0x604b6def,
        0xdaccb7b5,
        0x40ba5555,
        0xcff9cc31,
        0xcf0fef14,
        0x98917ec7,
        0xf32ef5f9,
        0x2a6f67d3,
        0x07bfef08,
        0x61ebfe2d,
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
#ifdef GUI_LAYOUT_REFERENCE
                memcpy(production, frame, sizeof(frame));
                memset(frame, 255, sizeof(frame));
                DrawGUI_Unclipped(&data, capture, NULL);
                assert(memcmp(production, frame, sizeof(frame)) == 0);
#endif
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
#ifdef GUI_LAYOUT_REFERENCE
void ui_resources_stub_set_component(ui_component_id_t id, const ui_component_t *value);

static void move_text(ui_component_id_t id, int16_t y, int16_t second,
                      uint8_t primary, uint8_t secondary, uint8_t tertiary)
{
    ui_component_t c = *ui_resource_component(id);
    c.y = y;
    c.h = 1; /* Legal groups need not contain their text's two baselines. */
    c.value0 = second;
    c.font_primary = primary;
    c.font_secondary = secondary;
    c.font_tertiary = tertiary;
    ui_resources_stub_set_component(id, &c);
}

static void test_moved_layouts(void)
{
    static unsigned char reference[2][48000];
    ui_component_t saved[UI_COMPONENT_COUNT - 1U];
    for (unsigned id = 1; id < UI_COMPONENT_COUNT; ++id)
        saved[id - 1U] = *ui_resource_component((ui_component_id_t)id);
    for (unsigned variant = 0; variant < 3; ++variant) {
        /* Vary slots as well as baselines. Case 0 reproduces the former clock
         * header bug (text at 100/122 while its line stays at y=68). Other
         * cases cross page edges and put the lower footer far beyond y+h. */
        int16_t y = variant == 0 ? 100 : variant == 1 ? 217 : 14;
        int16_t second = variant == 0 ? 122 : variant == 1 ? 301 : 55;
        uint8_t p = variant == 1 ? GUI_FONT_WQY9 : GUI_FONT_HELVB18;
        uint8_t s = variant == 2 ? GUI_FONT_HELVB18 : GUI_FONT_WQY12;
        uint8_t t = variant == 1 ? GUI_FONT_WQY12 : GUI_FONT_WQY9;
        move_text(UI_COMP_CLOCK_HEADER, y, second, p, s, t);
        move_text(UI_COMP_SPLIT_LEFT_HEADER, y, second, p, s, 0xffU);
        move_text(UI_COMP_CLOCK_BOTTOM_INFO, 329, 477 - (int16_t)variant,
                  s, 0xffU, 0xffU);
        move_text(UI_COMP_SPLIT_BOTTOM_INFO, 329, 477 - (int16_t)variant,
                  s, 0xffU, 0xffU);
        move_text(UI_COMP_CAL_HEADER, y, second, p, s, t);
        move_text(UI_COMP_SPLIT_RIGHT_HEADER, y, second, t, 0xffU, 0xffU);
        /* Move/narrow the TODO pane so long UTF-8 titles wrap across page
         * boundaries and all eight rows exercise independent page culling. */
        ui_component_t todo_top = saved[UI_COMP_SPLIT_LEFT_TOP_LINE - 1U];
        ui_component_t todo_bottom = saved[UI_COMP_SPLIT_LEFT_BOTTOM_LINE - 1U];
        todo_top.y = variant == 0 ? 133 : variant == 1 ? 191 : 215;
        todo_top.w = variant == 1 ? 180 : 240;
        todo_bottom.y = variant == 0 ? 362 : variant == 1 ? 408 : 440;
        ui_resources_stub_set_component(UI_COMP_SPLIT_LEFT_TOP_LINE, &todo_top);
        ui_resources_stub_set_component(UI_COMP_SPLIT_LEFT_BOTTOM_LINE, &todo_bottom);
        for (unsigned id = UI_COMP_CAL_WEEK; id <= UI_COMP_SPLIT_WEEK; id += 6U) {
            ui_component_t c = *ui_resource_component((ui_component_id_t)id);
            c.y = 160 + (int16_t)variant;
            c.h = variant == 1 ? 1 : 32;
            c.font_primary = s;
            ui_resources_stub_set_component((ui_component_id_t)id, &c);
        }
        for (unsigned absent = 0; absent < 2; ++absent) {
            missing_fonts = absent != 0;
            for (unsigned mode = MODE_CALENDAR; mode <= MODE_TODO; ++mode) {
                for (unsigned color = 1; color <= 2; ++color) {
                    gui_data_t data = {.mode=mode, .color=color, .width=800, .height=480,
                        .timestamp=1789347600U, .week_start=6, .temperature=-12,
                        .voltage_mv=2900, .ssid="NRF_EPD_LONG_NAME"};
                    memset(frame, 255, sizeof(frame));
                    DrawGUI_Unclipped(&data, capture, NULL);
                    memcpy(reference, frame, sizeof(reference));
                    memset(frame, 255, sizeof(frame));
                    DrawGUI(&data, capture, NULL);
                    if (memcmp(reference, frame, sizeof(reference)) != 0) {
                        fprintf(stderr, "layout mismatch variant=%u missing=%u mode=%u color=%u\n",
                                variant, absent, mode, color);
                        for (unsigned plane=0; plane<2; ++plane)
                            for (unsigned i=0; i<48000; ++i)
                                if (reference[plane][i] != frame[plane][i]) {
                                    fprintf(stderr, "first difference: plane=%u x=%u y=%u expected=%02x got=%02x\n",
                                            plane,(i%100)*8,i/100,reference[plane][i],frame[plane][i]);
                                    goto mismatch;
                                }
                        mismatch:
                        assert(0);
                    }
                }
            }
        }
    }
    missing_fonts = false;
    for (unsigned id = 1; id < UI_COMPONENT_COUNT; ++id)
        ui_resources_stub_set_component((ui_component_id_t)id, &saved[id - 1U]);
    puts("PASS: shifted headers/footers/voltage/week rows match uncullled reference, including missing fonts");
}
#endif
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *paths[] = {"tools/dist/u8g2_font_wqy9_t_lunar.bin", "tools/dist/u8g2_font_wqy12_t_lunar.bin", "tools/dist/u8g2_font_helvB18_tn.bin"};
    for (unsigned i = 0; i < 3; i++) {
        FILE *f = fopen(paths[i], "rb"); assert(f);
        size_t n = fread(fonts[i], 1, sizeof(fonts[i]), f); assert(feof(f)); fclose(f);
        sources[i] = (u8g2_font_source_t){NULL, n, read_font, (const void *)(size_t)i, 1};
    }
    calendar_annotations_reset();

    const unsigned expected[3][2]={{0xed91cc84,0x5c457108},{0x3e109725,0x59b1db99},{0x14bbafcf,0x14bbafcf}};
    for (unsigned mode = 1; mode <= 3; mode++) {
        for (unsigned color = 1; color <= 2; color++) {
            gui_data_t data = {.mode=mode, .color=color, .width=800, .height=480,
                              .timestamp=1789347600U, .week_start=1, .temperature=25,
                              .voltage_mv=3100, .ssid="NRF_EPD"};
            memset(frame, 255, sizeof(frame)); reads=pages=0;
            DrawGUI(&data, capture, NULL);
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
#ifndef GENERATE_GUI_GOLDEN
            assert(hash()==expected[mode-1][color-1]);
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
#else
            (void)expected;
#endif
            printf("mode=%u color=%u hash=%08x pages=%u font_reads=%u\n", mode, color, hash(), pages, reads);
        }
    }
    test_calendar_edges();
    test_annotation_dots_preserve_holiday_and_lunar_pixels();
    test_scene_render_io_failure();
    test_scene_initialization_io_failure();
    test_todo_calendar_render();
    test_todo_calendar_date_rollover();
    test_todo_custom_pane_bounds();
    test_todo_header();
    test_todo_font_io_failure_does_not_commit_frame();
    test_fullscreen_todo_render();
    test_todo_custom_fonts();
    test_todo_extended_fonts();
    test_todo_phase_render();
#ifdef GUI_LAYOUT_REFERENCE
    test_moved_layouts();
#endif
    /* A missing external UI package must produce a visible setup screen. */
    ui_resources_stub_set_available(false);
    gui_data_t missing = {.mode=MODE_CALENDAR, .color=1, .width=800, .height=480};
    memset(frame, 255, sizeof(frame));
    pages = 0;
    DrawGUI(&missing, capture, NULL);
    unsigned marked = 0;
    for (size_t i = 0; i < sizeof(frame[0]); ++i)
        marked += frame[0][i] != 255U;
    assert(pages && marked > 100U && GUI_RenderValid());
    ui_resources_stub_set_available(true);
#ifndef GENERATE_GUI_GOLDEN
    test_clock_strips();
    test_clock_render_io_failure();
#else
    (void)test_clock_strips;
#endif
    puts("PASS: full-frame golden images and byte-exact clock OLD/NEW strips, failure fallback");
    return 0;
}
