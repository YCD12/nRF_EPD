#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "u8g2_font.h"

static uint8_t bytes[96];
static int read_bytes(const void* context, uint32_t offset, uint8_t* data, size_t length)
{
    (void)context;
    assert(offset <= sizeof(bytes) && length <= sizeof(bytes) - offset);
    memcpy(data, bytes + offset, length);
    return 0;
}

int main(void)
{
    u8g2_font_t ctx = {0};
    bytes[0] = 1U;
    for (unsigned i = 2; i <= 7; i++) bytes[i] = 1U;
    bytes[8] = 4U;
    bytes[9] = bytes[10] = 12U;
    bytes[23] = 'A'; bytes[24] = 3U; bytes[25] = 0x9CU;
    u8g2_font_source_t source = {NULL, sizeof(bytes), read_bytes, NULL, 1U};
    u8g2_SetExternalFont(&ctx, &source);
    assert(u8g2_GetGlyphWidth(&ctx, 'A') == 1);
    /* An incomplete metrics stream reports missing/error, not a negative
     * advance that moves subsequent text backwards. */
    bytes[24] = 2U;
    u8g2_InvalidateFontCache(&ctx);
    int8_t advance = 42;
    assert(!u8g2_GetGlyphWidthIfPresent(&ctx, 'A', &advance));
    assert(advance == 42);
    u8g2_font_placeholder_t placeholder = {12, 12, 0, -2, 8, -2, 10, -1, 13};
    u8g2_SetPlaceholder(&ctx, &placeholder);
    assert(u8g2_DrawGlyph(&ctx, 0, 0, 'A') == 13);
    /* A valid first Unicode block followed by a zero-size second block. */
    memset(bytes + 23, 0, sizeof(bytes) - 23);
    bytes[22] = 2U;
    bytes[26] = 8U; bytes[27] = 1U;
    bytes[30] = 6U; bytes[31] = bytes[32] = 0xFFU;
    bytes[33] = 1U; bytes[35] = 4U;
    bytes[39] = 2U;
    u8g2_InvalidateFontCache(&ctx);
    assert(!u8g2_IsGlyph(&ctx, 0x0201U));
    /* Missing sentinel / zero index delta is bounded too. */
    bytes[30] = 0U;
    u8g2_InvalidateFontCache(&ctx);
    assert(!u8g2_IsGlyph(&ctx, 0x0201U));
    puts("PASS: glyph byte boundary, truncated metrics and bounded Unicode scan");
    return 0;
}
