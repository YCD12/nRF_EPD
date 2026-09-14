#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "u8g2_font.h"

static unsigned line_calls;

static void draw_line(u8g2_font_t* u8g2, int16_t x, int16_t y, int16_t len,
                      uint8_t dir, uint16_t color)
{
    (void)u8g2;
    (void)x;
    (void)y;
    (void)len;
    (void)dir;
    (void)color;
    line_calls++;
}

int main(void)
{
    const u8g2_font_placeholder_t placeholder = {12, 12, 0, -2, 8, -2, 10, -1, 13};
    u8g2_font_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.draw_hv_line = draw_line;
    u8g2_SetMissingFont(&ctx, &placeholder);

    assert(u8g2_DrawGlyph(&ctx, 10, 20, (uint16_t)' ') == 0);
    assert(line_calls == 0U);
    assert(u8g2_IsGlyph(&ctx, 0x4E2D) == 0U);
    assert(u8g2_GetGlyphWidth(&ctx, 0x4E2D) == 0);
    assert(u8g2_DrawGlyph(&ctx, 10, 20, 0x4E2D) == 13);
    assert(line_calls > 0U);
    puts("PASS: missing font uses fixed-width placeholder");
    return 0;
}
