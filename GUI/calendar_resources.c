#include "calendar_resources.h"
#include "resource_layout.h"
#include "resource_store.h"
#include <string.h>

#define CAL_MAGIC 0x314C4143UL /* CAL1 */
#define CAL_VERSION 1U
#define CAL_HEADER_SIZE 16U
#define CAL_ENTRY_SIZE 16U
#define CAL_SECTIONS 16U

typedef struct { uint32_t offset; uint16_t item_size, count; } section_t;
static section_t m_sections[CAL_SECTIONS];
static uint16_t m_year_min, m_year_max, m_holiday_year;
static bool m_attempted, m_ready;

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

bool calendar_resources_init(void)
{
    static const uint16_t sizes[CAL_SECTIONS] = {7,4,7,4,4,4,4,7,4,4,1,1,12,12,2,10};
    /* Zero denotes a variable-length section, validated against its metadata. */
    static const uint16_t counts[CAL_SECTIONS] = {13,2,31,7,12,10,12,24,0,0,0,24,0,0,0,5};
    uint8_t header[CAL_HEADER_SIZE];
    uint32_t length;
    if (m_attempted) return m_ready;
    m_attempted = true;
    if (resource_store_mount() != RS_OK ||
        resource_store_open(RS_PART_CALENDAR_RESOURCES, &length) != RS_OK ||
        length < CAL_HEADER_SIZE + CAL_SECTIONS * CAL_ENTRY_SIZE ||
        resource_store_read(RS_PART_CALENDAR_RESOURCES, 0, header, sizeof(header)) != RS_OK ||
        resource_get_u32(header) != CAL_MAGIC || u16(header + 4) != CAL_VERSION ||
        u16(header + 6) != CAL_SECTIONS) return false;
    m_year_min = u16(header + 8);
    m_year_max = u16(header + 10);
    m_holiday_year = u16(header + 12);
    /* CAL1 encodes solar years in 12 bits and includes the preceding lunar year. */
    if (m_year_min < 1584U || m_year_max < m_year_min || m_year_max > 4095U ||
        m_holiday_year < 1583U || m_holiday_year > 4095U ||
        header[14] || header[15]) return false;
    uint32_t end = CAL_HEADER_SIZE + CAL_SECTIONS * CAL_ENTRY_SIZE;
    for (unsigned i = 0; i < CAL_SECTIONS; ++i) {
        uint8_t raw[CAL_ENTRY_SIZE];
        if (resource_store_read(RS_PART_CALENDAR_RESOURCES,
                                CAL_HEADER_SIZE + i * CAL_ENTRY_SIZE,
                                raw, sizeof(raw)) != RS_OK ||
            u16(raw) != i + 1U || u16(raw + 6) != 0U) return false;
        section_t *s = &m_sections[i];
        s->item_size = u16(raw + 2);
        s->count = u16(raw + 4);
        s->offset = resource_get_u32(raw + 8);
        uint32_t bytes = resource_get_u32(raw + 12);
        if (s->item_size != sizes[i] || (counts[i] && s->count != counts[i]) ||
            bytes != (uint32_t)s->item_size * s->count ||
            s->offset < end || s->offset > length || bytes > length - s->offset) return false;
        end = s->offset + bytes;
    }
    uint32_t years = (uint32_t)m_year_max - m_year_min + 1U;
    if (m_sections[CAL_RES_LUNAR_MONTH_DAYS - 1U].count != years + 2U ||
        m_sections[CAL_RES_SOLAR_NEW_YEAR - 1U].count != years + 2U ||
        m_sections[CAL_RES_JIEQI_YEAR_BITS - 1U].count != (years + 1U) * 3U) return false;
    /* Both lunar tables start with the same base year, followed by its data. */
    for (unsigned i = CAL_RES_LUNAR_MONTH_DAYS; i <= CAL_RES_SOLAR_NEW_YEAR; ++i) {
        uint8_t raw[4];
        if (resource_store_read(RS_PART_CALENDAR_RESOURCES, m_sections[i - 1U].offset,
                                raw, sizeof(raw)) != RS_OK ||
            resource_get_u32(raw) != (uint32_t)m_year_min - 1U) return false;
    }
    m_ready = true;
    return true;
}

bool calendar_resources_ready(void) { return calendar_resources_init(); }
uint16_t calendar_resources_year_min(void) { return calendar_resources_init() ? m_year_min : 0U; }
uint16_t calendar_resources_year_max(void) { return calendar_resources_init() ? m_year_max : 0U; }
uint16_t calendar_resources_holiday_year(void) { return calendar_resources_init() ? m_holiday_year : 0U; }

bool calendar_resource_read(calendar_resource_section_t section, uint16_t index,
                            void *data, size_t length)
{
    if (!calendar_resources_init() || section < 1 || section >= CAL_RES_SECTION_COUNT || !data)
        return false;
    section_t *s = &m_sections[section - 1U];
    if (index >= s->count || length != s->item_size) return false;
    return resource_store_read(RS_PART_CALENDAR_RESOURCES,
                               s->offset + (uint32_t)index * s->item_size,
                               data, length) == RS_OK;
}

bool calendar_resource_text(calendar_resource_section_t section, uint16_t index,
                            char *text, size_t capacity)
{
    if (!calendar_resources_init() || section < 1 || section >= CAL_RES_SECTION_COUNT ||
        !text || !capacity) return false;
    section_t *s = &m_sections[section - 1U];
    if (index >= s->count || s->item_size > capacity ||
        resource_store_read(RS_PART_CALENDAR_RESOURCES,
                            s->offset + (uint32_t)index * s->item_size,
                            text, s->item_size) != RS_OK) return false;
    text[capacity - 1U] = '\0';
    return memchr(text, '\0', s->item_size) != NULL;
}

bool calendar_resource_u32(calendar_resource_section_t section, uint16_t index,
                           uint32_t *value)
{
    uint8_t raw[4];
    if (!value || !calendar_resource_read(section, index, raw, sizeof(raw))) return false;
    *value = resource_get_u32(raw);
    return true;
}

bool calendar_resource_u8(calendar_resource_section_t section, uint16_t index,
                          uint8_t *value)
{
    return value && calendar_resource_read(section, index, value, 1U);
}

bool calendar_resource_festival(calendar_resource_section_t section,
                                uint8_t month, uint8_t day,
                                char *name, size_t capacity)
{
    uint8_t raw[12];
    if (!calendar_resources_init() || !name || capacity < 10U ||
        (section != CAL_RES_SOLAR_FESTIVALS && section != CAL_RES_LUNAR_FESTIVALS))
        return false;
    section_t *s = &m_sections[section - 1U];
    if (s->item_size != sizeof(raw)) return false;
    for (uint16_t i = 0; i < s->count; ++i) {
        if (!calendar_resource_read(section, i, raw, sizeof(raw))) return false;
        if (raw[0] == month && raw[1] == day && memchr(raw + 2, '\0', 10U)) {
            memcpy(name, raw + 2, 10U);
            name[capacity - 1U] = '\0';
            return true;
        }
    }
    return false;
}

bool calendar_resource_holiday(uint16_t year, uint8_t month, uint8_t day, bool *work)
{
    uint8_t raw[2];
    if (!calendar_resources_init() || !work || year != m_holiday_year) return false;
    section_t *s = &m_sections[CAL_RES_HOLIDAYS - 1U];
    if (s->item_size != sizeof(raw)) return false;
    for (uint16_t i = 0; i < s->count; ++i) {
        if (!calendar_resource_read(CAL_RES_HOLIDAYS, i, raw, sizeof(raw))) return false;
        uint16_t value = u16(raw);
        if (((value >> 8) & 0xFU) == month && (value & 0xFFU) == day) {
            *work = (value >> 12) != 0U;
            return true;
        }
    }
    return false;
}

void calendar_resources_invalidate(void)
{
    memset(m_sections, 0, sizeof(m_sections));
    m_year_min = m_year_max = m_holiday_year = 0U;
    m_attempted = m_ready = false;
}

#if defined(CALENDAR_RESOURCES_HOST_TEST)
void calendar_resources_host_reset(void) { calendar_resources_invalidate(); }
#endif
