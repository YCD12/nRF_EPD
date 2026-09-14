#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Adafruit_GFX.h"

typedef struct {
    const uint8_t* data;
    size_t length;
} file_font_t;

static unsigned line_calls;

static void count_line(u8g2_font_t* u8g2, int16_t x, int16_t y, int16_t len,
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

static int read_font(const void* context, uint32_t offset, uint8_t* data, size_t length)
{
    const file_font_t* font = (const file_font_t*)context;

    if (font == NULL || data == NULL || offset > font->length ||
        length > font->length - offset) {
        return -1;
    }
    memcpy(data, font->data + offset, length);
    return 0;
}

static size_t load_file(const char* path, uint8_t* data, size_t capacity)
{
    FILE* file = fopen(path, "rb");
    size_t length;

    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    length = (size_t)ftell(file);
    assert(length <= capacity);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    assert(fread(data, 1U, length, file) == length);
    assert(fclose(file) == 0);
    return length;
}

static void assert_lunar_text(const char* path)
{
    uint8_t font_data[6000];
    file_font_t file_font;
    u8g2_font_source_t source;
    u8g2_font_placeholder_t placeholder = {12, 12, 0, -2, 8, -2, 10, -1, 13};
    Adafruit_GFX gfx;
    int16_t expected_width;

    file_font.data = font_data;
    file_font.length = load_file(path, font_data, sizeof(font_data));
    source = (u8g2_font_source_t){NULL, (uint32_t)file_font.length,
                                  read_font, &file_font, 1U};

    GFX_begin(&gfx, 800, 480, 25);
    GFX_setExternalFont(&gfx, &source);
    GFX_setFontPlaceholder(&gfx, &placeholder);
    assert(GFX_hasGlyph(&gfx, 0x516BU)); /* 八 */
    assert(GFX_hasGlyph(&gfx, 0x6708U)); /* 月 */
    assert(GFX_hasGlyph(&gfx, 0x521DU)); /* 初 */
    assert(GFX_hasGlyph(&gfx, 0x4E00U)); /* 一 */
    assert(GFX_hasGlyph(&gfx, 0x5341U)); /* 十: 2025-08-11 lunar date */
    assert(GFX_hasGlyph(&gfx, 0x516BU)); /* 八: 2025-08-11 lunar date */
    assert(GFX_hasGlyph(&gfx, 0x95F0U)); /* 闰: 2025-08-11 is leap June */

    expected_width = GFX_getUTF8Width(&gfx, "八月初一");
    assert(expected_width > 0);
    GFX_setCursor(&gfx, 0, 20);
    assert(GFX_printf(&gfx, "%s", "八月初一") == strlen("八月初一"));
    GFX_setCursor(&gfx, 0, 40);
    assert(GFX_printf(&gfx, "%s", "闰六月十八") == strlen("闰六月十八"));
    GFX_setCursor(&gfx, 0, 60);
    assert(GFX_printf(&gfx, "%s", "六月廿九") == strlen("六月廿九"));
    GFX_end(&gfx);
}

static void assert_august_11_ui_chars(const char* path)
{
    static const uint16_t required[] = {
        0x516D, /* 六: 2026-08-11 lunar month */
        0x6708, /* 月 */
        0x5341, /* 十: 2025-08-11 lunar day tens */
        0x516B, /* 八: 2025-08-11 lunar day units */
        0x95F0, /* 闰: leap month prefix */
        0x5EFF, /* 廿: lunar day tens */
        0x4E5D, /* 九: lunar day units */
        0x661F, /* 星 */
        0x671F, /* 期 */
        0x65E5, /* 日 */
        0x4E19, /* 丙 */
        0x5348, /* 午 */
        0x5E74, /* 年 */
        0x73ED, /* 班 */
        0x4F11, /* 休 */
        0x2103, /* ℃ */
    };
    uint8_t font_data[6000];
    file_font_t file_font;
    u8g2_font_source_t source;
    Adafruit_GFX gfx;
    size_t i;

    file_font.data = font_data;
    file_font.length = load_file(path, font_data, sizeof(font_data));
    source = (u8g2_font_source_t){NULL, (uint32_t)file_font.length,
                                  read_font, &file_font, 1U};

    GFX_begin(&gfx, 800, 480, 25);
    GFX_setExternalFont(&gfx, &source);
    for (i = 0U; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!GFX_hasGlyph(&gfx, required[i])) {
            fprintf(stderr, "%s missing U+%04X\\n", path, required[i]);
        }
        assert(GFX_hasGlyph(&gfx, required[i]));
    }
    GFX_end(&gfx);
}

static void assert_wqy12_space_is_invisible(const char* path)
{
    uint8_t font_data[6000];
    file_font_t file_font;
    u8g2_font_source_t source;
    u8g2_font_placeholder_t placeholder = {16, 16, 0, -2, 11, -3, 12, -4, 17};
    u8g2_font_t u8g2;

    file_font.data = font_data;
    file_font.length = load_file(path, font_data, sizeof(font_data));
    source = (u8g2_font_source_t){NULL, (uint32_t)file_font.length,
                                  read_font, &file_font, 1U};

    memset(&u8g2, 0, sizeof(u8g2));
    u8g2.draw_hv_line = count_line;
    u8g2_SetExternalFont(&u8g2, &source);
    u8g2_SetPlaceholder(&u8g2, &placeholder);
    line_calls = 0U;

    assert(u8g2_IsGlyph(&u8g2, (uint16_t)' ') == 0U);
    assert(u8g2_DrawGlyph(&u8g2, 0, 20, (uint16_t)' ') == 0);
    assert(line_calls == 0U);

    {
        Adafruit_GFX gfx;
        int16_t text_width;

        GFX_begin(&gfx, 800, 480, 25);
        GFX_setExternalFont(&gfx, &source);
        GFX_setFontPlaceholder(&gfx, &placeholder);
        text_width = GFX_getUTF8Width(&gfx, "八月初一");
        assert(text_width > 0);
        assert(GFX_getUTF8Width(&gfx, " 八月初一") == text_width);
        /* A trailing formatting space must not erase the last real glyph's
         * pixel-width correction.  A missing trailing glyph must keep that
         * correction and then add exactly one placeholder advance. */
        assert(GFX_getUTF8Width(&gfx, "八月初一 ") == text_width);
        assert(!GFX_hasGlyph(&gfx, 0x2603U)); /* snowman is absent */
        assert(GFX_getUTF8Width(&gfx, "八\xE2\x98\x83") ==
               GFX_getUTF8Width(&gfx, "八") + placeholder.advance);
        GFX_setCursor(&gfx, 0, 20);
        (void)GFX_printf(&gfx, "%s", "八月初一");
        {
            int16_t plain_tx = gfx.tx;

            GFX_setCursor(&gfx, 0, 40);
            (void)GFX_printf(&gfx, "%s", " 八月初一");
            assert(gfx.tx == plain_tx);
        }
        GFX_end(&gfx);
    }
}

int main(void)
{
    assert_lunar_text("tools/dist/u8g2_font_wqy9_t_lunar.bin");
    assert_lunar_text("tools/dist/u8g2_font_wqy12_t_lunar.bin");
    assert_august_11_ui_chars("tools/dist/u8g2_font_wqy9_t_lunar.bin");
    assert_august_11_ui_chars("tools/dist/u8g2_font_wqy12_t_lunar.bin");
    assert_wqy12_space_is_invisible("tools/dist/u8g2_font_wqy12_t_lunar.bin");
    puts("PASS: external font UTF-8 path renders lunar text and whitespace");
    return 0;
}
