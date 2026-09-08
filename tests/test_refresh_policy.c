#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "epd_battery_policy.h"
#include "epd_refresh_policy.h"

static epd_refresh_schedule_in_t base_schedule(void)
{
    epd_refresh_schedule_in_t in;

    memset(&in, 0, sizeof(in));
    in.mode = MODE_CLOCK_CALENDAR;
    in.timestamp = 120U;
    return in;
}

static void test_is_clock_mode(void)
{
    assert(!epd_is_clock_mode(MODE_PICTURE));
    assert(!epd_is_clock_mode(MODE_CALENDAR));
    assert(epd_is_clock_mode(MODE_CLOCK));
    assert(epd_is_clock_mode(MODE_CLOCK_CALENDAR));
}

static void test_quiet_hours_and_interval(void)
{
    assert(!epd_refresh_in_quiet_hours(0));
    assert(epd_refresh_in_quiet_hours(1));
    assert(epd_refresh_in_quiet_hours(5));
    assert(!epd_refresh_in_quiet_hours(6));
    assert(epd_refresh_full_interval_s(0) == 3600U);
    assert(epd_refresh_full_interval_s(0xFF) == 3600U);
    assert(epd_refresh_full_interval_s(30) == 1800U);
}

static void test_low_battery_keeps_calendar_clock_layout(void)
{
    /* Option A: hold periodic refresh, do not replace the GUI. */
    assert(epd_battery_is_low(LOW_BATTERY_THRESHOLD_MV));
    assert(epd_battery_is_low(CRITICAL_BATTERY_SHUTDOWN_MV));
    assert(!epd_battery_is_low(LOW_BATTERY_THRESHOLD_MV + 1U));
    assert(epd_battery_is_critical(CRITICAL_BATTERY_SHUTDOWN_MV - 1U));
    assert(!epd_battery_is_critical(CRITICAL_BATTERY_SHUTDOWN_MV));
    assert(!epd_battery_replaces_gui(LOW_BATTERY_THRESHOLD_MV));
    assert(!epd_battery_replaces_gui(CRITICAL_BATTERY_SHUTDOWN_MV - 1U));
    assert(!epd_battery_allows_clock_partial(LOW_BATTERY_THRESHOLD_MV));
    assert(epd_battery_allows_clock_partial(LOW_BATTERY_THRESHOLD_MV + 1U));
}

static void test_low_battery_led_blinks_every_30s(void)
{
    assert(LOW_BATTERY_LED_BLINK_INTERVAL_S == 30U);
    assert(!epd_battery_should_blink_led(false, false, 30U));
    assert(!epd_battery_should_blink_led(true, true, 30U));
    assert(!epd_battery_should_blink_led(true, false, 29U));
    assert(!epd_battery_should_blink_led(true, false, 31U));
    assert(epd_battery_should_blink_led(true, false, 0U));
    assert(epd_battery_should_blink_led(true, false, 30U));
    assert(epd_battery_should_blink_led(true, false, 60U));
}

static void test_schedule_skips_low_battery_hold(void)
{
    epd_refresh_schedule_in_t in = base_schedule();
    epd_refresh_schedule_out_t out;

    in.low_battery_hold = true;
    in.force_update = true;
    in.timestamp = 0U;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);
}

static void test_schedule_calendar_daily_and_clock_minutely(void)
{
    epd_refresh_schedule_in_t in = base_schedule();
    epd_refresh_schedule_out_t out;

    in.mode = MODE_CALENDAR;
    in.timestamp = 86400U;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);
    assert(!out.force_full);

    in.timestamp = 86401U;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);

    in.mode = MODE_CLOCK;
    in.timestamp = 180U;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);

    in.timestamp = 181U;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);
}

static void test_schedule_day_rollover_forces_full(void)
{
    epd_refresh_schedule_in_t in = base_schedule();
    epd_refresh_schedule_out_t out;

    in.day_rolled = true;
    in.timestamp = 181U;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);
    assert(out.force_full);
}

static void test_schedule_debounce_skips_same_minute_only(void)
{
    epd_refresh_schedule_in_t in = base_schedule();
    epd_refresh_schedule_out_t out;

    /* SET_TIME at 00:02:40 then the 00:03:00 tick is a new minute: must update. */
    in.timestamp = 180U;
    in.last_refresh_start = 160U;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);

    /* Same minute as an in-progress / just-finished refresh: skip the duplicate. */
    in.timestamp = 180U;
    in.last_refresh_start = 180U;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);

    /* Webpage "全屏时钟" at 11:42:16 must still 局刷 at 11:43:00 (44s < old 45s debounce). */
    in.timestamp = 11U * 3600U + 43U * 60U;
    in.last_refresh_start = 11U * 3600U + 42U * 60U + 16U;
    in.mode = MODE_CLOCK;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);
    assert(!out.force_full);

    in.timestamp = 180U;
    in.last_refresh_start = 160U;
    in.force_update = true;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);

    in.force_update = false;
    in.day_rolled = true;
    out = epd_refresh_schedule(&in);
    assert(out.should_refresh);
    assert(out.force_full);
}

static void test_picture_transfer_preempts_clock_refresh(void)
{
    epd_refresh_schedule_in_t in = base_schedule();
    epd_refresh_schedule_out_t out;

    assert(epd_clock_refresh_must_yield(MODE_PICTURE, false));
    assert(epd_clock_refresh_must_yield(MODE_CLOCK_CALENDAR, true));
    assert(!epd_clock_refresh_must_yield(MODE_CLOCK_CALENDAR, false));
    /* RESET_TRANSFER must hold the clock while still in clock mode. Flipping
     * to PICTURE first skips the clock-exit Init and the next full refresh
     * still shows the calendar framebuffer. */
    assert(epd_clock_refresh_must_yield(MODE_CLOCK, true));
    assert(epd_picture_write_blocked(true));
    assert(!epd_picture_write_blocked(false));

    in.timestamp = 180U;
    in.transfer_active = true;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);

    in.force_update = true;
    out = epd_refresh_schedule(&in);
    assert(!out.should_refresh);
}

static void test_kind_full_when_not_clock_or_hw_missing(void)
{
    epd_refresh_kind_in_t in;

    memset(&in, 0, sizeof(in));
    in.mode = MODE_CALENDAR;
    in.hardware_partial_ok = true;
    in.clock_state_valid = true;
    in.last_full_clear = 1U;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);

    in.mode = MODE_CLOCK;
    in.hardware_partial_ok = false;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);
}

static void test_kind_partial_unless_maintenance(void)
{
    epd_refresh_kind_in_t in;

    memset(&in, 0, sizeof(in));
    in.mode = MODE_CLOCK_CALENDAR;
    in.timestamp = 90U;
    in.last_full_clear = 1U;
    in.clock_state_valid = true;
    in.full_refresh_minutes = 0;
    in.hardware_partial_ok = true;
    in.hour = 10;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_PARTIAL);

    in.force_full_refresh = true;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);

    in.force_full_refresh = false;
    in.last_full_clear = 0U;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);

    in.last_full_clear = 1U;
    in.clock_state_valid = false;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);

    in.clock_state_valid = true;
    in.timestamp = 3600U;
    in.hour = 10;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_FULL);

    in.hour = 3;
    assert(epd_refresh_kind(&in) == EPD_REFRESH_KIND_PARTIAL);
}

int main(void)
{
    test_is_clock_mode();
    test_quiet_hours_and_interval();
    test_low_battery_keeps_calendar_clock_layout();
    test_low_battery_led_blinks_every_30s();
    test_schedule_skips_low_battery_hold();
    test_schedule_calendar_daily_and_clock_minutely();
    test_schedule_day_rollover_forces_full();
    test_schedule_debounce_skips_same_minute_only();
    test_picture_transfer_preempts_clock_refresh();
    test_kind_full_when_not_clock_or_hw_missing();
    test_kind_partial_unless_maintenance();
    puts("PASS: refresh policy");
    return 0;
}
