#ifndef CLOCK_LAYOUT_H
#define CLOCK_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 800x480 分屏：左时钟 + 右日历。左侧 392 px 按字节对齐；
 * TIME_REFRESH_* 在分隔线左侧再留 8 px。
 */
#define SPLIT_LEFT_WIDTH 392
#define SPLIT_RIGHT_START 392

#define TIME_CS 6                 /* 分屏 7 段缩放 */
#define TIME_ND 2                 /* 时/分各两位 */
#define TIME_REFRESH_X 0          /* 分屏时钟画布（相对左半屏） */
#define TIME_REFRESH_Y 169
#define TIME_REFRESH_WIDTH 384
#define TIME_REFRESH_HEIGHT 150

/* Draw7Number：宽 = nD*(11*cS+2)-2*cS，高 = 20*cS+4 */
#define CLOCK_DIGIT_BLOCK_WIDTH (TIME_ND * (11 * TIME_CS + 2) - 2 * TIME_CS)
#define CLOCK_TIME_WIDTH (2 * CLOCK_DIGIT_BLOCK_WIDTH + 4 * TIME_CS)
#define CLOCK_TIME_HEIGHT (20 * TIME_CS + 4)
#define CLOCK_DIGIT_SINGLE_WIDTH (11 * TIME_CS + 2)

static inline uint16_t clock_digit_block_width(uint16_t cS, uint16_t nD) /* HH 或 MM 一块 */
{
    return (uint16_t)(nD * (11U * cS + 2U) - 2U * cS);
}

/* 分屏 TIME_REFRESH 居中用的 HH:MM 跨度（+4*cS）。 */
static inline uint16_t clock_time_width(uint16_t cS, uint16_t nD)
{
    return (uint16_t)(2U * clock_digit_block_width(cS, nD) + 4U * cS);
}

/* DrawTime：HH 块 + 2*cS 到冒号 + 4*cS 到 MM + MM 块。
 * clock_time_width 的 +4*cS 是分屏 TIME_REFRESH 居中约定。 */
static inline uint16_t clock_time_draw_width(uint16_t cS, uint16_t nD)
{
    return (uint16_t)(2U * clock_digit_block_width(cS, nD) + 6U * cS);
}

/* 分屏用居中约定宽度，全屏用实际绘制宽度。 */
static inline uint16_t clock_hhmm_span(uint16_t cS, uint16_t nD)
{
    return (cS == TIME_CS) ? clock_time_width(cS, nD) : clock_time_draw_width(cS, nD);
}

static inline uint16_t clock_hhmm_span_gap(uint16_t cS, uint16_t nD, uint8_t gap)
{
    return (uint16_t)(2U * clock_digit_block_width(cS, nD) + (uint16_t)gap * cS);
}

static inline uint16_t clock_time_height(uint16_t cS) /* 7 段数字高度 */
{
    return (uint16_t)(20U * cS + 4U);
}

static inline uint16_t clock_digit_single_width(uint16_t cS) /* 含字间距的一位宽 */
{
    return (uint16_t)(11U * cS + 2U);
}

static inline uint16_t clock_hhmm_local_x(uint16_t canvas_w, uint16_t cS, uint16_t nD) /* 画布内水平居中 */
{
    return (uint16_t)((canvas_w - clock_hhmm_span(cS, nD)) / 2U);
}

static inline uint16_t clock_hhmm_local_x_gap(uint16_t canvas_w, uint16_t cS,
                                              uint16_t nD, uint8_t gap)
{
    return (uint16_t)((canvas_w - clock_hhmm_span_gap(cS, nD, gap)) / 2U);
}

static inline uint16_t clock_hhmm_local_y(uint16_t canvas_h, uint16_t cS) /* 画布内垂直居中 */
{
    return (uint16_t)((canvas_h - clock_time_height(cS)) / 2U);
}

/* 冒号方块：y + 4.5*cS + 1 与 y + 13.5*cS + 3。cS 为偶数。 */
static inline uint16_t clock_colon_top_y(uint16_t y, uint16_t cS)
{
    return (uint16_t)(y + (9U * cS) / 2U + 1U);
}

static inline uint16_t clock_colon_bot_y(uint16_t y, uint16_t cS) /* 下点 */
{
    return (uint16_t)(y + (27U * cS) / 2U + 3U);
}

/* 分钟个位左缘：HH 块 + 冒号跨度 + 十位占位。 */
static inline uint16_t clock_minute_ones_local_x(uint16_t hhmm_x, uint16_t cS, uint16_t nD)
{
    uint16_t block = clock_digit_block_width(cS, nD);

    return (uint16_t)(hhmm_x + block + 6U * cS + (block - clock_digit_single_width(cS)));
}

#define CLOCK_REFRESH_JUMP_MAX_S 600U /* 超过则不当成「只变分」 */
#define CLOCK_REFRESH_MARGIN_Y 2
#define CLOCK_REFRESH_WINDOWS_MAX 2
#define CLOCK_OLD_WINDOW_STRIDE_MAX ((CLOCK_TIME_WIDTH + 2U * TIME_CS + 7U) / 8U)
#define CLOCK_OLD_WINDOW_STORE_H 128U
#define CLOCK_OLD_WINDOW_BYTES (CLOCK_OLD_WINDOW_STRIDE_MAX * CLOCK_OLD_WINDOW_STORE_H)

typedef struct {
    uint16_t x; /* 画布局部坐标 */
    uint16_t y;
    uint16_t w;
    uint16_t h;
} clock_refresh_window_t;

/* 仅分钟个位变化（可缩到单数字窗）。 */
static inline bool clock_only_minute_ones_changed(uint32_t prev_ts, uint32_t curr_ts)
{
    if (prev_ts == 0U || curr_ts <= prev_ts ||
        (curr_ts - prev_ts) > CLOCK_REFRESH_JUMP_MAX_S) {
        return false;
    }

    /* Within the allowed forward jump, equal absolute ten-minute buckets
     * mean the hour and minute tens are unchanged, including at midnight. */
    uint32_t prev_minutes = prev_ts / 60U;
    uint32_t curr_minutes = curr_ts / 60U;
    return prev_minutes / 10U == curr_minutes / 10U && prev_minutes != curr_minutes;
}

/* 同时分变、小时不变（可缩到 MM）。 */
static inline bool clock_only_minutes_changed(uint32_t prev_ts, uint32_t curr_ts)
{
    if (prev_ts == 0U || curr_ts <= prev_ts ||
        (curr_ts - prev_ts) > CLOCK_REFRESH_JUMP_MAX_S) {
        return false;
    }

    uint32_t prev_minutes = prev_ts / 60U;
    uint32_t curr_minutes = curr_ts / 60U;
    return prev_minutes / 60U == curr_minutes / 60U && prev_minutes != curr_minutes;
}

static inline uint16_t clock_digit_draw_width(uint16_t cS)
{
    /* Draw7Number nD=1：nD*(11*cS+2)-2*cS，含三角端。 */
    return (uint16_t)(9U * cS + 2U);
}

/* 分钟十位左缘（冒号之后）。 */
static inline uint16_t clock_minute_tens_local_x(uint16_t hhmm_x, uint16_t cS, uint16_t nD)
{
    return (uint16_t)(hhmm_x + clock_digit_block_width(cS, nD) + 6U * cS);
}

/* 窗口能否放进 OLD 位图缓冲（宽 8 对齐）。 */
static inline bool clock_refresh_window_fits_old(const clock_refresh_window_t *win)
{
    if (win == NULL || win->w < 8U || (win->w & 7U) != 0U || win->h == 0U) {
        return false;
    }
    return (uint32_t)(win->w / 8U) * win->h <= CLOCK_OLD_WINDOW_BYTES;
}

/* 画布内整段 HH:MM 窗，上下各加边距。 */
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

static inline clock_refresh_window_t clock_hhmm_window_local_gap(uint16_t canvas_w,
                                                                 uint16_t canvas_h,
                                                                 uint16_t cS,
                                                                 uint16_t nD,
                                                                 uint8_t gap)
{
    clock_refresh_window_t win = clock_hhmm_window_local(canvas_w, canvas_h, cS, nD);
    win.x = clock_hhmm_local_x_gap(canvas_w, cS, nD, gap);
    win.w = clock_hhmm_span_gap(cS, nD, gap);
    return win;
}

/* x/w 对齐到 8 像素，满足控制器窗口。 */
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

/* 全屏 MODE_CLOCK 用 height/45（480 高时为 10）。分屏仍用 TIME_CS。 */
static inline uint16_t clock_full_cs(uint16_t screen_h)
{
    return (uint16_t)(screen_h / 45U);
}

typedef struct {
    uint16_t origin_x; /* 画布在屏幕上的原点 */
    uint16_t origin_y;
    uint16_t canvas_w;
    uint16_t canvas_h;
    uint16_t cS;
    uint16_t nD;
    uint8_t span_gap; /* legacy geometry */
    bool resource_geometry;
    bool partial_allowed;
    bool partial_sync_overlay;
    uint16_t face_x,face_y,face_w,face_h,digit_step,digit_w,minute_x;
} clock_refresh_geom_t;

/*
 * 分屏：TIME_REFRESH_* 画布，数字在屏幕 (56, 182)。
 * 全屏 800x480：绘制跨度 468，居中于 (160, 136) 的 480x208 画布。
 */
static inline clock_refresh_geom_t clock_refresh_geom(bool fullscreen,
                                                      uint16_t screen_w,
                                                      uint16_t screen_h)
{
    clock_refresh_geom_t g = {0};

    g.nD = TIME_ND;
    if (!fullscreen) {
        g.origin_x = TIME_REFRESH_X;
        g.origin_y = TIME_REFRESH_Y;
        g.canvas_w = TIME_REFRESH_WIDTH;
        g.canvas_h = TIME_REFRESH_HEIGHT;
        g.cS = TIME_CS;
        g.span_gap = 4U;
        return g;
    }

    g.cS = clock_full_cs(screen_h);
    g.span_gap = 6U;
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

/* 单个局刷窗：个位变化则缩到个位，否则整段 HH:MM。 */
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
    win = clock_hhmm_window_local_gap(g->canvas_w, g->canvas_h, g->cS, g->nD,
                                      g->span_gap);
    if(g->resource_geometry) {
        win.x=g->face_x;win.y=g->face_y;win.w=g->face_w;win.h=g->face_h;
        if(clock_only_minute_ones_changed(prev_ts,curr_ts)) {
            win.x+=g->minute_x+g->digit_step;win.w=g->digit_w;
        }
        clock_align_window_x(&win.x,&win.w,g->canvas_w);
        return win;
    }
    if (clock_only_minute_ones_changed(prev_ts, curr_ts)) {
        win.x = clock_minute_ones_local_x(win.x, g->cS, g->nD);
        win.w = clock_digit_single_width(g->cS);
    }
    clock_align_window_x(&win.x, &win.w, g->canvas_w);
    return win;
}

/* 在 HH:MM 底窗上裁出一个数字窗并 8 对齐。 */
static inline void clock_refresh_fill_digit_window(clock_refresh_window_t *win,
                                                   const clock_refresh_window_t *base,
                                                   uint16_t x, uint16_t w, uint16_t canvas_w)
{
    *win = *base;
    win->x = x;
    win->w = w;
    clock_align_window_x(&win->x, &win->w, canvas_w);
}

/* 填 1～2 个局刷窗。个位 1 窗；MM 能放下则 1 窗，否则两个数字窗；其余整段 HH:MM。 */
static inline uint8_t clock_refresh_local_windows(const clock_refresh_geom_t *g,
                                                  uint32_t prev_ts,
                                                  uint32_t curr_ts,
                                                  clock_refresh_window_t *out,
                                                  uint8_t max)
{
    clock_refresh_window_t base;
    clock_refresh_window_t mm;
    uint16_t hhmm_x;

    if (g == NULL || out == NULL || max == 0U) {
        return 0U;
    }

    if(g->resource_geometry) {
        base.x=g->face_x;base.y=g->face_y;base.w=g->face_w;base.h=g->face_h;
        if(clock_only_minute_ones_changed(prev_ts,curr_ts)) {
            base.x+=g->minute_x+g->digit_step;base.w=g->digit_w;
        } else if(clock_only_minutes_changed(prev_ts,curr_ts)) {
            base.x+=g->minute_x;base.w=g->digit_step+g->digit_w;
        }
        clock_align_window_x(&base.x,&base.w,g->canvas_w);
        if(clock_refresh_window_fits_old(&base)) {out[0]=base;return 1;}
        if(clock_only_minutes_changed(prev_ts,curr_ts) && max>=2) {
            for(uint8_t n=0;n<2;n++) {
                out[n]=base;out[n].x=g->face_x+g->minute_x+n*g->digit_step;out[n].w=g->digit_w;
                clock_align_window_x(&out[n].x,&out[n].w,g->canvas_w);
            }
            return clock_refresh_window_fits_old(&out[0]) && clock_refresh_window_fits_old(&out[1])?2:0;
        }
        out[0]=base;return 1;
    }
    base = clock_hhmm_window_local_gap(g->canvas_w, g->canvas_h, g->cS, g->nD,
                                       g->span_gap);
    hhmm_x = base.x;

    if (clock_only_minute_ones_changed(prev_ts, curr_ts)) {
        clock_refresh_fill_digit_window(&out[0], &base,
                                        clock_minute_ones_local_x(hhmm_x, g->cS, g->nD),
                                        clock_digit_single_width(g->cS),
                                        g->canvas_w);
        return 1U;
    }

    if (clock_only_minutes_changed(prev_ts, curr_ts)) {
        uint16_t tens_x = clock_minute_tens_local_x(hhmm_x, g->cS, g->nD);

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
        uint16_t draw_w = clock_digit_draw_width(g->cS);
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

/* 画布原点 + 局部居中 → 屏幕坐标。 */
static inline void clock_hhmm_screen_xy(const clock_refresh_geom_t *g, uint16_t *x, uint16_t *y)
{
    if (g == NULL) {
        return;
    }
    if (x != NULL) {
        *x = (uint16_t)(g->origin_x + clock_hhmm_local_x_gap(g->canvas_w, g->cS,
                                                             g->nD, g->span_gap));
    }
    if (y != NULL) {
        *y = (uint16_t)(g->origin_y + clock_hhmm_local_y(g->canvas_h, g->cS));
    }
}

/* 宽高为 0 时按 800x480。 */
static inline clock_refresh_geom_t clock_refresh_geom_sized(bool fullscreen,
                                                           uint16_t screen_w,
                                                           uint16_t screen_h)
{
    return clock_refresh_geom(fullscreen,
                              screen_w != 0U ? screen_w : 800U,
                              screen_h != 0U ? screen_h : 480U);
}

#endif
