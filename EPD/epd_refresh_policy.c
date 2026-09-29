#include "epd_refresh_policy.h"

/* 夜间静默：01:00–05:59，周期全刷改为局刷。 */
bool epd_refresh_in_quiet_hours(uint8_t hour)
{
    return hour >= 1U && hour <= 5U;
}

/* 配置的全刷间隔（秒）。0 / 0xFF 表示每小时。 */
uint32_t epd_refresh_full_interval_s(uint8_t full_refresh_minutes)
{
    if (full_refresh_minutes == 0U || full_refresh_minutes == 0xFFU) {
        return 3600U;
    }
    return (uint32_t)full_refresh_minutes * 60U;
}

/* 日历每天 00:00；时钟每分钟。低电量/传图时不安排。 */
epd_refresh_schedule_out_t epd_refresh_schedule(const epd_refresh_schedule_in_t *in)
{
    epd_refresh_schedule_out_t out = {false, false};
    bool periodic;

    /* A directly streamed picture has no MCU-side pixels to redraw after reset. */
    if (in == NULL || in->mode == MODE_PICTURE ||
        in->low_battery_hold || in->transfer_active) {
        return out;
    }

    periodic = false;
    if (in->mode == MODE_CALENDAR) {
        periodic = (in->timestamp % 86400U) == 0U;
    } else if (epd_is_clock_mode(in->mode)) {
        periodic = (in->timestamp % 60U) == 0U;
    }

    out.force_full = in->day_rolled &&
                     (in->mode == MODE_CALENDAR || epd_is_clock_mode(in->mode));

    if (!in->force_update && !out.force_full && !periodic) {
        return out;
    }

    /* 同一分钟内跳过重复的周期触发（SET_TIME 与 1 Hz 定时器都可能落在 :00）。
     * 不要跳过下一分钟：若在 :16 中途使能，距 :00 仅 44 秒，时钟仍须局刷。 */
    if (!in->force_update && !out.force_full && in->last_refresh_start != 0U &&
        (in->timestamp / 60U) == (in->last_refresh_start / 60U)) {
        return out;
    }

    out.should_refresh = true;
    return out;
}

/* 非时钟、无硬件局刷、首次/强制/周期边界（非夜间）走全刷。 */
epd_refresh_kind_t epd_refresh_kind(const epd_refresh_kind_in_t *in)
{
    uint32_t interval;
    bool at_boundary;

    if (in == NULL || !epd_is_clock_mode(in->mode) || !in->hardware_partial_ok) {
        return EPD_REFRESH_KIND_FULL;
    }

    interval = epd_refresh_full_interval_s(in->full_refresh_minutes);
    at_boundary = interval != 0U && (in->timestamp % interval) == 0U;

    if (in->force_full_refresh || in->last_full_clear == 0U || !in->clock_state_valid ||
        (at_boundary && !epd_refresh_in_quiet_hours(in->hour))) {
        return EPD_REFRESH_KIND_FULL;
    }
    return EPD_REFRESH_KIND_PARTIAL;
}
