#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "Adafruit_GFX.h"

/* Preserve the original full-circle rasterizer as a pixel equivalence oracle. */
static void reference_circle(Adafruit_GFX* gfx, int16_t x0, int16_t y0,
                             int16_t r, uint16_t color)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    GFX_drawPixel(gfx, x0, y0 + r, color);
    GFX_drawPixel(gfx, x0, y0 - r, color);
    GFX_drawPixel(gfx, x0 + r, y0, color);
    GFX_drawPixel(gfx, x0 - r, y0, color);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        GFX_drawPixel(gfx, x0 + x, y0 + y, color);
        GFX_drawPixel(gfx, x0 - x, y0 + y, color);
        GFX_drawPixel(gfx, x0 + x, y0 - y, color);
        GFX_drawPixel(gfx, x0 - x, y0 - y, color);
        GFX_drawPixel(gfx, x0 + y, y0 + x, color);
        GFX_drawPixel(gfx, x0 - y, y0 + x, color);
        GFX_drawPixel(gfx, x0 + y, y0 - x, color);
        GFX_drawPixel(gfx, x0 - y, y0 - x, color);
    }
}

static void test_circle_pixels(void)
{
    static const int16_t radii[] = {-2, 0, 1, 2, 3, 7, 20, 48};
    static const int16_t centers[][2] = {
        {20, 16}, {0, 0}, {-3, 12}, {44, 34}, {20, -6}, {20, 36}
    };
    static const int16_t page_heights[] = {32, 7};
    static const uint16_t colors[] = {GFX_BLACK, GFX_WHITE, GFX_RED};
    const unsigned rotations = GFX_FIXED_BWR_PANEL ? 1U : 4U;
    uint8_t expected[GFX_PAGE_BUFFER_SIZE];
    Adafruit_GFX gfx;

    for (unsigned three_color = 0; three_color < 2; three_color++) {
        for (unsigned rotation = 0; rotation < rotations; rotation++) {
            for (unsigned partial = 0; partial < 2; partial++) {
                for (unsigned height = 0; height < sizeof(page_heights) / sizeof(page_heights[0]); height++) {
                    if (three_color) {
                        GFX_begin_3c(&gfx, 40, 32, 2 * page_heights[height]);
                    } else {
                        GFX_begin(&gfx, 40, 32, page_heights[height]);
                    }
                    GFX_setRotation(&gfx, (GFX_Rotate)rotation);
                    if (partial) GFX_setWindow(&gfx, 8, 5, 24, 19);
                    else GFX_setWindow(&gfx, 0, 0, gfx._width, gfx._height);
                    GFX_firstPage(&gfx);
                    do {
                        for (unsigned r = 0; r < sizeof(radii) / sizeof(radii[0]); r++) {
                            for (unsigned center = 0; center < sizeof(centers) / sizeof(centers[0]); center++) {
                                for (unsigned color = 0; color < sizeof(colors) / sizeof(colors[0]); color++) {
                                    uint16_t background = colors[(color + 1U) % 3U];
                                    /* Include inactive bytes to detect writes outside this page. */
                                    memset(gfx.buffer, 0xa5, GFX_PAGE_BUFFER_SIZE);
                                    GFX_fillScreen(&gfx, background);
                                    reference_circle(&gfx, centers[center][0], centers[center][1],
                                                     radii[r], colors[color]);
                                    memcpy(expected, gfx.buffer, sizeof(expected));
                                    memset(gfx.buffer, 0xa5, GFX_PAGE_BUFFER_SIZE);
                                    GFX_fillScreen(&gfx, background);
                                    GFX_drawCircle(&gfx, centers[center][0], centers[center][1],
                                                   radii[r], colors[color]);
                                    assert(memcmp(expected, gfx.buffer, sizeof(expected)) == 0);
                                }
                            }
                        }
                    } while (GFX_nextPage(&gfx, NULL, NULL));
                    GFX_end(&gfx);
                }
            }
        }
    }
}

int main(void)
{
    Adafruit_GFX gfx = {0};
    uint8_t expected[128];
    GFX_begin(&gfx, 32, 32, 32);
    GFX_firstPage(&gfx);
    GFX_fillScreen(&gfx, GFX_WHITE);
    u8g2_font_placeholder_t p = {12,12,0,-2,8,-2,10,-1,13};
    GFX_setMissingFont(&gfx, &p);
    GFX_setTextColor(&gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(&gfx, 4, 12);
    GFX_puts(&gfx, "X");
    assert((gfx.buffer[9*4] & (0x80U >> 4)) == 0); /* Left edge. */
    assert((gfx.buffer[4*4+2] & (0x80U >> 4)) != 0); /* No spurious top edge. */

    GFX_fillScreen(&gfx, GFX_WHITE);
    memcpy(expected, gfx.buffer, sizeof(expected));
    GFX_drawFastHLine(&gfx, 4, 4, 0, GFX_BLACK);
    GFX_drawFastVLine(&gfx, 4, 4, 0, GFX_BLACK);
    GFX_drawFastHLine(&gfx, 4, 4, -3, GFX_BLACK);
    GFX_drawFastVLine(&gfx, 4, 4, -3, GFX_BLACK);
    assert(memcmp(expected, gfx.buffer, sizeof(expected)) == 0);
    GFX_drawLine(&gfx, -3, 4, 35, 4, GFX_BLACK);
    GFX_drawLine(&gfx, 9, -3, 9, 35, GFX_BLACK);
    memcpy(expected, gfx.buffer, sizeof(expected));
    GFX_fillScreen(&gfx, GFX_WHITE);
    GFX_drawFastHLine(&gfx, -3, 4, 39, GFX_BLACK);
    GFX_drawFastVLine(&gfx, 9, -3, 39, GFX_BLACK);
    assert(memcmp(expected, gfx.buffer, sizeof(expected)) == 0);
    GFX_end(&gfx);
    test_circle_pixels();
    puts("PASS: placeholder edges, clipped/empty/negative H/V lines and circle pixel equivalence");
    return 0;
}
