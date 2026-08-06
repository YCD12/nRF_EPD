#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "calendar_annotations.h"
#include "gui_porting_hal.h"

static uint8_t m_resource[160];
static uint32_t m_resource_length;
static int m_resource_available;

static void write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void set_record(uint16_t index, uint16_t year, uint8_t month, uint8_t day,
                       const char* label) {
    uint8_t* record = m_resource + CALENDAR_ANNOTATIONS_HEADER_SIZE +
                      index * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    size_t length = strlen(label);
    assert(length <= CALENDAR_ANNOTATIONS_LABEL_MAX);
    write_u16_le(record, year);
    record[2] = month;
    record[3] = day;
    record[4] = 1U;
    record[5] = (uint8_t)length;
    memcpy(record + 6, label, length);
}

static void make_valid_resource(void) {
    memset(m_resource, 0, sizeof(m_resource));
    write_u32_le(m_resource, CALENDAR_ANNOTATIONS_MAGIC);
    m_resource[4] = CALENDAR_ANNOTATIONS_VERSION;
    m_resource[5] = CALENDAR_ANNOTATIONS_RECORD_SIZE;
    write_u16_le(m_resource + 6, 3U);
    set_record(0, 2026U, 8U, 2U, "A");
    set_record(1, 2026U, 8U, 5U, "go");
    set_record(2, 2027U, 1U, 1U, "");
    m_resource[CALENDAR_ANNOTATIONS_HEADER_SIZE +
               CALENDAR_ANNOTATIONS_RECORD_SIZE + 4U] = 1U | (2U << 1) | (2U << 3);
    m_resource_length = CALENDAR_ANNOTATIONS_HEADER_SIZE +
                        3U * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    m_resource_available = 1;
}

static void make_resource_with_glyph(void) {
    uint32_t glyph_offset;
    uint32_t bitmap_offset;
    uint8_t* glyph;

    make_valid_resource();
    glyph_offset = m_resource_length;
    bitmap_offset = glyph_offset + CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE;
    m_resource[8] = CALENDAR_ANNOTATIONS_GLYPH_FORMAT;
    m_resource[9] = CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE;
    write_u16_le(m_resource + 10, 1U);
    write_u32_le(m_resource + 12, bitmap_offset);

    glyph = m_resource + glyph_offset;
    write_u16_le(glyph, 0x5B66U); /* 学 */
    glyph[2] = 12U;
    glyph[3] = 12U;
    glyph[4] = 12U;
    glyph[5] = 12U;
    glyph[6] = 0U;
    glyph[7] = (uint8_t)-11;
    glyph[8] = 24U;
    write_u16_le(glyph + 10, (uint16_t)bitmap_offset);
    memset(m_resource + bitmap_offset, 0xA5, 24U);
    m_resource_length = bitmap_offset + 24U;
}

ret_code_t res_hal_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry) {
    if (!m_resource_available || type != EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS ||
        id != EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY) {
        return NRF_ERROR_NOT_FOUND;
    }
    if (entry != NULL) {
        entry->type = type;
        entry->id = id;
        entry->offset = 0U;
        entry->length = m_resource_length;
        entry->crc32 = 0U;
    }
    return NRF_SUCCESS;
}

ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len) {
    if (!m_resource_available || type != EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS ||
        id != EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY) {
        return NRF_ERROR_NOT_FOUND;
    }
    if (buf == NULL || offset > m_resource_length || len > m_resource_length - offset) {
        return NRF_ERROR_INVALID_LENGTH;
    }
    memcpy(buf, m_resource + offset, len);
    return NRF_SUCCESS;
}

static void test_lookup_and_reset(void) {
    calendar_annotation_t annotation;

    make_valid_resource();
    calendar_annotations_reset();
    assert(calendar_annotations_get(2026U, 8U, 5U, &annotation));
    assert(annotation.present);
    assert(strcmp(annotation.label, "go") == 0);
    assert(annotation.font_size == CALENDAR_ANNOTATION_FONT_LARGE);
    assert(annotation.color == CALENDAR_ANNOTATION_COLOR_RED);
    assert(calendar_annotations_get(2026U, 8U, 2U, &annotation));
    assert(annotation.font_size == CALENDAR_ANNOTATION_FONT_FOLLOW);
    assert(annotation.color == CALENDAR_ANNOTATION_COLOR_BLACK);
    assert(calendar_annotations_get(2027U, 1U, 1U, &annotation));
    assert(annotation.label[0] == '\0');
    assert(!calendar_annotations_get(2026U, 8U, 3U, &annotation));

    m_resource_available = 0;
    calendar_annotations_reset();
    assert(!calendar_annotations_get(2026U, 8U, 5U, &annotation));

    make_valid_resource();
    set_record(1, 2026U, 8U, 5U, "ok");
    /* A failed lookup is cached until a resource commit/reset occurs. */
    assert(!calendar_annotations_get(2026U, 8U, 5U, &annotation));
    calendar_annotations_reset();
    assert(calendar_annotations_get(2026U, 8U, 5U, &annotation));
    assert(strcmp(annotation.label, "ok") == 0);
}

static void test_invalid_resource(void) {
    calendar_annotation_t annotation;

    make_valid_resource();
    m_resource[5] = 15U;
    calendar_annotations_reset();
    assert(!calendar_annotations_get(2026U, 8U, 2U, &annotation));

    make_valid_resource();
    m_resource[21] = CALENDAR_ANNOTATIONS_LABEL_MAX + 1U;
    calendar_annotations_reset();
    assert(!calendar_annotations_get(2026U, 8U, 2U, &annotation));

    m_resource_available = 0;
    calendar_annotations_reset();
    assert(!calendar_annotations_get(2026U, 8U, 2U, &annotation));
}

static void test_dynamic_glyph(void) {
    calendar_annotation_glyph_t glyph;

    make_resource_with_glyph();
    calendar_annotations_reset();
    assert(calendar_annotations_get_glyph(0x5B66U, 12U, &glyph));
    assert(glyph.width == 12U && glyph.height == 12U && glyph.advance == 12U);
    assert(glyph.y_offset == -11);
    assert(glyph.bitmap_length == 24U && glyph.bitmap[0] == 0xA5U);
    assert(!calendar_annotations_get_glyph(0x5B66U, 9U, &glyph));
    assert(!calendar_annotations_get_glyph(0x4F1AU, 12U, &glyph));

    /* Cached glyph data changes only after a resource commit/reset. */
    m_resource[m_resource_length - 24U] = 0x5AU;
    assert(calendar_annotations_get_glyph(0x5B66U, 12U, &glyph));
    assert(glyph.bitmap[0] == 0xA5U);
    calendar_annotations_reset();
    assert(calendar_annotations_get_glyph(0x5B66U, 12U, &glyph));
    assert(glyph.bitmap[0] == 0x5AU);

    make_resource_with_glyph();
    write_u32_le(m_resource + 12, m_resource_length + 1U);
    calendar_annotations_reset();
    assert(!calendar_annotations_get_glyph(0x5B66U, 12U, &glyph));
}

int main(void) {
    test_lookup_and_reset();
    test_invalid_resource();
    test_dynamic_glyph();
    puts("calendar annotations: OK");
    return 0;
}
