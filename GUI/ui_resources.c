#include "ui_resources.h"

#include <string.h>

#include "resource_layout.h"
#include "resource_store.h"

#define UI_MAGIC 0x31524955UL /* UIR1 */
#define UI_VERSION 1U
#define UI_HEADER_SIZE 16U
#define UI_RECORD_SIZE 20U
#define UI_SCREEN_WIDTH 800U
#define UI_SCREEN_HEIGHT 480U
#define UI_FONT_NONE 0xFFU
#define UI_FONT_COUNT 3U

static ui_component_t m_components[UI_COMPONENT_COUNT - 1U];
static uint32_t m_revision;
static bool m_attempted;
static bool m_ready;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)get_u16(p);
}

static bool font_valid(uint8_t font)
{
    return font == UI_FONT_NONE || font < UI_FONT_COUNT;
}

static bool component_valid(const ui_component_t *c)
{
    static const uint8_t modes[UI_COMPONENT_COUNT - 1U] = {
        1,1,1, 2,2,2,2,2,2,2,2,2,2,2, 3,3,3,3,3
    };
    static const uint8_t kinds[UI_COMPONENT_COUNT - 1U] = {
        UI_KIND_GROUP,UI_KIND_GROUP,UI_KIND_GRID,
        UI_KIND_LINE,UI_KIND_LINE,UI_KIND_LINE,UI_KIND_GROUP,UI_KIND_GROUP,
        UI_KIND_GRID,UI_KIND_GROUP,UI_KIND_LINE,UI_KIND_CLOCK,UI_KIND_LINE,
        UI_KIND_GROUP,UI_KIND_GROUP,UI_KIND_LINE,UI_KIND_CLOCK,UI_KIND_LINE,
        UI_KIND_GROUP
    };
    if (c->id == 0U || c->id >= UI_COMPONENT_COUNT ||
        c->mode != modes[c->id - 1U] || c->kind != kinds[c->id - 1U] ||
        c->flags != 0U || c->reserved != 0U ||
        !font_valid(c->font_primary) || !font_valid(c->font_secondary) ||
        !font_valid(c->font_tertiary) || c->x < 0 || c->y < 0 ||
        c->w <= 0 || c->h <= 0 ||
        (uint32_t)c->x + (uint16_t)c->w > UI_SCREEN_WIDTH ||
        (uint32_t)c->y + (uint16_t)c->h > UI_SCREEN_HEIGHT) return false;
    if ((c->kind == UI_KIND_GROUP || c->kind == UI_KIND_GRID) &&
        c->font_primary == UI_FONT_NONE) return false;
    switch (c->id) {
        case UI_COMP_CAL_HEADER:
        case UI_COMP_CAL_GRID:
        case UI_COMP_SPLIT_GRID:
        case UI_COMP_CLOCK_HEADER:
            if (c->font_secondary == UI_FONT_NONE || c->font_tertiary == UI_FONT_NONE)
                return false;
            break;
        case UI_COMP_SPLIT_LEFT_HEADER:
            if (c->font_secondary == UI_FONT_NONE) return false;
            break;
        default:
            break;
    }
    switch (c->id) {
        case UI_COMP_CAL_HEADER:
            if (c->value0 < 0 || c->value0 >= UI_SCREEN_HEIGHT ||
                c->value1 < 2 || c->value1 >= UI_SCREEN_WIDTH) return false;
            break;
        case UI_COMP_SPLIT_RIGHT_HEADER:
        case UI_COMP_SPLIT_LEFT_HEADER:
            if (c->value0 < 0 || c->value0 >= UI_SCREEN_HEIGHT ||
                c->value1 < 0 || c->value1 > UI_SCREEN_WIDTH) return false;
            break;
        case UI_COMP_SPLIT_BOTTOM_INFO:
        case UI_COMP_CLOCK_BOTTOM_INFO:
            if (c->value0 < c->y || c->value0 >= UI_SCREEN_HEIGHT ||
                c->value1 < 0 || c->value1 > UI_SCREEN_WIDTH) return false;
            break;
        case UI_COMP_CLOCK_HEADER:
            if (c->value0 < c->y || c->value0 >= UI_SCREEN_HEIGHT ||
                c->value1 < c->x || c->value1 > c->x + c->w) return false;
            break;
        default:
            break;
    }
    if (c->kind == UI_KIND_CLOCK) {
        uint16_t scale = (uint16_t)c->value0;
        uint16_t digits = (uint16_t)c->value1;
        uint16_t block;
        uint16_t span;
        if (scale < 2U || scale > 10U || (scale & 1U) != 0U || digits != 2U ||
            ((uint16_t)c->x & 7U) != 0U || ((uint16_t)c->w & 7U) != 0U ||
            c->h > 208) return false;
        block = (uint16_t)(digits * (11U * scale + 2U) - 2U * scale);
        span = (uint16_t)(2U * block + (c->mode == 2U ? 4U : 6U) * scale);
        if (span > (uint16_t)c->w || 20U * scale + 4U > (uint16_t)c->h)
            return false;
    }
    return true;
}

bool ui_resources_init(void)
{
    uint8_t header[UI_HEADER_SIZE];
    rs_package_info_t info;
    uint32_t length;
    if (m_attempted) return m_ready;
    m_attempted = true;
    if (resource_store_mount() != RS_OK ||
        resource_store_info(RS_PART_UI_RESOURCES, &info) != RS_OK ||
        resource_store_open(RS_PART_UI_RESOURCES, &length) != RS_OK ||
        length != info.length ||
        length != UI_HEADER_SIZE + (UI_COMPONENT_COUNT - 1U) * UI_RECORD_SIZE ||
        resource_store_read(RS_PART_UI_RESOURCES, 0U, header, sizeof(header)) != RS_OK ||
        resource_get_u32(header) != UI_MAGIC || get_u16(header + 4U) != UI_VERSION ||
        get_u16(header + 6U) != UI_COMPONENT_COUNT - 1U ||
        get_u16(header + 8U) != UI_SCREEN_WIDTH ||
        get_u16(header + 10U) != UI_SCREEN_HEIGHT ||
        get_u16(header + 12U) != UI_RECORD_SIZE || get_u16(header + 14U) != 0U)
        return false;
    for (uint8_t i = 0U; i < UI_COMPONENT_COUNT - 1U; ++i) {
        uint8_t raw[UI_RECORD_SIZE];
        ui_component_t *c = &m_components[i];
        if (resource_store_read(RS_PART_UI_RESOURCES,
                                UI_HEADER_SIZE + (uint32_t)i * UI_RECORD_SIZE,
                                raw, sizeof(raw)) != RS_OK) return false;
        c->mode = raw[0]; c->id = raw[1]; c->kind = raw[2]; c->flags = raw[3];
        c->font_primary = raw[4]; c->font_secondary = raw[5];
        c->font_tertiary = raw[6]; c->reserved = raw[7];
        c->x = get_i16(raw + 8U); c->y = get_i16(raw + 10U);
        c->w = get_i16(raw + 12U); c->h = get_i16(raw + 14U);
        c->value0 = get_i16(raw + 16U); c->value1 = get_i16(raw + 18U);
        if (c->id != i + 1U || !component_valid(c)) return false;
    }
    m_revision = info.revision;
    m_ready = true;
    return true;
}

bool ui_resources_ready(void) { return ui_resources_init(); }
uint32_t ui_resources_revision(void) { return ui_resources_init() ? m_revision : 0U; }

const ui_component_t *ui_resource_component(ui_component_id_t id)
{
    if (!ui_resources_init() || id <= 0 || id >= UI_COMPONENT_COUNT) return NULL;
    return &m_components[id - 1U];
}

bool ui_resources_clock_geom(bool fullscreen, uint16_t screen_w, uint16_t screen_h,
                             clock_refresh_geom_t *geometry)
{
    const ui_component_t *c;
    if (!geometry || screen_w != UI_SCREEN_WIDTH || screen_h != UI_SCREEN_HEIGHT)
        return false;
    c = ui_resource_component(fullscreen ? UI_COMP_CLOCK_TIME : UI_COMP_SPLIT_TIME);
    if (!c) return false;
    geometry->origin_x = (uint16_t)c->x;
    geometry->origin_y = (uint16_t)c->y;
    geometry->canvas_w = (uint16_t)c->w;
    geometry->canvas_h = (uint16_t)c->h;
    geometry->cS = (uint16_t)c->value0;
    geometry->nD = (uint16_t)c->value1;
    geometry->span_gap = fullscreen ? 6U : 4U;
    return true;
}

#if defined(UI_RESOURCES_HOST_TEST)
void ui_resources_host_reset(void)
{
    memset(m_components, 0, sizeof(m_components));
    m_revision = 0U;
    m_attempted = m_ready = false;
}
#endif
