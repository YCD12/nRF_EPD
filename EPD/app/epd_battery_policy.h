#ifndef EPD_BATTERY_POLICY_H
#define EPD_BATTERY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* 阈值与 EPD_driver.h 一致。单独放在此头，避免主机测试拉入驱动。
 * 若已先包含 EPD_driver.h，这些宏不会被重复定义。 */
#ifndef LOW_BATTERY_THRESHOLD_MV
#define LOW_BATTERY_THRESHOLD_MV 2800
#endif
#ifndef CRITICAL_BATTERY_SHUTDOWN_MV
#define CRITICAL_BATTERY_SHUTDOWN_MV 2700
#endif
#ifndef LOW_BATTERY_LED_BLINK_INTERVAL_S
#define LOW_BATTERY_LED_BLINK_INTERVAL_S 30
#endif

/* <= 2.80 V：低电量保持 */
static inline bool epd_battery_is_low(uint16_t voltage_mv)
{
    return voltage_mv <= (uint16_t)LOW_BATTERY_THRESHOLD_MV;
}

/* < 2.70 V：临界，画完最后一帧后关机 */
static inline bool epd_battery_is_critical(uint16_t voltage_mv)
{
    return voltage_mv < (uint16_t)CRITICAL_BATTERY_SHUTDOWN_MV;
}

/* 方案 A：保留月历/时钟布局，头部电压文字仍可见。 */
static inline bool epd_battery_replaces_gui(uint16_t voltage_mv)
{
    (void)voltage_mv;
    return false;
}

/* 低电量时不用时钟局刷，避免波形功耗 */
static inline bool epd_battery_allows_clock_partial(uint16_t voltage_mv)
{
    return !epd_battery_is_low(voltage_mv);
}

/* 低电量保持空闲时，每个间隔闪一次 LED。 */
static inline bool epd_battery_should_blink_led(bool low_battery_hold, bool force_update,
                                                uint32_t timestamp)
{
    if (!low_battery_hold || force_update) {
        return false;
    }
    return (timestamp % (uint32_t)LOW_BATTERY_LED_BLINK_INTERVAL_S) == 0U;
}

#endif
