#ifndef EPD_CLOCK_WINDOW_H
#define EPD_CLOCK_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * DrawGUI_ClockOnly 分页前会调用 GFX_setWindow()，窗口原点变到 (0,0)，
 * 每页 stride = page_width / 8。硬件 x/y 仍是屏幕坐标，源 x/y 是页内坐标。
 */
typedef struct {
    uint16_t hw_x;       /* 屏幕坐标 */
    uint16_t hw_y;
    uint16_t src_stride; /* 页宽/8 */
    uint16_t src_x;      /* 页内，setWindow 后为 0 */
    uint16_t src_y;
} epd_clock_page_push_t;

/* 把 GFX 分页矩形映射到硬件窗口；宽须 8 对齐。 */
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
