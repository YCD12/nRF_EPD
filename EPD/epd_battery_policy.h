#ifndef EPD_BATTERY_POLICY_H
#define EPD_BATTERY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Same thresholds as EPD_driver.h. Kept here so host tests do not pull the
 * driver. If EPD_driver.h is included first, these stay unused. */
#ifndef LOW_BATTERY_THRESHOLD_MV
#define LOW_BATTERY_THRESHOLD_MV 2800
#endif
#ifndef CRITICAL_BATTERY_SHUTDOWN_MV
#define CRITICAL_BATTERY_SHUTDOWN_MV 2700
#endif
#ifndef LOW_BATTERY_LED_BLINK_INTERVAL_S
#define LOW_BATTERY_LED_BLINK_INTERVAL_S 30
#endif

static inline bool epd_battery_is_low(uint16_t voltage_mv)
{
    return voltage_mv <= (uint16_t)LOW_BATTERY_THRESHOLD_MV;
}

static inline bool epd_battery_is_critical(uint16_t voltage_mv)
{
    return voltage_mv < (uint16_t)CRITICAL_BATTERY_SHUTDOWN_MV;
}

/* Option A: keep calendar/clock. Header voltage text stays visible. */
static inline bool epd_battery_replaces_gui(uint16_t voltage_mv)
{
    (void)voltage_mv;
    return false;
}

static inline bool epd_battery_allows_clock_partial(uint16_t voltage_mv)
{
    return !epd_battery_is_low(voltage_mv);
}

/* One EPD_LED_BLINK per interval while low-battery hold is idle. */
static inline bool epd_battery_should_blink_led(bool low_battery_hold, bool force_update,
                                                uint32_t timestamp)
{
    if (!low_battery_hold || force_update) {
        return false;
    }
    return (timestamp % (uint32_t)LOW_BATTERY_LED_BLINK_INTERVAL_S) == 0U;
}

#endif
