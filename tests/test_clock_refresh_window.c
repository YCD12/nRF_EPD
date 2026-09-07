#include <assert.h>
#include <stdio.h>

#include "clock_layout.h"

static void test_only_minute_ones_shrinks(void)
{
    /* 12:08 -> 12:09 */
    assert(clock_only_minute_ones_changed(43680U, 43740U));
}

static void test_tens_or_hour_or_jump_does_not_shrink(void)
{
    /* 12:09 -> 12:10 */
    assert(!clock_only_minute_ones_changed(43740U, 43800U));
    /* 11:59 -> 12:00 */
    assert(!clock_only_minute_ones_changed(43140U, 43200U));
    /* first refresh / backwards / >10 min jump */
    assert(!clock_only_minute_ones_changed(0U, 43740U));
    assert(!clock_only_minute_ones_changed(43740U, 43680U));
    assert(!clock_only_minute_ones_changed(43000U, 43740U));
}

static void test_only_minutes_changed_covers_tens_rollover(void)
{
    /* 14:09 -> 14:10 keeps the hour, so minutes can stay on 局刷. */
    assert(clock_only_minutes_changed(14U * 3600U + 9U * 60U,
                                      14U * 3600U + 10U * 60U));
    assert(clock_only_minutes_changed(43740U, 43800U));
    assert(!clock_only_minutes_changed(43140U, 43200U)); /* 11:59 -> 12:00 */
    assert(!clock_only_minutes_changed(0U, 43800U));
}

static void test_hhmm_window_adds_two_pixel_margin(void)
{
    clock_refresh_window_t w =
        clock_hhmm_window_local(TIME_REFRESH_WIDTH, TIME_REFRESH_HEIGHT, TIME_CS, TIME_ND);

    assert(w.x == 56);
    assert(w.y == 11);
    assert(w.w == 272);
    assert(w.h == 128);
}

static void test_align_window_keeps_8px_and_expands_end(void)
{
    uint16_t x = 56;
    uint16_t w = 272;

    clock_align_window_x(&x, &w, TIME_REFRESH_WIDTH);
    assert(x == 56);
    assert(w == 272);

    x = 272;
    w = 68;
    clock_align_window_x(&x, &w, TIME_REFRESH_WIDTH);
    assert(x == 272);
    assert(w == 72);
}

static void test_local_window_follows_mode_geom(void)
{
    clock_refresh_geom_t split = clock_refresh_geom(false, 800, 480);
    clock_refresh_geom_t full = clock_refresh_geom(true, 800, 480);
    clock_refresh_window_t split_full =
        clock_refresh_local_window(&split, 0U, 43740U);
    clock_refresh_window_t split_ones =
        clock_refresh_local_window(&split, 43680U, 43740U);
    clock_refresh_window_t full_ones =
        clock_refresh_local_window(&full, 43680U, 43740U);
    clock_refresh_window_t full_hhmm =
        clock_refresh_local_window(&full, 0U, 43740U);

    assert(split_full.w == 272);
    assert(split_ones.w == 72);
    assert(split.origin_y + split_ones.y == 180);
    assert(full.cS == 10);
    assert(full_ones.w < full_hhmm.w);
    assert((uint32_t)(full_ones.w / 8U) * full_ones.h <= 34U * 128U);
    assert((uint32_t)(full_hhmm.w / 8U) * full_hhmm.h > 34U * 128U);
}

static void test_fullscreen_minute_tens_uses_fitting_windows(void)
{
    clock_refresh_geom_t full = clock_refresh_geom(true, 800, 480);
    clock_refresh_window_t wins[CLOCK_REFRESH_WINDOWS_MAX];
    uint8_t n = clock_refresh_local_windows(&full, 14U * 3600U + 9U * 60U,
                                            14U * 3600U + 10U * 60U,
                                            wins, CLOCK_REFRESH_WINDOWS_MAX);
    uint8_t i;

    assert(n == 2U);
    for (i = 0U; i < n; i++) {
        assert(clock_refresh_window_fits_old(&wins[i]));
        assert(wins[i].w < full.canvas_w);
    }
}

static void test_split_minute_tens_uses_one_fitting_window(void)
{
    clock_refresh_geom_t split = clock_refresh_geom(false, 800, 480);
    clock_refresh_window_t wins[CLOCK_REFRESH_WINDOWS_MAX];
    uint8_t n = clock_refresh_local_windows(&split, 43740U, 43800U,
                                            wins, CLOCK_REFRESH_WINDOWS_MAX);

    assert(n == 1U);
    assert(clock_refresh_window_fits_old(&wins[0]));
}

static void test_fullscreen_after_mid_minute_full_refresh_uses_ones_window(void)
{
    /* SET_TIME 11:42:16 then the 11:43:00 tick: only the minute ones digit changes.
     * prev_ts must be the last successful refresh. prev_ts=0 picks 448-wide HH:MM
     * and try() falls back to a flashing full refresh. */
    clock_refresh_geom_t full = clock_refresh_geom(true, 800, 480);
    uint32_t enable_ts = 11U * 3600U + 42U * 60U + 16U;
    uint32_t next_min = 11U * 3600U + 43U * 60U;
    clock_refresh_window_t synced =
        clock_refresh_local_window(&full, enable_ts, next_min);
    clock_refresh_window_t unsynced =
        clock_refresh_local_window(&full, 0U, next_min);

    assert(clock_only_minute_ones_changed(enable_ts, next_min));
    assert((uint32_t)(synced.w / 8U) * synced.h <= 34U * 128U);
    assert((uint32_t)(unsynced.w / 8U) * unsynced.h > 34U * 128U);
}

int main(void)
{
    test_only_minute_ones_shrinks();
    test_tens_or_hour_or_jump_does_not_shrink();
    test_only_minutes_changed_covers_tens_rollover();
    test_hhmm_window_adds_two_pixel_margin();
    test_align_window_keeps_8px_and_expands_end();
    test_local_window_follows_mode_geom();
    test_fullscreen_minute_tens_uses_fitting_windows();
    test_split_minute_tens_uses_one_fitting_window();
    test_fullscreen_after_mid_minute_full_refresh_uses_ones_window();
    puts("PASS: clock refresh window");
    return 0;
}
