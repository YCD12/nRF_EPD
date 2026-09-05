#ifndef EPD_CLOCK_WINDOW_H
#define EPD_CLOCK_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * DrawGUI_ClockOnly calls GFX_setWindow() before paging.  That transposes the
 * window to (0,0) and packs each page with stride = page_width / 8.
 * Hardware x/y stay in screen space; source x/y stay page-local.
 */
typedef struct {
    uint16_t hw_x;
    uint16_t hw_y;
    uint16_t src_stride;
    uint16_t src_x;
    uint16_t src_y;
} epd_clock_page_push_t;

static inline bool epd_clock_map_gfx_page(uint16_t window_x, uint16_t window_y,
                                          uint16_t hardware_x, uint16_t hardware_y,
                                          uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h,
                                          epd_clock_page_push_t *out)
{
    if (out == NULL || x != window_x || y < window_y ||
        (w & 7U) != 0U || w < 8U || h == 0U) {
        return false;
    }

    out->hw_x = hardware_x;
    out->hw_y = (uint16_t)(hardware_y + (y - window_y));
    out->src_stride = (uint16_t)(w / 8U);
    out->src_x = 0U;
    out->src_y = 0U;
    return true;
}

#endif
