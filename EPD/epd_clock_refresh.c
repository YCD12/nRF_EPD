#include "epd_clock_refresh.h"

#include <string.h>

#include "UC8179.h"
#include "clock_layout.h"
#include "epd_clock_window.h"
#include "epd_refresh_policy.h"

/* 分屏 HH:MM 为 272x128。全屏个位/十位数字能放下，整段 HH:MM 绘制窗放不下。
 * 分钟十位进位：分屏用一个 MM 窗，全屏用两个数字窗。高度上限仅作检查。 */
#define EPD_CLOCK_OLD_STRIDE_MAX CLOCK_OLD_WINDOW_STRIDE_MAX
#define EPD_CLOCK_OLD_HEIGHT_MAX 208U

static uint8_t s_clock_old_window[CLOCK_OLD_WINDOW_BYTES]; /* 上一帧窗口位图 */
static uint32_t s_clock_last_timestamp;
static bool s_clock_last_valid;
static bool s_clock_cache_ready; /* 缓存窗口与本次相同则可复用 OLD */
static uint16_t s_clock_cache_x;
static uint16_t s_clock_cache_y;
static uint16_t s_clock_cache_w;
static uint16_t s_clock_cache_h;

typedef struct {
    uint8_t *dst;
    uint16_t window_x;
    uint16_t window_y;
    uint16_t dst_stride;
    uint16_t height;
    bool failed;
} epd_clock_capture_ctx_t; /* 用上一时刻重绘，捕获 OLD */

typedef struct {
    epd_model_t *epd;
    uint8_t *old_bitmap;
    uint16_t old_stride;
    uint16_t hardware_x;
    uint16_t hardware_y;
    uint16_t window_x;
    uint16_t window_y;
    bool failed;
} epd_clock_diff_ctx_t; /* 推 NEW，并回写缓存 */

void epd_clock_refresh_invalidate(void)
{
    s_clock_last_valid = false;
    s_clock_cache_ready = false;
}

void epd_clock_refresh_mark_valid(uint32_t timestamp)
{
    s_clock_last_timestamp = timestamp;
    s_clock_last_valid = true;
}

bool epd_clock_refresh_is_valid(void)
{
    return s_clock_last_valid;
}

/* 按行拷贝一页（stride 为字节）。 */
static void epd_clock_store_page(uint8_t *dst, uint16_t dst_stride,
                                 const uint8_t *src, uint16_t src_stride,
                                 uint16_t h)
{
    for (uint16_t row = 0; row < h; row++) {
        memcpy(dst + (uint32_t)row * dst_stride,
               src + (uint32_t)row * src_stride,
               dst_stride);
    }
}

/* GFX 分页回调：把上一时刻画面存进 OLD 缓冲。 */
static void epd_clock_capture_cb(void *user_data, uint8_t *black, uint8_t *color,
                                 uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)color;
    epd_clock_capture_ctx_t *ctx = (epd_clock_capture_ctx_t *)user_data;
    epd_clock_page_push_t page;

    if (ctx == NULL) return;
    if (black == NULL ||
        !epd_clock_map_gfx_page(ctx->window_x, ctx->window_y,
                                0U, 0U, x, y, w, h, &page) ||
        page.src_stride != ctx->dst_stride ||
        (uint32_t)(y - ctx->window_y) + h > ctx->height ||
        (uint32_t)(y - ctx->window_y + h) * ctx->dst_stride >
            sizeof(s_clock_old_window)) {
        ctx->failed = true;
        return;
    }

    epd_clock_store_page(ctx->dst + (uint32_t)(y - ctx->window_y) * ctx->dst_stride,
                         ctx->dst_stride, black, page.src_stride, h);
}

/* GFX 分页回调：diff_push(OLD, NEW)，再把 NEW 存成下次的 OLD。 */
static void epd_clock_diff_cb(void *user_data, uint8_t *black, uint8_t *color,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)color;
    epd_clock_diff_ctx_t *ctx = (epd_clock_diff_ctx_t *)user_data;
    epd_clock_page_push_t page;

    if (ctx == NULL) return;
    if (ctx->epd == NULL || ctx->epd->drv == NULL ||
        ctx->epd->drv->diff_push == NULL || black == NULL ||
        !epd_clock_map_gfx_page(ctx->window_x, ctx->window_y,
                                ctx->hardware_x, ctx->hardware_y,
                                x, y, w, h, &page)) {
        ctx->failed = true;
        return;
    }

    uint8_t *old_bm =
        ctx->old_bitmap + (uint32_t)(y - ctx->window_y) * ctx->old_stride;
    if (!ctx->epd->drv->diff_push(ctx->epd, page.hw_x, page.hw_y, w, h,
                                  old_bm, black)) {
        ctx->failed = true;
        return;
    }

    epd_clock_store_page(old_bm, ctx->old_stride, black, page.src_stride, h);
}

/* 刷新一个局刷窗：必要时重绘 OLD，再 diff_push + refresh_partial。 */
static bool epd_clock_refresh_one(epd_model_t *epd, const gui_data_t *data,
                                  const clock_refresh_geom_t *geom,
                                  const clock_refresh_window_t *local)
{
    uint16_t x = 0U;
    uint16_t y = 0U;
    uint16_t w = 0U;
    uint16_t h = 0U;
    uint16_t window_x;
    uint16_t window_y;
    uint16_t width_bytes;
    bool reuse_old;

    GetTimeRefreshAreaFromLocal(data, local->x, local->y, local->w, local->h,
                                &x, &y, &w, &h);

    if (w == 0U || h == 0U || x < geom->origin_x || y < geom->origin_y ||
        (uint32_t)x + w > epd->width || (uint32_t)y + h > epd->height) {
        return false;
    }

    window_x = (uint16_t)(x - geom->origin_x);
    window_y = (uint16_t)(y - geom->origin_y);
    width_bytes = (uint16_t)(w / 8U);
    if ((uint32_t)window_x + w > (uint32_t)geom->canvas_w ||
        width_bytes == 0U || width_bytes > EPD_CLOCK_OLD_STRIDE_MAX ||
        h > EPD_CLOCK_OLD_HEIGHT_MAX ||
        (uint32_t)width_bytes * h > sizeof(s_clock_old_window)) {
        return false;
    }

    reuse_old = s_clock_cache_ready &&
                s_clock_cache_x == x && s_clock_cache_y == y &&
                s_clock_cache_w == w && s_clock_cache_h == h;

    if (!reuse_old) {
        gui_data_t old_data = *data;
        epd_clock_capture_ctx_t capture = {
            .dst = s_clock_old_window,
            .window_x = window_x,
            .window_y = window_y,
            .dst_stride = width_bytes,
            .height = h,
            .failed = false,
        };

        old_data.timestamp = s_clock_last_timestamp;
        DrawGUI_ClockOnly(&old_data, epd_clock_capture_cb, &capture);
        if (capture.failed) return false;
    }

    {
        epd_clock_diff_ctx_t ctx = {
            .epd = epd,
            .old_bitmap = s_clock_old_window,
            .old_stride = width_bytes,
            .hardware_x = x,
            .hardware_y = y,
            .window_x = window_x,
            .window_y = window_y,
            .failed = false,
        };
        s_clock_cache_ready = false;
        DrawGUI_ClockOnly(data, epd_clock_diff_cb, &ctx);
        if (ctx.failed) return false;
    }

    if (!epd->drv->refresh_partial(epd, x, y, w, h) || UC8179_HadBusyTimeout()) {
        return false;
    }

    s_clock_cache_ready = true;
    s_clock_cache_x = x;
    s_clock_cache_y = y;
    s_clock_cache_w = w;
    s_clock_cache_h = h;
    return true;
}

/* 按变化填 1～2 个窗，逐个 OLD/NEW 局刷；任一个失败则放弃。 */
bool epd_clock_refresh_try(epd_model_t *epd, const gui_data_t *data)
{
    clock_refresh_geom_t geom;
    clock_refresh_window_t wins[CLOCK_REFRESH_WINDOWS_MAX];
    uint8_t n;
    uint8_t i;

    if (epd == NULL || epd->drv == NULL || data == NULL ||
        !s_clock_last_valid || epd->drv->diff_push == NULL ||
        epd->drv->refresh_partial == NULL ||
        !epd_is_clock_mode(data->mode)) {
        return false;
    }

    geom = clock_refresh_geom_sized(data->mode == MODE_CLOCK,
                                    data->width, data->height);
    n = clock_refresh_local_windows(&geom, s_clock_last_timestamp, data->timestamp,
                                    wins, CLOCK_REFRESH_WINDOWS_MAX);
    if (n == 0U) {
        return false;
    }

    for (i = 0U; i < n; i++) {
        if (!epd_clock_refresh_one(epd, data, &geom, &wins[i])) {
            return false;
        }
    }

    s_clock_last_timestamp = data->timestamp;
    s_clock_last_valid = true;
    return true;
}
