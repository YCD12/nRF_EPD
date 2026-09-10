#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fonts.h"
#include "u8g2_font.h"

static void load_font(u8g2_font_t* ctx, const uint8_t* font)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->font = (const uint8_t*)1;
    u8g2_SetFont(ctx, font);
}

int main(void)
{
    u8g2_font_t ctx;

    load_font(&ctx, u8g2_font_wqy9_t_lunar);
    assert(u8g2_IsGlyph(&ctx, (uint16_t)'A'));
    assert(u8g2_IsGlyph(&ctx, (uint16_t)'z'));
    assert(u8g2_IsGlyph(&ctx, (uint16_t)'0'));

    load_font(&ctx, u8g2_font_wqy12_t_lunar);
    assert(u8g2_IsGlyph(&ctx, (uint16_t)'0'));
    assert(u8g2_IsGlyph(&ctx, (uint16_t)'9'));
    assert(!u8g2_IsGlyph(&ctx, (uint16_t)'A'));

    puts("PASS: wqy ascii (letters in wqy9 only)");
    return 0;
}
