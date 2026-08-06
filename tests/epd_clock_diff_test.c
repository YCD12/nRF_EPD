#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epd_clock_diff.h"
#include "Lunar.h"
#include "ui_runtime.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

static uint8_t gfx_buffer[GFX_PAGE_BUFFER_SIZE];
static ui_component_record_t components[2];
static int draw_values[64];
static size_t draw_value_count;
static uint16_t expected_x;
static uint16_t expected_y;
static uint16_t expected_w;
static uint16_t expected_h;
static uint16_t expected_tile_h;
static size_t expected_pushes;
static size_t push_count;
static size_t refresh_count;
static bool push_result;
static bool refresh_result;

static uint32_t test_time(uint8_t hour, uint8_t minute)
{
    return ((uint32_t)hour << 8) | minute;
}

void transformTime(uint32_t unix_time, struct devtm *result)
{
    memset(result, 0, sizeof(*result));
    result->tm_hour = (uint8_t)(unix_time >> 8);
    result->tm_min = (uint8_t)unix_time;
}

ret_code_t ui_runtime_load_render_cache(uint8_t mode_id,
                                        uint16_t screen_w,
                                        uint16_t screen_h,
                                        ui_mode_record_t *mode,
                                        const ui_component_record_t **out_components,
                                        uint8_t *component_count)
{
    assert(screen_w == 800U);
    assert(screen_h == 480U);
    memset(mode, 0, sizeof(*mode));
    memset(components, 0, sizeof(components));
    mode->mode_id = mode_id;

    components[0].type = UI_COMP_TIME_7SEG;
    components[0].p1 = 2;

    if (mode_id == MODE_CLOCK_CALENDAR) {
        components[0].x = 56;
        components[0].y = 182;
        components[0].p0 = 6;
        components[1] = components[0];
        components[1].flags = UI_FLAG_PARTIAL_ONLY;
        *component_count = 2U;
    } else if (mode_id == MODE_CLOCK) {
        components[0].x = 166;
        components[0].y = 139;
        components[0].p0 = 10;
        *component_count = 1U;
    } else {
        return 1U;
    }

    *out_components = components;
    return NRF_SUCCESS;
}

uint8_t *GFX_get_buffer(void)
{
    return gfx_buffer;
}

size_t GFX_get_buffer_size(void)
{
    return sizeof(gfx_buffer);
}

void GFX_begin(Adafruit_GFX *gfx, int16_t w, int16_t h, int16_t buffer_height)
{
    memset(gfx, 0, sizeof(*gfx));
    gfx->WIDTH = w;
    gfx->HEIGHT = h;
    gfx->_width = w;
    gfx->_height = h;
    gfx->page_height = buffer_height;
}

void GFX_fillScreen(Adafruit_GFX *gfx, uint16_t color)
{
    size_t bytes = (size_t)((gfx->_width + 7) / 8) * (size_t)gfx->page_height;
    memset(gfx->buffer, color == GFX_WHITE ? 0xFF : 0x00, bytes);
}

void GFX_fillRect(Adafruit_GFX *gfx, int16_t x, int16_t y,
                  int16_t w, int16_t h, uint16_t color)
{
    (void)gfx;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void Draw7Number(Adafruit_GFX *gfx, int value,
                 unsigned int x, unsigned int y, char scale,
                 unsigned int fg, unsigned int bg, int digits)
{
    (void)gfx;
    (void)x;
    (void)y;
    (void)scale;
    (void)fg;
    (void)bg;
    (void)digits;
    assert(draw_value_count < ARRAY_SIZE(draw_values));
    draw_values[draw_value_count++] = value;
}

static bool test_diff_push(epd_model_t *epd, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h,
                           const uint8_t *old_bitmap,
                           const uint8_t *new_bitmap)
{
    (void)epd;
    assert(push_count < expected_pushes);
    assert(x == expected_x);
    assert(y == expected_y + push_count * expected_tile_h);
    assert(w == expected_w);

    uint16_t remaining = expected_h - (uint16_t)(push_count * expected_tile_h);
    uint16_t expected_rows = remaining > expected_tile_h ? expected_tile_h : remaining;
    assert(h == expected_rows);
    assert(old_bitmap != NULL);
    assert(new_bitmap != NULL);
    assert(old_bitmap != new_bitmap);
    ++push_count;
    return push_result;
}

static bool test_refresh_partial(epd_model_t *epd, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h)
{
    (void)epd;
    assert(x == expected_x);
    assert(y == expected_y);
    assert(w == expected_w);
    assert(h == expected_h);
    assert(push_count == expected_pushes);
    ++refresh_count;
    return refresh_result;
}

static epd_driver_t test_driver = {
    .ic = EPD_DRIVER_IC_UC8179,
    .refresh_partial = test_refresh_partial,
    .diff_push = test_diff_push,
};

static epd_model_t test_model = {
    .id = EPD_UC8179_750_BWR,
    .color = EPD_COLOR_BWR,
    .drv = &test_driver,
    .width = 800U,
    .height = 480U,
};

static void reset_observation(uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              uint16_t tile_h, size_t pushes)
{
    memset(gfx_buffer, 0, sizeof(gfx_buffer));
    memset(draw_values, 0, sizeof(draw_values));
    draw_value_count = 0U;
    push_count = 0U;
    refresh_count = 0U;
    push_result = true;
    refresh_result = true;
    expected_x = x;
    expected_y = y;
    expected_w = w;
    expected_h = h;
    expected_tile_h = tile_h;
    expected_pushes = pushes;
}

static void expect_draw_values(size_t tiles,
                               uint8_t old_hour, uint8_t old_minute,
                               uint8_t new_hour, uint8_t new_minute)
{
    assert(draw_value_count == tiles * 4U);
    for (size_t tile = 0U; tile < tiles; ++tile) {
        size_t base = tile * 4U;
        assert(draw_values[base] == old_hour);
        assert(draw_values[base + 1U] == old_minute);
        assert(draw_values[base + 2U] == new_hour);
        assert(draw_values[base + 3U] == new_minute);
    }
}

static void test_clock_calendar_ignores_partial_only_mirror(void)
{
    reset_observation(56U, 174U, 288U, 140U, 35U, 4U);
    epd_clock_diff_seed(MODE_CLOCK_CALENDAR, test_time(12U, 34U));

    assert(epd_clock_diff_try_partial(&test_model, test_time(12U, 35U),
                                      MODE_CLOCK_CALENDAR));
    assert(push_count == 4U);
    assert(refresh_count == 1U);
    expect_draw_values(4U, 12U, 34U, 12U, 35U);

    assert(epd_clock_diff_try_partial(&test_model, test_time(12U, 35U),
                                      MODE_CLOCK_CALENDAR));
    assert(push_count == 4U);
    assert(refresh_count == 1U);
}

static void test_full_clock_layout_is_tiled(void)
{
    reset_observation(160U, 131U, 480U, 220U, 21U, 11U);
    epd_clock_diff_seed(MODE_CLOCK, test_time(8U, 59U));

    assert(epd_clock_diff_try_partial(&test_model, test_time(9U, 0U), MODE_CLOCK));
    assert(push_count == 11U);
    assert(refresh_count == 1U);
    expect_draw_values(11U, 8U, 59U, 9U, 0U);
}

static void test_failed_refresh_does_not_advance_old_state(void)
{
    reset_observation(160U, 131U, 480U, 220U, 21U, 11U);
    epd_clock_diff_seed(MODE_CLOCK, test_time(10U, 0U));
    refresh_result = false;

    assert(!epd_clock_diff_try_partial(&test_model, test_time(10U, 1U), MODE_CLOCK));
    expect_draw_values(11U, 10U, 0U, 10U, 1U);

    reset_observation(160U, 131U, 480U, 220U, 21U, 11U);
    assert(epd_clock_diff_try_partial(&test_model, test_time(10U, 2U), MODE_CLOCK));
    expect_draw_values(11U, 10U, 0U, 10U, 2U);
}

int main(void)
{
    test_clock_calendar_ignores_partial_only_mirror();
    test_full_clock_layout_is_tiled();
    test_failed_refresh_does_not_advance_old_state();
    puts("PASS: clock differential tiling and OLD-state contract");
    return 0;
}
