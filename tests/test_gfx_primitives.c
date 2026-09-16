#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "Adafruit_GFX.h"

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
    puts("PASS: placeholder edges and clipped/empty/negative H/V lines");
    return 0;
}
