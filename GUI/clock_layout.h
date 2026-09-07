#ifndef CLOCK_LAYOUT_H
#define CLOCK_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Split-screen clock + calendar on the 800x480 panel.
 * Left 392 px is byte-aligned; TIME_REFRESH_* stays 8 px left of the separator.
 */
#define SPLIT_LEFT_WIDTH 392
#define SPLIT_RIGHT_START 392

#define TIME_CS 6
#define TIME_ND 2
#define TIME_REFRESH_X 0
#define TIME_REFRESH_Y 169
#define TIME_REFRESH_WIDTH 384
#define TIME_REFRESH_HEIGHT 150

/* Draw7Number: width = nD*(11*cS+2)-2*cS, height = 20*cS+4 */
#define CLOCK_DIGIT_BLOCK_WIDTH (TIME_ND * (11 * TIME_CS + 2) - 2 * TIME_CS)
#define CLOCK_TIME_WIDTH (2 * CLOCK_DIGIT_BLOCK_WIDTH + 4 * TIME_CS)
#define CLOCK_TIME_HEIGHT (20 * TIME_CS + 4)
#define CLOCK_DIGIT_SINGLE_WIDTH (11 * TIME_CS + 2)

static inline uint16_t clock_digit_block_width(uint16_t cS, uint16_t nD)
{
    return (uint16_t)(nD * (11U * cS + 2U) - 2U * cS);
}

static inline uint16_t clock_time_width(uint16_t cS, uint16_t nD)
{
    return (uint16_t)(2U * clock_digit_block_width(cS, nD) + 4U * cS);
}

/* DrawTime: HH block + 2*cS to the colon + 4*cS to MM + MM block.
 * clock_time_width's +4*cS is the split TIME_REFRESH centering contract. */
static inline uint16_t clock_time_draw_width(uint16_t cS, uint16_t nD)
{
    return (uint16_t)(2U * clock_digit_block_width(cS, nD) + 6U * cS);
}

static inline uint16_t clock_hhmm_span(uint16_t cS, uint16_t nD)
{
    return (cS == TIME_CS) ? clock_time_width(cS, nD) : clock_time_draw_width(cS, nD);
}

static inline uint16_t clock_time_height(uint16_t cS)
{
    return (uint16_t)(20U * cS + 4U);
}

static inline uint16_t clock_digit_single_width(uint16_t cS)
{
    return (uint16_t)(11U * cS + 2U);
}

static inline uint16_t clock_hhmm_local_x(uint16_t canvas_w, uint16_t cS, uint16_t nD)
{
    return (uint16_t)((canvas_w - clock_hhmm_span(cS, nD)) / 2U);
}

static inline uint16_t clock_hhmm_local_y(uint16_t canvas_h, uint16_t cS)
{
    return (uint16_t)((canvas_h - clock_time_height(cS)) / 2U);
}

/* Colon squares were y + 4.5*cS + 1 and y + 13.5*cS + 3. cS is even. */
static inline uint16_t clock_colon_top_y(uint16_t y, uint16_t cS)
{
    return (uint16_t)(y + (9U * cS) / 2U + 1U);
}

static inline uint16_t clock_colon_bot_y(uint16_t y, uint16_t cS)
{
    return (uint16_t)(y + (27U * cS) / 2U + 3U);
}

static inline uint16_t clock_minute_ones_local_x(uint16_t hhmm_x, uint16_t cS, uint16_t nD)
{
    uint16_t block = clock_digit_block_width(cS, nD);

    return (uint16_t)(hhmm_x + block + 6U * cS + (block - clock_digit_single_width(cS)));
}

#define CLOCK_REFRESH_JUMP_MAX_S 600U
#define CLOCK_REFRESH_MARGIN_Y 2
#define CLOCK_REFRESH_WINDOWS_MAX 2
#define CLOCK_OLD_WINDOW_STRIDE_MAX (CLOCK_TIME_WIDTH / 8U)
#define CLOCK_OLD_WINDOW_STORE_H 128U
#define CLOCK_OLD_WINDOW_BYTES (CLOCK_OLD_WINDOW_STRIDE_MAX * CLOCK_OLD_WINDOW_STORE_H)

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} clock_refresh_window_t;

static inline bool clock_only_minute_ones_changed(uint32_t prev_ts, uint32_t curr_ts)
{
    uint32_t prev_minutes;
    uint32_t curr_minutes;
    uint8_t prev_hour;
    uint8_t prev_min;
    uint8_t curr_hour;
    uint8_t curr_min;

    if (prev_ts == 0U || curr_ts <= prev_ts ||
        (curr_ts - prev_ts) > CLOCK_REFRESH_JUMP_MAX_S) {
        return false;
    }

    prev_minutes = (prev_ts / 60U) % (24U * 60U);
    curr_minutes = (curr_ts / 60U) % (24U * 60U);
    prev_hour = (uint8_t)(prev_minutes / 60U);
    prev_min = (uint8_t)(prev_minutes % 60U);
    curr_hour = (uint8_t)(curr_minutes / 60U);
    curr_min = (uint8_t)(curr_minutes % 60U);
    return (prev_hour / 10U) == (curr_hour / 10U) &&
           (prev_hour % 10U) == (curr_hour % 10U) &&
           (prev_min / 10U) == (curr_min / 10U) &&
           (prev_min % 10U) != (curr_min % 10U);
}

static inline bool clock_only_minutes_changed(uint32_t prev_ts, uint32_t curr_ts)
{
    uint32_t prev_minutes;
    uint32_t curr_minutes;

    if (prev_ts == 0U || curr_ts <= prev_ts ||
        (curr_ts - prev_ts) > CLOCK_REFRESH_JUMP_MAX_S) {
        return false;
    }

    prev_minutes = (prev_ts / 60U) % (24U * 60U);
    curr_minutes = (curr_ts / 60U) % (24U * 60U);
    return (prev_minutes / 60U) == (curr_minutes / 60U) &&
           (prev_minutes % 60U) != (curr_minutes % 60U);
}

static inline uint16_t clock_digit_draw_width(uint16_t cS)
{
    /* Draw7Number nD=1: nD*(11*cS+2)-2*cS, includes the triangular ends. */
    return (uint16_t)(9U * cS + 2U);
}

static inline uint16_t clock_minute_tens_local_x(uint16_t hhmm_x, uint16_t cS, uint16_t nD)
{
    return (uint16_t)(hhmm_x + clock_digit_block_width(cS, nD) + 6U * cS);
}

static inline bool clock_refresh_window_fits_old(const clock_refresh_window_t *win)
{
    if (win == NULL || win->w < 8U || (win->w & 7U) != 0U || win->h == 0U) {
        return false;
    }
    return (uint32_t)(win->w / 8U) * win->h <= CLOCK_OLD_WINDOW_BYTES;
}

static inline clock_refresh_window_t clock_hhmm_window_local(uint16_t canvas_w,
                                                            uint16_t canvas_h,
                                                            uint16_t cS,
                                                            uint16_t nD)
{
    clock_refresh_window_t win;
    uint16_t time_h = clock_time_height(cS);
    int16_t y = (int16_t)clock_hhmm_local_y(canvas_h, cS);
    int16_t h = (int16_t)time_h;

    if (y > CLOCK_REFRESH_MARGIN_Y) {
        y -= CLOCK_REFRESH_MARGIN_Y;
        h += CLOCK_REFRESH_MARGIN_Y;
    }
    if (y + h + CLOCK_REFRESH_MARGIN_Y <= (int16_t)canvas_h) {
        h += CLOCK_REFRESH_MARGIN_Y;
    }

    win.x = clock_hhmm_local_x(canvas_w, cS, nD);
    win.y = (uint16_t)y;
    win.w = clock_hhmm_span(cS, nD);
    win.h = (uint16_t)h;
    return win;
}

static inline void clock_align_window_x(uint16_t *x, uint16_t *w, uint16_t canvas_w)
{
    uint16_t aligned;
    uint16_t end;

    if (x == NULL || w == NULL) {
        return;
    }
    aligned = (uint16_t)(*x & (uint16_t)~7U);
    end = (uint16_t)((*x + *w + 7U) & (uint16_t)~7U);
    if (end > canvas_w) {
        end = canvas_w;
    }
    *x = aligned;
    *w = (uint16_t)(end - aligned);
}

/* Full-screen MODE_CLOCK uses height/45 (10 on 480). Split keeps TIME_CS. */
static inline uint16_t clock_full_cs(uint16_t screen_h)
{
    return (uint16_t)(screen_h / 45U);
}

typedef struct {
    uint16_t origin_x;
    uint16_t origin_y;
    uint16_t canvas_w;
    uint16_t canvas_h;
    uint16_t cS;
    uint16_t nD;
} clock_refresh_geom_t;

/*
 * Split: TIME_REFRESH_* canvas, digits at screen (56, 182).
 * Full-screen 800x480: draw-span 468 centered in a 480x208 canvas at (160, 136).
 */
static inline clock_refresh_geom_t clock_refresh_geom(bool fullscreen,
                                                      uint16_t screen_w,
                                                      uint16_t screen_h)
{
    clock_refresh_geom_t g;

    g.nD = TIME_ND;
    if (!fullscreen) {
        g.origin_x = TIME_REFRESH_X;
        g.origin_y = TIME_REFRESH_Y;
        g.canvas_w = TIME_REFRESH_WIDTH;
        g.canvas_h = TIME_REFRESH_HEIGHT;
        g.cS = TIME_CS;
        return g;
    }

    g.cS = clock_full_cs(screen_h);
    {
        uint16_t time_w = clock_time_draw_width(g.cS, g.nD);
        uint16_t time_h = clock_time_height(g.cS);
        uint16_t digit_x = (uint16_t)((screen_w - time_w) / 2U);
        uint16_t digit_y = (uint16_t)((68U + (uint16_t)(screen_h - 68U)) / 2U - time_h / 2U);

        g.origin_x = (uint16_t)(digit_x & (uint16_t)~7U);
        if (digit_y > CLOCK_REFRESH_MARGIN_Y) {
            g.origin_y = (uint16_t)(digit_y - CLOCK_REFRESH_MARGIN_Y);
            g.canvas_h = (uint16_t)(time_h + 2U * CLOCK_REFRESH_MARGIN_Y);
        } else {
            g.origin_y = digit_y;
            g.canvas_h = (uint16_t)(time_h + CLOCK_REFRESH_MARGIN_Y);
        }
        g.canvas_w = (uint16_t)(((digit_x - g.origin_x) + time_w + 7U) & (uint16_t)~7U);
    }
    return g;
}

static inline clock_refresh_window_t clock_refresh_local_window(const clock_refresh_geom_t *g,
                                                               uint32_t prev_ts,
                                                               uint32_t curr_ts)
{
    clock_refresh_window_t win;

    win.x = 0;
    win.y = 0;
    win.w = 0;
    win.h = 0;
    if (g == NULL) {
        return win;
    }
    win = clock_hhmm_window_local(g->canvas_w, g->canvas_h, g->cS, g->nD);
    if (clock_only_minute_ones_changed(prev_ts, curr_ts)) {
        win.x = clock_minute_ones_local_x(win.x, g->cS, g->nD);
        win.w = clock_digit_single_width(g->cS);
    }
    clock_align_window_x(&win.x, &win.w, g->canvas_w);
    return win;
}

static inline void clock_refresh_fill_digit_window(clock_refresh_window_t *win,
                                                   const clock_refresh_window_t *base,
                                                   uint16_t x, uint16_t w, uint16_t canvas_w)
{
    *win = *base;
    win->x = x;
    win->w = w;
    clock_align_window_x(&win->x, &win->w, canvas_w);
}

static inline uint8_t clock_refresh_local_windows(const clock_refresh_geom_t *g,
                                                  uint32_t prev_ts,
                                                  uint32_t curr_ts,
                                                  clock_refresh_window_t *out,
                                                  uint8_t max)
{
    clock_refresh_window_t base;
    clock_refresh_window_t mm;
    uint16_t hhmm_x;
    uint16_t tens_x;
    uint16_t draw_w;

    if (g == NULL || out == NULL || max == 0U) {
        return 0U;
    }

    base = clock_hhmm_window_local(g->canvas_w, g->canvas_h, g->cS, g->nD);
    hhmm_x = base.x;

    if (clock_only_minute_ones_changed(prev_ts, curr_ts)) {
        clock_refresh_fill_digit_window(&out[0], &base,
                                        clock_minute_ones_local_x(hhmm_x, g->cS, g->nD),
                                        clock_digit_single_width(g->cS),
                                        g->canvas_w);
        return 1U;
    }

    if (clock_only_minutes_changed(prev_ts, curr_ts)) {
        tens_x = clock_minute_tens_local_x(hhmm_x, g->cS, g->nD);
        mm = base;
        mm.x = tens_x;
        mm.w = clock_digit_block_width(g->cS, g->nD);
        clock_align_window_x(&mm.x, &mm.w, g->canvas_w);
        if (clock_refresh_window_fits_old(&mm)) {
            out[0] = mm;
            return 1U;
        }
        if (max < 2U) {
            return 0U;
        }
        draw_w = clock_digit_draw_width(g->cS);
        clock_refresh_fill_digit_window(&out[0], &base, tens_x, draw_w, g->canvas_w);
        clock_refresh_fill_digit_window(&out[1], &base,
                                        (uint16_t)(tens_x + clock_digit_single_width(g->cS)),
                                        draw_w, g->canvas_w);
        if (!clock_refresh_window_fits_old(&out[0]) ||
            !clock_refresh_window_fits_old(&out[1])) {
            return 0U;
        }
        return 2U;
    }

    clock_align_window_x(&base.x, &base.w, g->canvas_w);
    out[0] = base;
    return 1U;
}

static inline void clock_hhmm_screen_xy(const clock_refresh_geom_t *g, uint16_t *x, uint16_t *y)
{
    if (g == NULL) {
        return;
    }
    if (x != NULL) {
        *x = (uint16_t)(g->origin_x + clock_hhmm_local_x(g->canvas_w, g->cS, g->nD));
    }
    if (y != NULL) {
        *y = (uint16_t)(g->origin_y + clock_hhmm_local_y(g->canvas_h, g->cS));
    }
}

static inline clock_refresh_geom_t clock_refresh_geom_sized(bool fullscreen,
                                                           uint16_t screen_w,
                                                           uint16_t screen_h)
{
    return clock_refresh_geom(fullscreen,
                              screen_w != 0U ? screen_w : 800U,
                              screen_h != 0U ? screen_h : 480U);
}

#endif
