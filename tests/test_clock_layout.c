#include <assert.h>
#include <stdio.h>

#include "clock_layout.h"

static void test_split_clock_constants_match_panel_layout(void)
{
    assert(TIME_CS == 6);
    assert(TIME_ND == 2);
    assert(TIME_REFRESH_X == 0);
    assert(TIME_REFRESH_Y == 169);
    assert(TIME_REFRESH_WIDTH == 384);
    assert(TIME_REFRESH_HEIGHT == 150);
    assert(SPLIT_LEFT_WIDTH == 392);
    assert(SPLIT_RIGHT_START == 392);
}

static void test_hhmm_geometry_matches_draw7number(void)
{
    assert(CLOCK_DIGIT_BLOCK_WIDTH == 124);
    assert(CLOCK_TIME_WIDTH == 272);
    assert(CLOCK_TIME_HEIGHT == 124);
    assert(CLOCK_DIGIT_SINGLE_WIDTH == 68);
    assert(clock_digit_block_width(TIME_CS, TIME_ND) == CLOCK_DIGIT_BLOCK_WIDTH);
    assert(clock_time_width(TIME_CS, TIME_ND) == CLOCK_TIME_WIDTH);
    assert(clock_time_height(TIME_CS) == CLOCK_TIME_HEIGHT);
    assert(clock_digit_single_width(TIME_CS) == CLOCK_DIGIT_SINGLE_WIDTH);
}

static void test_hhmm_is_centered_in_refresh_canvas(void)
{
    assert(clock_hhmm_local_x(TIME_REFRESH_WIDTH, TIME_CS, TIME_ND) == 56);
    assert(clock_hhmm_local_y(TIME_REFRESH_HEIGHT, TIME_CS) == 13);
}

static void test_colon_y_matches_former_float_offsets(void)
{
    /* DrawTime used y + 4.5*cS + 1 and y + 13.5*cS + 3. cS is even (6 or 10). */
    assert(clock_colon_top_y(0, 6) == 28);
    assert(clock_colon_bot_y(0, 6) == 84);
    assert(clock_colon_top_y(182, 6) == 210);
    assert(clock_colon_bot_y(182, 6) == 266);
    assert(clock_colon_top_y(138, 10) == 184);
    assert(clock_colon_bot_y(138, 10) == 276);
}

static void test_minute_ones_local_x_is_rightmost_digit(void)
{
    uint16_t hhmm_x = clock_hhmm_local_x(TIME_REFRESH_WIDTH, TIME_CS, TIME_ND);

    /* HH at 56, MM starts at 56+124+36=216, ones digit at 216+56=272. */
    assert(clock_minute_ones_local_x(hhmm_x, TIME_CS, TIME_ND) == 272);
}

static void test_fullscreen_clock_scale_uses_same_formulas(void)
{
    uint16_t cS = (uint16_t)(480 / 45);

    assert(cS == 10);
    assert(clock_time_width(cS, 2) == 448);
    assert(clock_time_draw_width(cS, 2) == 468);
    assert(clock_time_height(cS) == 204);
}

static void test_split_refresh_geom_keeps_time_refresh_canvas(void)
{
    clock_refresh_geom_t g = clock_refresh_geom(false, 800, 480);
    uint16_t sx = 0;
    uint16_t sy = 0;

    assert(g.origin_x == TIME_REFRESH_X);
    assert(g.origin_y == TIME_REFRESH_Y);
    assert(g.canvas_w == TIME_REFRESH_WIDTH);
    assert(g.canvas_h == TIME_REFRESH_HEIGHT);
    assert(g.cS == TIME_CS);
    assert(g.nD == TIME_ND);
    clock_hhmm_screen_xy(&g, &sx, &sy);
    assert(sx == 56);
    assert(sy == 182);
}

static void test_fullscreen_refresh_geom_matches_drawclock_digits(void)
{
    clock_refresh_geom_t g = clock_refresh_geom(true, 800, 480);
    uint16_t sx = 0;
    uint16_t sy = 0;

    assert(clock_full_cs(480) == 10);
    assert(g.cS == 10);
    assert(g.nD == 2);
    assert(g.canvas_w == 480);
    assert(g.canvas_h == 208);
    assert(g.origin_x == 160);
    assert(g.origin_y == 136);
    clock_hhmm_screen_xy(&g, &sx, &sy);
    assert(sx == 166);
    assert(sy == 138);
}

static void test_fullscreen_canvas_covers_draw7number_ones_triangles(void)
{
    /* DrawTime uses block + 6*cS + MM, not clock_time_width's +4*cS.
     * The ones-digit right triangles sit past 448px and were clipped on 局刷. */
    clock_refresh_geom_t g = clock_refresh_geom(true, 800, 480);
    uint16_t hhmm_x = clock_hhmm_local_x(g.canvas_w, g.cS, g.nD);
    uint16_t ones_x = (uint16_t)(hhmm_x + clock_digit_block_width(g.cS, g.nD)
                                 + 6U * g.cS + clock_digit_single_width(g.cS));
    uint16_t ones_r = (uint16_t)(ones_x + 9U * g.cS + 2U);
    clock_refresh_window_t win =
        clock_refresh_local_window(&g, 43680U, 43740U);

    assert(ones_r <= g.canvas_w);
    assert(win.x <= ones_x);
    assert((uint32_t)win.x + win.w >= ones_r);
}

static void test_fullscreen_ones_window_fits_old_buffer(void)
{
    clock_refresh_geom_t g = clock_refresh_geom(true, 800, 480);
    clock_refresh_window_t win =
        clock_hhmm_window_local(g.canvas_w, g.canvas_h, g.cS, g.nD);
    uint16_t ones_w;
    uint32_t ones_bytes;
    uint32_t full_bytes;

    win.x = clock_minute_ones_local_x(win.x, g.cS, g.nD);
    win.w = clock_digit_single_width(g.cS);
    clock_align_window_x(&win.x, &win.w, g.canvas_w);
    ones_w = (uint16_t)(win.w / 8U);
    ones_bytes = (uint32_t)ones_w * win.h;
    full_bytes = (uint32_t)(g.canvas_w / 8U) * win.h;

    assert(ones_bytes <= 34U * 128U);
    assert(full_bytes > 34U * 128U);
}

int main(void)
{
    test_split_clock_constants_match_panel_layout();
    test_hhmm_geometry_matches_draw7number();
    test_hhmm_is_centered_in_refresh_canvas();
    test_colon_y_matches_former_float_offsets();
    test_minute_ones_local_x_is_rightmost_digit();
    test_fullscreen_clock_scale_uses_same_formulas();
    test_split_refresh_geom_keeps_time_refresh_canvas();
    test_fullscreen_refresh_geom_matches_drawclock_digits();
    test_fullscreen_canvas_covers_draw7number_ones_triangles();
    test_fullscreen_ones_window_fits_old_buffer();
    puts("PASS: clock layout");
    return 0;
}
