#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epd_clock_refresh.h"

void DrawGUI_ClockOnly(const gui_data_t *data, buffer_callback callback, void *callback_data)
{
    (void)data;
    (void)callback;
    (void)callback_data;
}

void GetTimeRefreshArea(const gui_data_t *data, uint32_t prev_ts,
                        uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    (void)data;
    (void)prev_ts;
    *x = 56;
    *y = 180;
    *w = 272;
    *h = 124;
}

void GetTimeRefreshAreaFromLocal(const gui_data_t *data,
                                 uint16_t lx, uint16_t ly, uint16_t lw, uint16_t lh,
                                 uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    (void)data;
    *x = lx;
    *y = ly;
    *w = lw;
    *h = lh;
}

bool UC8179_HadBusyTimeout(void)
{
    return false;
}

static void test_invalidate_then_mark_valid(void)
{
    epd_clock_refresh_invalidate();
    assert(!epd_clock_refresh_is_valid());
    epd_clock_refresh_mark_valid(100);
    assert(epd_clock_refresh_is_valid());
    epd_clock_refresh_invalidate();
    assert(!epd_clock_refresh_is_valid());
}

static void test_try_rejects_null_and_invalid_state(void)
{
    gui_data_t data;
    epd_model_t model;
    epd_driver_t drv;

    memset(&data, 0, sizeof(data));
    memset(&model, 0, sizeof(model));
    memset(&drv, 0, sizeof(drv));
    data.mode = MODE_CLOCK_CALENDAR;
    data.timestamp = 120;
    model.drv = &drv;
    model.width = 800;
    model.height = 480;

    epd_clock_refresh_invalidate();
    assert(!epd_clock_refresh_try(NULL, &data));
    assert(!epd_clock_refresh_try(&model, NULL));
    assert(!epd_clock_refresh_try(&model, &data));
}

static bool dummy_diff_push(epd_model_t *epd, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h,
                            const uint8_t *old_bm, const uint8_t *new_bm)
{
    (void)epd; (void)x; (void)y; (void)w; (void)h; (void)old_bm; (void)new_bm;
    return false;
}

static bool dummy_refresh_partial(epd_model_t *epd, uint16_t x, uint16_t y,
                                  uint16_t w, uint16_t h)
{
    (void)epd; (void)x; (void)y; (void)w; (void)h;
    return false;
}

static void test_try_rejects_non_clock_mode(void)
{
    gui_data_t data;
    epd_model_t model;
    epd_driver_t drv;

    memset(&data, 0, sizeof(data));
    memset(&model, 0, sizeof(model));
    memset(&drv, 0, sizeof(drv));
    data.mode = MODE_CALENDAR;
    data.timestamp = 120;
    model.drv = &drv;
    model.width = 800;
    model.height = 480;
    drv.diff_push = dummy_diff_push;
    drv.refresh_partial = dummy_refresh_partial;

    epd_clock_refresh_mark_valid(60);
    assert(!epd_clock_refresh_try(&model, &data));
}

int main(void)
{
    test_invalidate_then_mark_valid();
    test_try_rejects_null_and_invalid_state();
    test_try_rejects_non_clock_mode();
    puts("PASS: clock refresh");
    return 0;
}
