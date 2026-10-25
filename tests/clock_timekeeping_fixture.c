#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sdk_config.h"

#define RTC_MASK 0x00FFFFFFU
#define APP_TIMER_CLOCK_FREQ 32768U
#define TIMER_TICKS(ms) ((uint32_t)(((uint64_t)(ms) * APP_TIMER_CLOCK_FREQ + \
                         500U * (APP_TIMER_CONFIG_RTC_FREQUENCY + 1U)) / \
                         (1000U * (APP_TIMER_CONFIG_RTC_FREQUENCY + 1U))))
#define UNUSED_PARAMETER(value) ((void)(value))
#define APP_ERROR_CHECK(result) assert((result) == 0U)
#define CRITICAL_REGION_ENTER() do { assert(critical_depth == 0U); critical_depth++; } while (0)
#define CRITICAL_REGION_EXIT() do { assert(critical_depth == 1U); critical_depth--; } while (0)
#define app_timer_stop(...) assert(!"set_timestamp must leave the RTC timer running")

#define INITIAL_TIMESTAMP 1735689600U
#define WEEK_SECONDS 604800U

static unsigned critical_depth, counter_reads, timer_starts, timer_callbacks;
static bool rtc_ready;
static uint32_t rtc_ticks, notified_timestamp;
static uint64_t total_ticks;
static unsigned m_epd;
static const unsigned m_clock_timer_id = 1U;

static uint32_t app_timer_cnt_get(void)
{
    assert(rtc_ready && critical_depth == 1U);
    counter_reads++;
    return rtc_ticks;
}

static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before)
{
    return (now - before) & RTC_MASK;
}

static uint32_t app_timer_start(unsigned id, uint32_t interval, void *context)
{
    assert(critical_depth == 1U && rtc_ready);
    assert(id == m_clock_timer_id && interval == TIMER_TICKS(1000U));
    assert(context == NULL);
    timer_starts++;
    return 0U;
}

static void ble_epd_on_timer(unsigned *epd, uint32_t now, bool force)
{
    assert(critical_depth == 0U && epd == &m_epd && !force);
    timer_callbacks++;
    notified_timestamp = now;
}

/* PRODUCTION_CLOCK */

static void reset_clock(uint32_t initial_ticks)
{
    assert(critical_depth == 0U);
    m_timestamp = INITIAL_TIMESTAMP;
    m_uptime_seconds = 1U;
    m_clock_started = false;
    m_clock_last_ticks = m_clock_subsecond_ticks = m_uptime_subsecond_ticks = 0U;
    m_clock_calibration_remainder = 0U;
    rtc_ticks = initial_ticks & RTC_MASK;
    total_ticks = 0U;
    rtc_ready = false;
    counter_reads = timer_starts = timer_callbacks = notified_timestamp = 0U;
}

static void start_clock(void)
{
    rtc_ready = true;
    application_timers_start();
    assert(m_clock_started && timer_starts == 1U && critical_depth == 0U);
}

static void advance_ticks(uint32_t ticks)
{
    /* A 1 Hz callback keeps production samples below this full-wrap limit. */
    assert(ticks <= RTC_MASK);
    rtc_ticks = (rtc_ticks + ticks) & RTC_MASK;
    total_ticks += ticks;
}

static uint64_t oracle_ticks(void)
{
    return total_ticks * 1000000ULL / (1000000 + EXPECTED_CALIBRATION_PPM);
}

static uint32_t oracle_seconds(void)
{
    return (uint32_t)(total_ticks * 1000000ULL /
                      ((uint64_t)CLOCK_TICKS_PER_SECOND *
                       (1000000 + EXPECTED_CALIBRATION_PPM)));
}

static void assert_unsynced_time(void)
{
    assert(timestamp() == INITIAL_TIMESTAMP + oracle_seconds());
    assert(app_uptime_seconds() == 1U + oracle_seconds());
    assert(critical_depth == 0U);
}

static void test_reads_before_start(void)
{
    reset_clock(12345U);
    assert(timestamp() == INITIAL_TIMESTAMP);
    assert(app_uptime_seconds() == 1U);
    set_timestamp(INITIAL_TIMESTAMP + 42U);
    advance_ticks(CLOCK_TICKS_PER_SECOND);
    assert(timestamp() == INITIAL_TIMESTAMP + 42U);
    assert(app_uptime_seconds() == 1U);
    assert(counter_reads == 0U && timer_starts == 0U);
    start_clock();
    assert(timestamp() == INITIAL_TIMESTAMP + 42U);
    assert(app_uptime_seconds() == 1U);
}

static void test_fractional_reads_delays_wrap_and_duplicate_callbacks(void)
{
    reset_clock(RTC_MASK - CLOCK_TICKS_PER_SECOND / 2U);
    start_clock();
    advance_ticks(CLOCK_TICKS_PER_SECOND / 3U);
    assert_unsynced_time();
    advance_ticks(5U * CLOCK_TICKS_PER_SECOND + CLOCK_TICKS_PER_SECOND / 7U);
    clock_timer_timeout_handler(NULL);
    assert(notified_timestamp == INITIAL_TIMESTAMP + oracle_seconds());
    assert(rtc_ticks < RTC_MASK - CLOCK_TICKS_PER_SECOND / 2U);
    assert_unsynced_time();

    uint32_t saved_timestamp = timestamp();
    uint32_t saved_uptime = app_uptime_seconds();
    for (unsigned i = 0U; i < 8U; i++) {
        clock_timer_timeout_handler(NULL);
        assert(timestamp() == saved_timestamp && app_uptime_seconds() == saved_uptime);
    }
    assert(timer_callbacks == 9U);

    for (unsigned i = 0U; i < 997U; i++) {
        advance_ticks(i % 7U + 1U);
        assert_unsynced_time();
        assert_unsynced_time(); /* Re-reading cannot consume the fractional remainder. */
    }
    advance_ticks(RTC_MASK - 100U); /* Nearly one wrap of delayed processing. */
    clock_timer_timeout_handler(NULL);
    assert_unsynced_time();
    assert(timer_starts == 1U);
}

static void test_sync_preserves_uptime_phase_and_timer(void)
{
    const uint32_t sync_time = INITIAL_TIMESTAMP + 1000U;
    reset_clock(89U);
    start_clock();
    advance_ticks(3U * CLOCK_TICKS_PER_SECOND / 4U);
    assert_unsynced_time();
    set_timestamp(sync_time);
    uint64_t wall_ticks_at_sync = oracle_ticks();
    advance_ticks(CLOCK_TICKS_PER_SECOND / 2U);
    assert(timestamp() == sync_time +
           (uint32_t)((oracle_ticks() - wall_ticks_at_sync) / CLOCK_TICKS_PER_SECOND));
    assert(app_uptime_seconds() == 2U); /* The pre-sync 3/4 second was retained. */

    for (unsigned i = 0U; i < 64U; i++) {
        set_timestamp(sync_time + i);
        wall_ticks_at_sync = oracle_ticks();
        advance_ticks(CLOCK_TICKS_PER_SECOND / 8U);
        assert(app_uptime_seconds() == 1U + oracle_seconds());
        assert(timestamp() == sync_time + i +
               (uint32_t)((oracle_ticks() - wall_ticks_at_sync) / CLOCK_TICKS_PER_SECOND));
        assert(timer_starts == 1U);
    }
}

static void test_large_sample_gaps(void)
{
    reset_clock(RTC_MASK - 10U);
    start_clock();
    for (unsigned i = 0U; i < 64U; i++) {
        /* Exercise signed correction products near their largest 24-bit input. */
        advance_ticks(RTC_MASK - i * 7919U);
        clock_timer_timeout_handler(NULL);
        assert_unsynced_time();
        advance_ticks(1U);
        assert_unsynced_time();
    }
}

static uint32_t simulate_week(uint64_t measured_ticks)
{
    reset_clock(RTC_MASK - 321U);
    start_clock();
    for (unsigned sample = 0U; total_ticks < measured_ticks; sample++) {
        uint32_t step = (1U + sample % 23U) * CLOCK_TICKS_PER_SECOND + sample % 97U;
        if ((uint64_t)step > measured_ticks - total_ticks)
            step = (uint32_t)(measured_ticks - total_ticks);
        advance_ticks(step);
        if ((sample % 3U) == 0U) {
            clock_timer_timeout_handler(NULL);
            if ((sample % 9U) == 0U) clock_timer_timeout_handler(NULL);
            assert(notified_timestamp == INITIAL_TIMESTAMP + oracle_seconds());
        } else if ((sample % 3U) == 1U) {
            (void)timestamp();
        } else {
            (void)app_uptime_seconds();
        }
        assert_unsynced_time();
    }
    assert(timer_starts == 1U);
    return timestamp() - INITIAL_TIMESTAMP;
}

static void test_seven_day_calibration(void)
{
    uint32_t elapsed = simulate_week((uint64_t)WEEK_SECONDS * CLOCK_TICKS_PER_SECOND);
    if (EXPECTED_CALIBRATION_PPM < 0) assert(elapsed > WEEK_SECONDS);
    if (EXPECTED_CALIBRATION_PPM > 0) assert(elapsed < WEEK_SECONDS);
    if (EXPECTED_CALIBRATION_PPM == 0) assert(elapsed == WEEK_SECONDS);

    /* Feed the corresponding measured oscillator rate for seven real days. */
    uint64_t measured = (uint64_t)WEEK_SECONDS * CLOCK_TICKS_PER_SECOND *
                        (1000000 + EXPECTED_CALIBRATION_PPM) / 1000000ULL;
    elapsed = simulate_week(measured);
    assert(elapsed >= WEEK_SECONDS - 1U && elapsed <= WEEK_SECONDS + 1U);

    /* The reported board lost three minutes: 604620 RTC seconds in seven days. */
    if (EXPECTED_CALIBRATION_PPM == -298) {
        elapsed = simulate_week(604620ULL * CLOCK_TICKS_PER_SECOND);
        assert(elapsed >= WEEK_SECONDS - 1U && elapsed <= WEEK_SECONDS + 1U);
    }
}

int main(void)
{
    assert(APP_TIMER_KEEPS_RTC_ACTIVE == 0); /* Exercise the unmodified SDK default. */
    assert(APP_CLOCK_CALIBRATION_PPM == EXPECTED_CALIBRATION_PPM);
    test_reads_before_start();
    test_fractional_reads_delays_wrap_and_duplicate_callbacks();
    test_sync_preserves_uptime_phase_and_timer();
    test_large_sample_gaps();
    test_seven_day_calibration();
    printf("PASS: clock RTC accounting, sync phase and seven days; prescaler=%u ppm=%d\n",
           (unsigned)APP_TIMER_CONFIG_RTC_FREQUENCY, EXPECTED_CALIBRATION_PPM);
    return 0;
}
