#include "epd_clock_diff.h"

#include "Lunar.h"
#include "ui_runtime.h"

#define CLOCK_DIFF_MARGIN_Y 8U
#define CLOCK_DIFF_DIGITS   2U

typedef struct {
    display_mode_t mode;
    uint8_t hour;
    uint8_t minute;
    bool valid;
} clock_state_t;

static clock_state_t s_clock_prev;

static bool clock_mode_supported(display_mode_t mode) {
    return mode == MODE_CLOCK || mode == MODE_CLOCK_CALENDAR;
}

void epd_clock_diff_seed(display_mode_t mode, uint32_t timestamp) {
    tm_t now = {0};

    if (!clock_mode_supported(mode)) {
        s_clock_prev.valid = false;
        return;
    }

    transformTime(timestamp, &now);
    s_clock_prev.mode = mode;
    s_clock_prev.hour = now.tm_hour;
    s_clock_prev.minute = now.tm_min;
    s_clock_prev.valid = true;
}

void epd_clock_diff_invalidate(void) {
    s_clock_prev.valid = false;
}

static void draw_time_tile(uint8_t *bitmap,
                           uint16_t bitmap_w,
                           uint16_t total_h,
                           uint16_t tile_h,
                           uint16_t page,
                           uint16_t offset_x,
                           uint16_t scale,
                           uint8_t hour,
                           uint8_t minute) {
    Adafruit_GFX gfx;
    uint16_t digit_block_w = CLOCK_DIFF_DIGITS * (11U * scale + 2U) - 2U * scale;
    uint16_t colon_x = offset_x + digit_block_w + 2U * scale;
    uint16_t minute_x = colon_x + 4U * scale;

    GFX_begin(&gfx, bitmap_w, total_h, tile_h);
    gfx.buffer = bitmap;
    gfx.current_page = page;
    GFX_fillScreen(&gfx, GFX_WHITE);

    Draw7Number(&gfx, hour, offset_x, CLOCK_DIFF_MARGIN_Y,
                (char)scale, GFX_BLACK, GFX_WHITE, CLOCK_DIFF_DIGITS);
    GFX_fillRect(&gfx, colon_x,
                 CLOCK_DIFF_MARGIN_Y + (9U * scale) / 2U + 1U,
                 2U * scale, 2U * scale, GFX_BLACK);
    GFX_fillRect(&gfx, colon_x,
                 CLOCK_DIFF_MARGIN_Y + (27U * scale) / 2U + 3U,
                 2U * scale, 2U * scale, GFX_BLACK);
    Draw7Number(&gfx, minute, minute_x, CLOCK_DIFF_MARGIN_Y,
                (char)scale, GFX_BLACK, GFX_WHITE, CLOCK_DIFF_DIGITS);
}

bool epd_clock_diff_try_partial(epd_model_t *epd, uint32_t timestamp,
                                display_mode_t mode) {
    ui_mode_record_t cached_mode;
    const ui_component_record_t *components = NULL;
    const ui_component_record_t *time_component = NULL;
    uint8_t component_count = 0;
    tm_t now = {0};

    if (epd == NULL || epd->drv == NULL ||
        !clock_mode_supported(mode) ||
        !s_clock_prev.valid || s_clock_prev.mode != mode ||
        epd->drv->diff_push == NULL || epd->drv->refresh_partial == NULL) {
        return false;
    }

    transformTime(timestamp, &now);
    if (now.tm_hour == s_clock_prev.hour &&
        now.tm_min == s_clock_prev.minute) {
        return true;
    }

    if (ui_runtime_load_render_cache((uint8_t)mode, epd->width, epd->height,
                                     &cached_mode, &components,
                                     &component_count) != NRF_SUCCESS ||
        components == NULL) {
        return false;
    }

    for (uint8_t i = 0; i < component_count; i++) {
        if (components[i].type != UI_COMP_TIME_7SEG ||
            (components[i].flags & UI_FLAG_PARTIAL_ONLY) != 0U) {
            continue;
        }
        if (time_component != NULL) return false;
        time_component = &components[i];
    }
    if (time_component == NULL ||
        time_component->p0 < UI_TIME_SCALE_MIN ||
        time_component->p0 > UI_TIME_SCALE_MAX ||
        time_component->p1 != CLOCK_DIFF_DIGITS) {
        return false;
    }

    uint16_t scale = (uint16_t)time_component->p0;
    uint16_t content_w = 46U * scale + 8U;
    uint16_t content_h = 20U * scale + 4U;
    int32_t region_x = time_component->x;
    int32_t region_y = (int32_t)time_component->y - CLOCK_DIFF_MARGIN_Y;
    if (region_x < 0 || region_y < 0) return false;

    uint16_t aligned_x = (uint16_t)region_x & (uint16_t)~7U;
    uint16_t offset_x = (uint16_t)region_x - aligned_x;
    uint32_t needed_w = (uint32_t)offset_x + content_w;
    uint16_t region_w = (uint16_t)((needed_w + 7U) & ~7U);
    uint16_t region_h = content_h + 2U * CLOCK_DIFF_MARGIN_Y;

    if (region_w < 16U || region_h < 2U ||
        (uint32_t)aligned_x + region_w > epd->width ||
        (uint32_t)region_y + region_h > epd->height) {
        return false;
    }

    uint16_t width_bytes = region_w / 8U;
    size_t buffer_size = GFX_get_buffer_size();
    uint16_t tile_h = (uint16_t)(buffer_size / (2U * width_bytes));
    if (tile_h > region_h) tile_h = region_h;
    while (tile_h > 2U && (region_h % tile_h) == 1U) tile_h--;
    if (tile_h < 2U || (region_h % tile_h) == 1U) return false;

    size_t plane_size = (size_t)width_bytes * tile_h;
    if (plane_size * 2U > buffer_size) return false;

    uint8_t *old_bitmap = GFX_get_buffer();
    uint8_t *new_bitmap = old_bitmap + plane_size;
    uint16_t tile_y = 0U;
    uint16_t page = 0U;

    while (tile_y < region_h) {
        uint16_t rows = region_h - tile_y;
        if (rows > tile_h) rows = tile_h;

        draw_time_tile(old_bitmap, region_w, region_h, tile_h, page,
                       offset_x, scale,
                       s_clock_prev.hour, s_clock_prev.minute);
        draw_time_tile(new_bitmap, region_w, region_h, tile_h, page,
                       offset_x, scale, now.tm_hour, now.tm_min);

        if (!epd->drv->diff_push(epd, aligned_x,
                                  (uint16_t)region_y + tile_y,
                                  region_w, rows,
                                  old_bitmap, new_bitmap)) {
            return false;
        }

        tile_y += rows;
        page++;
    }

    if (!epd->drv->refresh_partial(epd, aligned_x, (uint16_t)region_y,
                                    region_w, region_h)) {
        return false;
    }

    s_clock_prev.hour = now.tm_hour;
    s_clock_prev.minute = now.tm_min;
    return true;
}
