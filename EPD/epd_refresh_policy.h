#ifndef EPD_REFRESH_POLICY_H
#define EPD_REFRESH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "GUI.h"

typedef struct {
    display_mode_t mode;
    uint32_t timestamp;
    bool force_update;
    bool low_battery_hold;
    bool day_rolled;
    uint32_t last_refresh_start;
    bool transfer_active;
} epd_refresh_schedule_in_t;

typedef struct {
    bool should_refresh;
    bool force_full;
} epd_refresh_schedule_out_t;

typedef struct {
    display_mode_t mode;
    uint32_t timestamp;
    bool force_full_refresh;
    uint32_t last_full_clear;
    bool clock_state_valid;
    uint8_t full_refresh_minutes;
    bool hardware_partial_ok;
    uint8_t hour;
} epd_refresh_kind_in_t;

typedef enum {
    EPD_REFRESH_KIND_FULL = 0,
    EPD_REFRESH_KIND_PARTIAL,
} epd_refresh_kind_t;

static inline bool epd_is_clock_mode(display_mode_t mode)
{
    return mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR;
}

/* Picture transfer must abort/skip an in-flight clock refresh so finish
 * does not sleep the panel or release GPIO mid-WRITE_BLOCK. */
static inline bool epd_clock_refresh_must_yield(display_mode_t mode,
                                                bool transfer_active)
{
    return transfer_active || mode == MODE_PICTURE;
}

/* WRITE_BLOCK runs in the BLE IRQ and can nest inside WaitBusy. Do not
 * stream DTM1/DTM2 while a clock full refresh is still on the panel. */
static inline bool epd_picture_write_blocked(bool display_refreshing)
{
    return display_refreshing;
}

bool epd_refresh_in_quiet_hours(uint8_t hour);
uint32_t epd_refresh_full_interval_s(uint8_t full_refresh_minutes);
epd_refresh_schedule_out_t epd_refresh_schedule(const epd_refresh_schedule_in_t *in);
epd_refresh_kind_t epd_refresh_kind(const epd_refresh_kind_in_t *in);

#endif
