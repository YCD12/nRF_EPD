#include "epd_clock_diff.h"
#include "GUI.h"
#include "Lunar.h"
#include "ui_runtime.h"

// Access the shared GFX page buffer through the public accessor.
// Avoids a cross-layer extern on an internal symbol.

typedef struct {
    uint8_t h1;
    uint8_t h2;
    uint8_t m1;
    uint8_t m2;
    bool valid;
} clock_state_t;

static clock_state_t s_clock_prev = {0, 0, 0, 0, false};

void epd_clock_diff_seed(uint8_t hh, uint8_t mm) {
    s_clock_prev.h1 = hh / 10;
    s_clock_prev.h2 = hh % 10;
    s_clock_prev.m1 = mm / 10;
    s_clock_prev.m2 = mm % 10;
    s_clock_prev.valid = true;
}

void epd_clock_diff_invalidate(void) {
    s_clock_prev.valid = false;
}

static void draw_digit_bitmap(Adafruit_GFX *gfx, uint8_t *bm, uint16_t w, uint16_t h, uint8_t val, uint16_t offset_x, uint16_t cS, uint16_t margin_y) {
    GFX_begin(gfx, w, h, h);
    gfx->buffer = bm;
    GFX_firstPage(gfx);
    GFX_fillScreen(gfx, GFX_WHITE);
    Draw7Number(gfx, val, offset_x, margin_y, (char)cS, GFX_BLACK, GFX_WHITE, 1);
}

/**
 * @brief 绘制时钟冒号到局部缓冲区
 * @param gfx 临时图形句柄
 * @param bm 目标图像缓冲区指针
 * @param w 缓冲区宽度 (4 * cS 像素)
 * @param h 缓冲区高度
 * @param cS 缩放因子
 * @param margin_y 垂直边距
 */
static void draw_colon_bitmap(Adafruit_GFX *gfx, uint8_t *bm, uint16_t w, uint16_t h, uint16_t cS, uint16_t margin_y) {
    GFX_begin(gfx, w, h, h);
    gfx->buffer = bm;
    GFX_firstPage(gfx);
    GFX_fillScreen(gfx, GFX_WHITE);
    GFX_fillRect(gfx, 0, margin_y + (9 * cS) / 2 + 1, 2 * cS, 2 * cS, GFX_BLACK);
    GFX_fillRect(gfx, 0, margin_y + (27 * cS) / 2 + 3, 2 * cS, 2 * cS, GFX_BLACK);
}

bool epd_clock_diff_try_partial(epd_model_t *epd, uint32_t timestamp, display_mode_t mode) {
    // Only perform clock-specific partial refresh in MODE_CLOCK_CALENDAR
    // Require diff_push in vtable — non-partial drivers won't have it.
    if (mode != MODE_CLOCK_CALENDAR || !s_clock_prev.valid ||
        epd->drv->diff_push == NULL) {
        return false;
    }

    tm_t tm_now = {0};
    transformTime(timestamp, &tm_now);

    uint8_t new_vals[4];
    new_vals[0] = tm_now.tm_hour / 10;
    new_vals[1] = tm_now.tm_hour % 10;
    new_vals[2] = tm_now.tm_min / 10;
    new_vals[3] = tm_now.tm_min % 10;

    uint8_t old_vals[4] = {s_clock_prev.h1, s_clock_prev.h2, s_clock_prev.m1, s_clock_prev.m2};

    // Query scale and position parameters dynamically from ui_runtime cache.
    // If the cache is unavailable, abort and let the caller fall back to full refresh.
    int16_t comp_x, comp_y;
    uint16_t cS;

    ui_mode_record_t cached_mode;
    const ui_component_record_t* cached_components = NULL;
    uint8_t cached_component_count = 0;
    ret_code_t ui_ret = ui_runtime_load_render_cache(MODE_CLOCK_CALENDAR, epd->width, epd->height,
                                                     &cached_mode, &cached_components,
                                                     &cached_component_count);
    if (ui_ret != NRF_SUCCESS || cached_components == NULL) {
        return false;
    }

    bool found_time_comp = false;
    for (uint8_t i = 0; i < cached_component_count; i++) {
        if (cached_components[i].type == UI_COMP_TIME_7SEG) {
            comp_x = cached_components[i].x;
            comp_y = cached_components[i].y;
            cS = (uint16_t)cached_components[i].p0;
            found_time_comp = true;
            break;
        }
    }
    if (!found_time_comp) {
        return false;
    }

    uint16_t margin_y = 8;
    uint16_t digit_h = 20 * cS + 4 + 2 * margin_y;
    uint16_t digit_w = 11 * cS + 2;
    uint16_t align_w = ((digit_w + 7) / 8) * 8;

    int16_t min_x = epd->width;
    int16_t max_x = 0;

    int16_t x_offsets[4];
    x_offsets[0] = 0;
    x_offsets[1] = digit_w;
    x_offsets[2] = 2 * digit_w + 4 * cS;
    x_offsets[3] = 2 * digit_w + 4 * cS + digit_w;

    // Partition the shared GFX page buffer into old / new digit bitmaps.
    uint8_t *buf = GFX_get_buffer();
    uint32_t single_digit_size = (align_w / 8) * digit_h;
    uint8_t *old_bm = buf;
    uint8_t *new_bm = buf + single_digit_size;

    bool min_tens_changed = (new_vals[2] != old_vals[2]);  // 分钟十位是否改变 | Whether minute tens digit has changed
    bool hour_changed = (new_vals[0] != old_vals[0] || new_vals[1] != old_vals[1]);  // 小时是否改变 | Whether hour digits have changed
    bool changed = false;

    for (int i = 0; i < 4; i++) {
        if (new_vals[i] == old_vals[i]) continue;

        int16_t rx = comp_x + x_offsets[i];
        int16_t aligned_x = (rx / 8) * 8;
        uint16_t offset_x = rx - aligned_x;

        Adafruit_GFX temp_gfx;

        draw_digit_bitmap(&temp_gfx, old_bm, align_w, digit_h, old_vals[i], offset_x, cS, margin_y);
        draw_digit_bitmap(&temp_gfx, new_bm, align_w, digit_h, new_vals[i], offset_x, cS, margin_y);

        epd->drv->diff_push(epd, aligned_x, comp_y - margin_y, align_w, digit_h, old_bm, new_bm);

        if (aligned_x < min_x) min_x = aligned_x;
        if (aligned_x + align_w > max_x) max_x = aligned_x + align_w;
        changed = true;
    }

    if (changed) {
        if (max_x > min_x && epd->drv->refresh_partial) {
            // 如果分钟十位改变，或者小时改变，主动将窗口左边界扩展至覆盖冒号位置
            if (min_tens_changed || hour_changed) {
                int16_t colon_x = comp_x + 2 * digit_w;
                uint16_t colon_w = 4 * cS;
                Adafruit_GFX temp_gfx;
                draw_colon_bitmap(&temp_gfx, old_bm, colon_w, digit_h, cS, margin_y);
                draw_colon_bitmap(&temp_gfx, new_bm, colon_w, digit_h, cS, margin_y);
                epd->drv->diff_push(epd, colon_x, comp_y - margin_y, colon_w, digit_h, old_bm, new_bm);

                if (min_x > colon_x) {
                    min_x = colon_x;
                }
            }
            epd->drv->refresh_partial(epd, min_x, comp_y - margin_y, max_x - min_x, digit_h);
        }
        s_clock_prev.h1 = new_vals[0];
        s_clock_prev.h2 = new_vals[1];
        s_clock_prev.m1 = new_vals[2];
        s_clock_prev.m2 = new_vals[3];
    }
    return true;
}
