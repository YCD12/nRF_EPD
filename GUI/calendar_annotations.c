#include "calendar_annotations.h"

#include <string.h>

#include "gui_porting_hal.h"

#define CALENDAR_ANNOTATION_FLAG_PRESENT 0x01U
#define CALENDAR_ANNOTATION_FONT_MODE_MASK 0x06U
#define CALENDAR_ANNOTATION_FONT_MODE_SHIFT 1U
#define CALENDAR_ANNOTATION_FONT_MODE_SMALL 1U
#define CALENDAR_ANNOTATION_FONT_MODE_LARGE 2U
#define CALENDAR_ANNOTATION_COLOR_MODE_MASK 0x18U
#define CALENDAR_ANNOTATION_COLOR_MODE_SHIFT 3U
#define CALENDAR_ANNOTATION_COLOR_MODE_WHITE 1U
#define CALENDAR_ANNOTATION_COLOR_MODE_RED   2U

typedef struct {
    bool initialized;
    bool available;
    uint8_t version;
    uint16_t count;
    uint16_t glyph_count;
    uint32_t glyph_table_offset;
    uint32_t bitmap_data_offset;
    epd_ext_resource_entry_t entry;
} calendar_annotations_cache_t;

static calendar_annotations_cache_t m_cache;
#define CALENDAR_ANNOTATIONS_GLYPH_CACHE_WAYS 3U
typedef struct {
    bool valid;
    uint16_t codepoint;
    uint8_t pixel_size;
    calendar_annotation_glyph_t glyph;
} calendar_glyph_cache_entry_t;

static calendar_glyph_cache_entry_t m_glyph_cache[CALENDAR_ANNOTATIONS_GLYPH_CACHE_WAYS];
static uint8_t m_glyph_cache_next;

static uint16_t read_u16_le(const uint8_t* value) {
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* value) {
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) |
           ((uint32_t)value[3] << 24);
}

static bool calendar_annotations_init(void) {
    uint8_t header[CALENDAR_ANNOTATIONS_HEADER_SIZE];
    uint32_t required_length;

    if (m_cache.initialized) {
        return m_cache.available;
    }
    m_cache.initialized = true;

    if (res_hal_get_info(EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS,
                         EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY,
                         &m_cache.entry) != NRF_SUCCESS ||
        m_cache.entry.length < CALENDAR_ANNOTATIONS_HEADER_SIZE ||
        res_hal_read(EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS,
                     EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY,
                     0, header, sizeof(header)) != NRF_SUCCESS) {
        return false;
    }

    m_cache.version = header[4];
    m_cache.count = read_u16_le(&header[6]);
    required_length = CALENDAR_ANNOTATIONS_HEADER_SIZE +
                      (uint32_t)m_cache.count * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    if (read_u32_le(header) != CALENDAR_ANNOTATIONS_MAGIC ||
        m_cache.version != CALENDAR_ANNOTATIONS_VERSION ||
        header[5] != CALENDAR_ANNOTATIONS_RECORD_SIZE ||
        m_cache.count > CALENDAR_ANNOTATIONS_MAX_RECORDS ||
        required_length > m_cache.entry.length) {
        return false;
    }

    /* Bytes 8..15 were reserved in ANNO v1.  A zero glyph format is the
     * original payload; format 1 appends a backward-compatible glyph table. */
    if (header[8] == CALENDAR_ANNOTATIONS_GLYPH_FORMAT) {
        m_cache.glyph_count = read_u16_le(&header[10]);
        m_cache.glyph_table_offset = required_length;
        m_cache.bitmap_data_offset = read_u32_le(&header[12]);
        required_length = m_cache.glyph_table_offset +
                          (uint32_t)m_cache.glyph_count * CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE;
        if (header[9] != CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE ||
            m_cache.glyph_count > CALENDAR_ANNOTATIONS_MAX_GLYPHS ||
            m_cache.bitmap_data_offset < required_length ||
            m_cache.bitmap_data_offset > m_cache.entry.length) {
            return false;
        }
    }

    m_cache.available = true;
    return true;
}

void calendar_annotations_reset(void) {
    memset(&m_cache, 0, sizeof(m_cache));
    memset(m_glyph_cache, 0, sizeof(m_glyph_cache));
    m_glyph_cache_next = 0U;
}

bool calendar_annotations_get(uint16_t year, uint8_t month, uint8_t day,
                              calendar_annotation_t* annotation) {
    uint16_t low;
    uint16_t high;
    uint8_t record[CALENDAR_ANNOTATIONS_RECORD_SIZE];

    if (annotation != NULL) {
        memset(annotation, 0, sizeof(*annotation));
    }
    if (annotation == NULL || month < 1U || month > 12U || day < 1U || day > 31U ||
        !calendar_annotations_init()) {
        return false;
    }

    low = 0;
    high = m_cache.count;
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2U);
        uint16_t record_year;
        uint8_t record_month;
        uint8_t record_day;

        if (res_hal_read(EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS,
                         EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY,
                         CALENDAR_ANNOTATIONS_HEADER_SIZE +
                         (uint32_t)middle * CALENDAR_ANNOTATIONS_RECORD_SIZE,
                         record, sizeof(record)) != NRF_SUCCESS) {
            return false;
        }

        record_year = read_u16_le(record);
        record_month = record[2];
        record_day = record[3];
        if (year == record_year && month == record_month && day == record_day) {
            uint8_t label_length = record[5];
            uint8_t font_mode = (uint8_t)((record[4] & CALENDAR_ANNOTATION_FONT_MODE_MASK) >>
                                          CALENDAR_ANNOTATION_FONT_MODE_SHIFT);
            uint8_t color_mode = (uint8_t)((record[4] & CALENDAR_ANNOTATION_COLOR_MODE_MASK) >>
                                           CALENDAR_ANNOTATION_COLOR_MODE_SHIFT);
            if ((record[4] & CALENDAR_ANNOTATION_FLAG_PRESENT) == 0U ||
                label_length > CALENDAR_ANNOTATIONS_LABEL_MAX) {
                return false;
            }
            if (font_mode == CALENDAR_ANNOTATION_FONT_MODE_SMALL) {
                annotation->font_size = CALENDAR_ANNOTATION_FONT_SMALL;
            } else if (font_mode == CALENDAR_ANNOTATION_FONT_MODE_LARGE) {
                annotation->font_size = CALENDAR_ANNOTATION_FONT_LARGE;
            } else {
                annotation->font_size = CALENDAR_ANNOTATION_FONT_FOLLOW;
            }
            if (color_mode == CALENDAR_ANNOTATION_COLOR_MODE_WHITE) {
                annotation->color = CALENDAR_ANNOTATION_COLOR_WHITE;
            } else if (color_mode == CALENDAR_ANNOTATION_COLOR_MODE_RED) {
                annotation->color = CALENDAR_ANNOTATION_COLOR_RED;
            } else {
                annotation->color = CALENDAR_ANNOTATION_COLOR_BLACK;
            }
            memcpy(annotation->label, &record[6], label_length);
            annotation->label[label_length] = '\0';
            annotation->present = true;
            return true;
        }

        if (record_year < year ||
            (record_year == year && (record_month < month ||
             (record_month == month && record_day < day)))) {
            low = (uint16_t)(middle + 1U);
        } else {
            high = middle;
        }
    }
    return false;
}

bool calendar_annotations_get_glyph(uint16_t codepoint, uint8_t pixel_size,
                                    calendar_annotation_glyph_t* glyph) {
    uint16_t low;
    uint16_t high;
    uint8_t record[CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE];

    if (glyph == NULL || pixel_size == 0U || !calendar_annotations_init() ||
        m_cache.glyph_count == 0U) {
        return false;
    }
    for (uint8_t i = 0U; i < CALENDAR_ANNOTATIONS_GLYPH_CACHE_WAYS; i++) {
        if (m_glyph_cache[i].valid && m_glyph_cache[i].codepoint == codepoint &&
            m_glyph_cache[i].pixel_size == pixel_size) {
            *glyph = m_glyph_cache[i].glyph;
            return true;
        }
    }

    low = 0U;
    high = m_cache.glyph_count;
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2U);
        uint16_t record_codepoint;
        uint8_t record_pixel_size;

        if (res_hal_read(EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS,
                         EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY,
                         m_cache.glyph_table_offset +
                         (uint32_t)middle * CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE,
                         record, sizeof(record)) != NRF_SUCCESS) {
            return false;
        }
        record_codepoint = read_u16_le(record);
        record_pixel_size = record[2];
        if (record_codepoint == codepoint && record_pixel_size == pixel_size) {
            uint8_t width = record[3];
            uint8_t height = record[4];
            uint8_t bitmap_length = record[8];
            uint16_t bitmap_offset = read_u16_le(&record[10]);
            uint16_t expected_length = (uint16_t)(((width + 7U) / 8U) * height);

            if (width == 0U || width > CALENDAR_ANNOTATIONS_GLYPH_MAX_DIM ||
                height == 0U || height > CALENDAR_ANNOTATIONS_GLYPH_MAX_DIM ||
                record[5] == 0U || bitmap_length == 0U ||
                bitmap_length > CALENDAR_ANNOTATIONS_GLYPH_BITMAP_MAX ||
                bitmap_length != expected_length ||
                bitmap_offset < m_cache.bitmap_data_offset ||
                bitmap_offset > m_cache.entry.length ||
                bitmap_length > m_cache.entry.length - bitmap_offset) {
                return false;
            }

            calendar_glyph_cache_entry_t* cache = &m_glyph_cache[m_glyph_cache_next];
            memset(cache, 0, sizeof(*cache));
            cache->glyph.width = width;
            cache->glyph.height = height;
            cache->glyph.advance = record[5];
            cache->glyph.x_offset = (int8_t)record[6];
            cache->glyph.y_offset = (int8_t)record[7];
            cache->glyph.bitmap_length = bitmap_length;
            if (res_hal_read(EPD_EXT_RES_TYPE_CALENDAR_ANNOTATIONS,
                             EPD_EXT_CALENDAR_ANNOTATIONS_ENTRY,
                             bitmap_offset, cache->glyph.bitmap,
                             bitmap_length) != NRF_SUCCESS) {
                memset(cache, 0, sizeof(*cache));
                return false;
            }
            cache->codepoint = codepoint;
            cache->pixel_size = pixel_size;
            cache->valid = true;
            *glyph = cache->glyph;
            m_glyph_cache_next = (uint8_t)((m_glyph_cache_next + 1U) %
                                           CALENDAR_ANNOTATIONS_GLYPH_CACHE_WAYS);
            return true;
        }

        if (record_codepoint < codepoint ||
            (record_codepoint == codepoint && record_pixel_size < pixel_size)) {
            low = (uint16_t)(middle + 1U);
        } else {
            high = middle;
        }
    }
    return false;
}
