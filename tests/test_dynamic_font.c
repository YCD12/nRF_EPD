#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "resource_layout.h"
#include "resource_store.h"

/* Browser-produced FPK1 bytes are consumed by the real font validator and
 * renderer. Only SPI and the outer resource package are mocked here. */
static uint8_t font_pack[RS_SIZE_FONTS];
static uint32_t font_pack_length;
static uint32_t font_revision = 1U;
static uint8_t image[64][64];

void app_feed_wdt(void) { }
ret_code_t fm25q04b_init(void) { return NRF_SUCCESS; }
ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t *id)
{
    *id = (fm25q04b_jedec_id_t){0xEFU, 0x40U, 0x18U};
    return NRF_SUCCESS;
}
ret_code_t fm25q04b_read(uint32_t address, void *data, size_t length)
{
    uint32_t base = RS_OFFSET_FONTS + RS_HEADER_SIZE;
    if (address < base || address - base > font_pack_length ||
        length > font_pack_length - (address - base)) return NRF_ERROR_INVALID_ADDR;
    memcpy(data, font_pack + address - base, length);
    return NRF_SUCCESS;
}
int fm25q04b_read_region(const void *context, uint32_t offset,
                       uint8_t *data, size_t length)
{
    const fm25q04b_region_t *region = context;
    if (offset > region->length || length > region->length - offset)
        return NRF_ERROR_INVALID_ADDR;
    return fm25q04b_read(region->base_address + offset, data, length);
}
rs_result_t resource_store_mount(void) { return RS_OK; }
rs_result_t resource_store_find(uint16_t id, rs_partition_t *partition)
{
    if (id != RS_PART_FONTS) return RS_NOT_FOUND;
    *partition = (rs_partition_t){RS_PART_FONTS, 7U, 1U, 0U,
                                 RS_OFFSET_FONTS, RS_SIZE_FONTS};
    return RS_OK;
}
rs_result_t resource_store_info(uint16_t id, rs_package_info_t *package)
{
    if (id != RS_PART_FONTS) return RS_NOT_FOUND;
    *package = (rs_package_info_t){font_revision, font_pack_length, 0U};
    return RS_OK;
}
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    if (id != RS_PART_FONTS) return RS_NOT_FOUND;
    *length = font_pack_length;
    return RS_OK;
}
rs_result_t resource_store_read(uint16_t id, uint32_t offset,
                                void *data, size_t length)
{
    if (id != RS_PART_FONTS || offset > font_pack_length ||
        length > font_pack_length - offset) return RS_INVALID;
    memcpy(data, font_pack + offset, length);
    return RS_OK;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4U; ++i) bytes[i] = (uint8_t)(value >> (i * 8U));
}
static void draw_line(u8g2_font_t *font, int16_t x, int16_t y,
                      int16_t length, uint8_t direction, uint16_t color)
{
    (void)font;
    for (int16_t i = 0; i < length; ++i) {
        int16_t px = x + (direction == 0U ? i : 0);
        int16_t py = y + (direction == 1U ? i : 0);
        assert(px >= 0 && px < 64 && py >= 0 && py < 64);
        image[py][px] = color != 0U;
    }
}
static void assert_glyph(u8g2_font_t *font, unsigned slot, uint16_t code)
{
    bool private_use = code == 0xE000U;
    unsigned width = private_use ? (slot == 0U ? 12U : 16U) : (slot == 0U ? 8U : 10U);
    unsigned height = private_use ? width : (slot == 0U ? 7U : 11U);
    int x_offset = private_use ? 0 : 1;
    int y_offset = private_use ? -2 : 0;
    int advance = private_use ? (int)width : (slot == 0U ? 10 : 12);
    int origin_x = 3 + x_offset;
    int origin_y = 30 - ((int)height + y_offset);

    assert(u8g2_IsGlyph(font, code));
    assert(u8g2_GetGlyphWidth(font, code) == advance);
    memset(image, 0, sizeof(image));
    assert(u8g2_DrawGlyph(font, 3, 30, code) == advance);
    assert(!font->font_read_error);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            int gx = x - origin_x, gy = y - origin_y;
            bool inside = gx >= 0 && gx < (int)width && gy >= 0 && gy < (int)height;
            bool expected = inside && (private_use ? gy >= (int)height / 2 : ((gx + gy) & 1) != 0);
            assert(image[y][x] == expected);
        }
    }
}
static void assert_loaded(u8g2_font_t *font)
{
    for (unsigned slot = 0U; slot < FONT_STORE_SLOT_COUNT; ++slot)
        assert(font_store_get_source((font_store_slot_t)slot) != NULL);
    for (unsigned slot = 0U; slot < 2U; ++slot) {
        u8g2_SetExternalFont(font, font_store_get_source((font_store_slot_t)slot));
        assert(u8g2_IsGlyph(font, 0x6708U)); /* Existing lunar-calendar glyph. */
        assert_glyph(font, slot, slot == 0U ? 0x00A1U : (uint16_t)'A');
        assert_glyph(font, slot, 0xE000U);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL);
    font_pack_length = (uint32_t)fread(font_pack, 1U, sizeof(font_pack), file);
    assert(font_pack_length > 56U && !ferror(file));
    assert(fclose(file) == 0);
    u8g2_font_t font = {0};
    font.draw_hv_line = draw_line;
    u8g2_SetForegroundColor(&font, 1U);
    u8g2_SetBackgroundColor(&font, 0U);

    assert(font_store_init() == NRF_SUCCESS);
    assert_loaded(&font);
    /* A reboot must rediscover the browser glyphs from persistent bytes. */
    font_store_host_reset();
    memset(&font, 0, sizeof(font));
    font.draw_hv_line = draw_line;
    u8g2_SetForegroundColor(&font, 1U);
    assert(font_store_init() == NRF_SUCCESS);
    assert_loaded(&font);

    /* A package revision must evict cached misses even when its size and
     * physical addresses stay unchanged. Hide and restore an ASCII record. */
    uint32_t offset = read_u32(font_pack + 8U);
    uint32_t length = read_u32(font_pack + 12U);
    uint8_t *record = font_pack + offset + 23U;
    uint8_t *ascii_end = font_pack + offset + length;
    while (record + 2U <= ascii_end && record[1] != 0U && record[0] != 0xA1U)
        record += record[1];
    assert(record + 2U <= ascii_end && record[0] == 0xA1U);
    record[0] = 0xA0U;
    write_u32(font_pack + 16U, resource_crc32(UINT32_MAX, font_pack + offset, length) ^ UINT32_MAX);
    ++font_revision;
    font_store_invalidate();
    assert(font_store_init() == NRF_SUCCESS);
    u8g2_SetExternalFont(&font, font_store_get_source(FONT_STORE_SLOT_WQY9));
    assert(!u8g2_IsGlyph(&font, 0xA1U));
    record[0] = 0xA1U;
    write_u32(font_pack + 16U, resource_crc32(UINT32_MAX, font_pack + offset, length) ^ UINT32_MAX);
    ++font_revision;
    font_store_invalidate();
    assert(font_store_init() == NRF_SUCCESS);
    assert_loaded(&font);

    /* Corrupt persisted glyph data without its per-font CRC: reboot rejects
     * the entire pack, then accepts the repaired package. */
    record[2] ^= 1U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    for (unsigned slot = 0U; slot < FONT_STORE_SLOT_COUNT; ++slot)
        assert(font_store_get_source((font_store_slot_t)slot) == NULL);
    record[2] ^= 1U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_loaded(&font);
    puts("PASS: browser glyph ABI, exact pixels, reboot, revision reload and CRC rejection");
    return 0;
}
