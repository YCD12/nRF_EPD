#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u8g2_font.h"

typedef struct {
    const uint8_t* data;
    size_t length;
} file_font_t;

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

static void assert_lunar_chars(const char* path)
{
    static const uint16_t required[] = {
        0x516D, /* 六 */
        0x6708, /* 月 */
        0x516B, /* 八 */
        0x95F0, /* 闰 */
        0x5341, /* 十 */
        0x5EFF, /* 廿 */
        0x4E5D, /* 九 */
    };
    uint8_t data[6000];
    file_font_t file_font;
    u8g2_font_source_t source;
    u8g2_font_t context;
    size_t i;

    file_font.data = data;
    file_font.length = load_file(path, data, sizeof(data));
    source.data = NULL;
    source.length = (uint32_t)file_font.length;
    source.read = read_font;
    source.context = &file_font;
    source.revision = 1U;
    memset(&context, 0, sizeof(context));
    u8g2_SetExternalFont(&context, &source);

    for (i = 0U; i < sizeof(required) / sizeof(required[0]); i++) {
        assert(u8g2_IsGlyph(&context, required[i]) == 1U);
    }
}

static void assert_ascii_chars(const char* path, const uint16_t* required, size_t count)
{
    uint8_t data[6000];
    file_font_t file_font;
    u8g2_font_source_t source;
    u8g2_font_t context;
    size_t i;

    file_font.data = data;
    file_font.length = load_file(path, data, sizeof(data));
    source.data = NULL;
    source.length = (uint32_t)file_font.length;
    source.read = read_font;
    source.context = &file_font;
    source.revision = 1U;
    memset(&context, 0, sizeof(context));
    u8g2_SetExternalFont(&context, &source);

    for (i = 0U; i < count; i++) {
        assert(u8g2_IsGlyph(&context, required[i]) == 1U);
    }
}

int main(void)
{
    static const uint16_t helvb18_ui_chars[] = {
        (uint16_t)' ', (uint16_t)'-',
        (uint16_t)'0', (uint16_t)'1', (uint16_t)'2', (uint16_t)'3', (uint16_t)'4',
        (uint16_t)'5', (uint16_t)'6', (uint16_t)'7', (uint16_t)'8', (uint16_t)'9',
    };

    assert_lunar_chars("tools/dist/u8g2_font_wqy9_t_lunar.bin");
    assert_lunar_chars("tools/dist/u8g2_font_wqy12_t_lunar.bin");
    assert_ascii_chars("tools/dist/u8g2_font_wqy9_t_lunar.bin",
                       (const uint16_t[]){(uint16_t)' '}, 1U);
    assert_ascii_chars("tools/dist/u8g2_font_helvB18_tn.bin",
                       helvb18_ui_chars,
                       sizeof(helvb18_ui_chars) / sizeof(helvb18_ui_chars[0]));
    puts("PASS: font files contain required UI glyphs");
    return 0;
}
