#include "epd_refresh_policy.h"

bool epd_refresh_in_quiet_hours(uint8_t hour)
{
    return hour >= 1U && hour <= 5U;
}

uint32_t epd_refresh_full_interval_s(uint8_t full_refresh_minutes)
{
    if (full_refresh_minutes == 0U || full_refresh_minutes == 0xFFU) {
        return 3600U;
    }
    return (uint32_t)full_refresh_minutes * 60U;
}

epd_refresh_schedule_out_t epd_refresh_schedule(const epd_refresh_schedule_in_t *in)
{
    epd_refresh_schedule_out_t out = {false, false};
    bool periodic;

    if (in == NULL || in->low_battery_hold || in->transfer_active) {
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

    /* Skip a duplicate periodic tick in the same minute (SET_TIME and the
     * 1 Hz timer can both land on :00). Do not skip the next minute: a
     * mid-minute enable at :16 is only 44s before :00, and the clock must
     * still 局刷. */
    if (!in->force_update && !out.force_full && in->last_refresh_start != 0U &&
        (in->timestamp / 60U) == (in->last_refresh_start / 60U)) {
        return out;
    }

    out.should_refresh = true;
    return out;
}

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
