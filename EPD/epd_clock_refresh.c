#include "epd_clock_refresh.h"

#include <string.h>

#include "UC8179.h"
#include "clock_layout.h"
#include "epd_clock_window.h"
#include "epd_refresh_policy.h"

/* Preserve the established window geometry and submit it in bounded strips.
 * Minute carries use one MM window in split mode, two digits in full mode. */
#define EPD_CLOCK_OLD_STRIDE_MAX CLOCK_OLD_WINDOW_STRIDE_MAX
#define EPD_CLOCK_OLD_HEIGHT_MAX 208U

/* Regenerate OLD from the last successfully displayed time, one strip at a
 * time. OLD must survive while the shared GFX page buffer renders NEW. */
static uint8_t s_clock_old_window[512];
static uint32_t s_clock_last_timestamp;
static bool s_clock_last_valid;

typedef struct {
    uint8_t *dst;
    uint16_t window_x;
    uint16_t window_y;
    uint16_t dst_stride;
    uint16_t height;
    bool failed;
} epd_clock_capture_ctx_t; /* 用上一时刻重绘，捕获 OLD */

typedef struct {
    const epd_model_t *epd;
    uint8_t *old_bitmap;
    uint16_t old_stride;
    uint16_t hardware_x;
    uint16_t hardware_y;
    uint16_t window_x;
    uint16_t window_y;
    bool failed;
} epd_clock_diff_ctx_t; /* 提交本条带的 OLD/NEW */

void epd_clock_refresh_invalidate(void)
{
    s_clock_last_valid = false;
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

    if (ctx == NULL || ctx->failed) return;
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

/* GFX page callback: submit this strip's regenerated OLD and NEW data. */
static void epd_clock_diff_cb(void *user_data, uint8_t *black, uint8_t *color,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)color;
    epd_clock_diff_ctx_t *ctx = (epd_clock_diff_ctx_t *)user_data;
    epd_clock_page_push_t page;

    if (ctx == NULL || ctx->failed) return;
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

}

/* Regenerate OLD/NEW in strips, then trigger one physical window refresh. */
static bool epd_clock_refresh_one(const epd_model_t *epd, const gui_data_t *data,
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
        h > EPD_CLOCK_OLD_HEIGHT_MAX) {
        return false;
    }

    for (uint16_t row = 0U; row < h;) {
        uint16_t rows = sizeof(s_clock_old_window) / width_bytes;
        if (rows > h - row) rows = h - row;
        /* Both renders use exactly the same logical strip. The driver gets
         * absolute panel coordinates; refresh only after the whole window. */
        GetTimeRefreshAreaFromLocal(data, window_x, window_y + row, w, rows,
                                    NULL, NULL, NULL, NULL);
        gui_data_t old_data = *data;
        epd_clock_capture_ctx_t capture = {
            .dst = s_clock_old_window,
            .window_x = window_x,
            .window_y = window_y + row,
            .dst_stride = width_bytes,
            .height = rows,
            .failed = false,
        };

        old_data.timestamp = s_clock_last_timestamp;
        DrawGUI_ClockOnly(&old_data, epd_clock_capture_cb, &capture);
        if (capture.failed) return false;
        epd_clock_diff_ctx_t ctx = {
            .epd = epd,
            .old_bitmap = s_clock_old_window,
            .old_stride = width_bytes,
            .hardware_x = x,
            .hardware_y = y + row,
            .window_x = window_x,
            .window_y = window_y + row,
            .failed = false,
        };
        DrawGUI_ClockOnly(data, epd_clock_diff_cb, &ctx);
        if (ctx.failed) return false;
        row += rows;
    }

    if (!epd->drv->refresh_partial(epd, x, y, w, h) || UC8179_HadBusyTimeout()) {
        return false;
    }

    return true;
}

/* 按变化填 1～2 个窗，逐个 OLD/NEW 局刷；任一个失败则放弃。 */
bool epd_clock_refresh_try(const epd_model_t *epd, const gui_data_t *data)
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
            epd_clock_refresh_invalidate();
            return false;
        }
    }

    s_clock_last_timestamp = data->timestamp;
    s_clock_last_valid = true;
    return true;
}
