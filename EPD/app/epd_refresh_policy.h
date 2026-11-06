#ifndef EPD_REFRESH_POLICY_H
#define EPD_REFRESH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "GUI.h"

/* 是否安排一次物理刷新（周期/强制/换日）。 */
typedef struct {
    display_mode_t mode;
    uint32_t timestamp;         /* Unix 秒 */
    bool force_update;          /* 强制刷新（校时、切模式等） */
    bool low_battery_hold;      /* 低电量保持：不周期刷新 */
    bool day_rolled;            /* 跨日，日历/时钟需全刷 */
    uint32_t last_refresh_start; /* 上次物理刷新开始时刻，0=从未 */
    bool transfer_active;       /* 传图进行中，跳过刷新 */
} epd_refresh_schedule_in_t;

typedef struct {
    bool should_refresh; /* 本拍需要刷新 */
    bool force_full;     /* 因跨日等原因要求全刷 */
} epd_refresh_schedule_out_t;

/* 已决定刷新后，选全刷还是时钟局刷。 */
typedef struct {
    display_mode_t mode;
    uint32_t timestamp;
    bool force_full_refresh;
    uint32_t last_full_clear;     /* 上次全刷时刻，0=从未 */
    bool clock_state_valid;       /* 时钟 OLD 缓存是否可用 */
    uint8_t full_refresh_minutes; /* 周期全刷间隔（分钟；0/0xFF=1 小时） */
    bool hardware_partial_ok;     /* 驱动支持真 OLD/NEW 且电量允许 */
    uint8_t hour;                 /* 本地小时，用于夜间静默 */
} epd_refresh_kind_in_t;

typedef enum {
    EPD_REFRESH_KIND_FULL = 0,
    EPD_REFRESH_KIND_PARTIAL,
} epd_refresh_kind_t;

/* 全屏时钟或分屏时钟+日历 */
static inline bool epd_is_clock_mode(display_mode_t mode)
{
    return mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR;
}

static inline bool epd_is_todo_mode(display_mode_t mode)
{
    return mode == MODE_TODO_CALENDAR || mode == MODE_TODO;
}

/* 传图须中止/跳过进行中的时钟刷新，避免刷新结束时在 WRITE_BLOCK 中途睡眠或释放 GPIO。 */
static inline bool epd_clock_refresh_must_yield(display_mode_t mode,
                                                bool transfer_active)
{
    return transfer_active || mode == MODE_PICTURE;
}

/* WRITE_BLOCK 在 BLE 中断里执行，可能嵌套在 WaitBusy 中。时钟全刷未完成时不要流式写 DTM1/DTM2。 */
static inline bool epd_picture_write_blocked(bool display_refreshing)
{
    return display_refreshing;
}

bool epd_refresh_in_quiet_hours(uint8_t hour); /* 01:00–05:59 跳过整点全刷 */
uint32_t epd_refresh_full_interval_s(uint8_t full_refresh_minutes); /* 0/0xFF → 3600 秒 */
epd_refresh_schedule_out_t epd_refresh_schedule(const epd_refresh_schedule_in_t *in);
epd_refresh_kind_t epd_refresh_kind(const epd_refresh_kind_in_t *in); /* 全刷或时钟局刷 */

#endif
