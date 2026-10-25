#define UI_SCHEMA_VALIDATION
#include "ui_resources.h"
#include "resource_layout.h"
#include "resource_store.h"
#include <string.h>
static ui_component_t m_components[UI_MAX_COMPONENTS];
static uint32_t m_revision;
static uint8_t m_count;
static bool m_attempted, m_ready;
static uint16_t u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
bool ui_resource_params(const ui_component_t *c, int16_t *v) {
    return c && v &&
           resource_store_read(RS_PART_UI_RESOURCES, c->params_offset, v, c->params_count * 2U) ==
               RS_OK;
}
bool ui_resource_text(const ui_component_t *c, uint8_t index, char *text, uint16_t size) {
    uint8_t ch;
    uint16_t at = 0;
    if (!c || !text || !size || index >= c->text_count)
        return false;
    while (index && at < c->text_length) {
        if (resource_store_read(RS_PART_UI_RESOURCES, c->text_offset + at++, &ch, 1) != RS_OK)
            return false;
        if (!ch)
            --index;
    }
    uint16_t n = c->text_length - at;
    if (n > size)
        n = size;
    if (!n || resource_store_read(RS_PART_UI_RESOURCES, c->text_offset + at, text, n) != RS_OK)
        return false;
    return memchr(text, 0, n) != NULL;
}
static bool valid_text(const ui_component_t *c) {
    uint16_t at = 0;
    for (uint8_t i = 0; i < c->text_count; i++) {
        uint8_t text[UI_TEXT_BYTES];
        uint16_t size = c->text_length - at;
        if (size > sizeof(text))
            size = sizeof(text);
        if (!size ||
            resource_store_read(RS_PART_UI_RESOURCES, c->text_offset + at, text, size) != RS_OK)
            return false;
        uint16_t pos = 0;
        while (pos < size && text[pos]) {
            uint8_t op = text[pos++];
            if (op < 4U) {
                if (c->kind != UI_KIND_TEXT || pos >= size)
                    return false;
                uint8_t n = text[pos++];
                if ((op == 1U && (!n || n > UI_TOKEN_COUNT)) ||
                    (op == 2U && (n > 2U || (&c->font_primary)[n] == 255U)) || (op == 3U && n > 2U))
                    return false;
            } else if (op < 32U || op == 127U)
                return false;
            else if (op >= 128U) {
                uint8_t extra = op < 0xe0U ? 1U : 2U;
                if (op < 0xc2U || op >= 0xf0U || pos + extra > size)
                    return false;
                if (extra == 2U &&
                    ((op == 0xe0U && text[pos] < 0xa0U) || (op == 0xedU && text[pos] >= 0xa0U)))
                    return false;
                for (uint8_t j = 0; j < extra; j++)
                    if ((text[pos++] & 0xc0U) != 0x80U)
                        return false;
            }
        }
        if (pos >= size)
            return false;
        at += pos + 1U;
    }
    return at == c->text_length;
}
void ui_resource_clock_layout(const ui_component_t *c, const int16_t *s,
                              ui_clock_layout_t *layout) {
    int scale = c->value0;
    layout->step = s[UI_CLOCK_DIGIT_STEP_SCALE] * scale + s[UI_CLOCK_DIGIT_STEP_BIAS];
    layout->block = 2 * layout->step - s[UI_CLOCK_BLOCK_TRIM] * scale;
    int height = s[UI_CLOCK_HEIGHT_SCALE] * scale + s[UI_CLOCK_HEIGHT_BIAS];
    layout->x = (c->w - 2 * layout->block - s[UI_CLOCK_SPAN_GAP] * scale) / 2 + s[UI_CLOCK_OFFSET_X];
    layout->y = (c->h - height) / 2 + s[UI_CLOCK_OFFSET_Y];
    layout->minute = layout->block + (s[UI_CLOCK_HOUR_GAP] + s[UI_CLOCK_MINUTE_GAP]) * scale;
}
static bool clock_geometry(const ui_component_t *c, const int16_t *s, clock_refresh_geom_t *g) {
    ui_clock_layout_t layout;
    ui_resource_clock_layout(c, s, &layout);
    int scale = c->value0, step = layout.step, block = layout.block;
    int x = layout.x, y = layout.y, minute = layout.minute;
    int minx = c->w, miny = c->h, maxx = 0, maxy = 0;
    if (x < 0 || y < 0 || block < 1)
        return false;
    for (uint8_t n = 0; n < 4; n++)
        for (uint8_t seg = 0; seg < 7; seg++) {
            const int16_t *a = s + UI_CLOCK_SEGMENT_0_X_SCALE + 5U * seg;
            int sx = x + (n < 2 ? n * step : minute + (n - 2) * step) + a[0] * scale + a[1];
            int sy = y + a[2] * scale + a[3];
            int w = a[4] ? (s[UI_CLOCK_HORIZONTAL_LENGTH] + 2) * scale : 2 * scale;
            int h = a[4] ? 2 * scale : (s[UI_CLOCK_VERTICAL_LENGTH] + 2) * scale;
            if (sx < minx)
                minx = sx;
            if (sy < miny)
                miny = sy;
            if (sx + w > maxx)
                maxx = sx + w;
            if (sy + h > maxy)
                maxy = sy + h;
        }
    for (uint8_t i = 0; i < 2; i++) {
        int sx = x + block + s[UI_CLOCK_HOUR_GAP] * scale;
        int sy =
            y + s[UI_CLOCK_COLON_TOP + 2U * i] * scale / 2 + s[UI_CLOCK_COLON_TOP_BIAS + 2U * i];
        int w = s[UI_CLOCK_COLON_WIDTH] * scale, h = s[UI_CLOCK_COLON_HEIGHT] * scale;
        if (sx < minx)
            minx = sx;
        if (sy < miny)
            miny = sy;
        if (sx + w > maxx)
            maxx = sx + w;
        if (sy + h > maxy)
            maxy = sy + h;
    }
    if (minx < 0 || miny < 0 || maxx > c->w || maxy > c->h)
        return false;
    if (g) {
        memset(g, 0, sizeof(*g));
        g->origin_x = c->x;
        g->origin_y = c->y;
        g->canvas_w = c->w;
        g->canvas_h = c->h;
        g->cS = scale;
        g->nD = 2;
        g->span_gap = s[UI_CLOCK_SPAN_GAP];
        g->resource_geometry = true;
        g->face_x = x;
        g->face_y = y;
        g->face_w = maxx - x;
        g->face_h = maxy - y;
        g->digit_step = step;
        g->digit_w = maxx - (x + minute + step);
        g->minute_x = minute;
    }
    return true;
}
bool ui_resources_init(void) {
    uint8_t raw[16];
    rs_package_info_t info;
    if (m_attempted)
        return m_ready;
    m_attempted = true;
    if (resource_store_mount() != RS_OK ||
        resource_store_open_info(RS_PART_UI_RESOURCES, &info) != RS_OK ||
        info.length < 16 || info.length > 65504 ||
        resource_store_read(RS_PART_UI_RESOURCES, 0, raw, 16) != RS_OK ||
        resource_get_u32(raw) != 0x32524955UL || u16(raw + 4) != 2 || !u16(raw + 6) ||
        u16(raw + 6) > UI_MAX_COMPONENTS || u16(raw + 8) != 800 || u16(raw + 10) != 480 ||
        u16(raw + 12) != 32 || u16(raw + 14))
        return false;
    m_count = (uint8_t)u16(raw + 6);
    uint32_t tail = 16U + 32U * m_count;
    for (uint8_t i = 0; i < m_count; i++) {
        ui_component_t *c = &m_components[i];
        if (resource_store_read(RS_PART_UI_RESOURCES, 16U + 32U * i, c, 32) != RS_OK || !c->id ||
            !c->modes || c->modes > 31 || c->kind < 1 || c->kind > 6 || c->flags > 15 ||
            (c->flags & 12U) == 12U || c->align > 2 || (c->kind != UI_KIND_TEXT && c->align) ||
            (c->kind != UI_KIND_CLOCK && (c->value0 || c->value1)) || c->reserved ||
            c->foreground > 2 || c->background > 2 || c->accent > 2 || c->x < 0 || c->y < 0 ||
            c->w < 1 || c->h < 1 || (int32_t)c->x + c->w > 800 || (int32_t)c->y + c->h > 480 ||
            c->params_offset != tail || c->params_count != ui_style_counts[c->kind] ||
            c->text_offset != tail + 2U * c->params_count ||
            (uint32_t)c->text_offset + c->text_length > info.length)
            return false;
        for (uint8_t j = 0; j < i; j++)
            if (m_components[j].id == c->id)
                return false;
        for (uint8_t j = 0; j < 3; j++)
            if ((&c->font_primary)[j] != 255U && (&c->font_primary)[j] > 3U)
                return false;
        uint8_t expected = c->kind == UI_KIND_TEXT   ? 1U
                           : c->kind == UI_KIND_GRID ? 2U
                           : c->kind == UI_KIND_TODO ? 3U
                                                     : 0U;
        if (c->text_count != expected || (expected && c->font_primary == 255U) ||
            (c->kind == UI_KIND_WEEK && c->font_primary == 255U) ||
            (c->kind == UI_KIND_GRID && (c->font_secondary == 255U || c->font_tertiary == 255U)))
            return false;
        int16_t params[UI_MAX_PARAMS];
        uint16_t base = 0;
        for (uint8_t j = 1; j < c->kind; j++)
            base += ui_style_counts[j];
        if (!ui_resource_params(c, params))
            return false;
        for (uint8_t j = 0; j < c->params_count; j++)
            if (params[j] < ui_style_bounds[ui_style_bound_index[base + j]][0] ||
                params[j] > ui_style_bounds[ui_style_bound_index[base + j]][1])
                return false;
        if (!valid_text(c))
            return false;
        if (c->kind == UI_KIND_CLOCK &&
            (c->value0 < 2 || c->value0 > 10 || (c->value0 & 1) || c->value1 != 2 || (c->x & 7) ||
             (c->w & 7) || c->h > 208 || !clock_geometry(c, params, NULL)))
            return false;
        tail = (uint32_t)c->text_offset + c->text_length;
    }
    if (tail != info.length)
        return false;
    m_revision = info.revision;
    m_ready = true;
    return true;
}
bool ui_resources_ready(void) { return ui_resources_init(); }
bool ui_resources_retry_init(void) {
    m_attempted = m_ready;
    return ui_resources_init();
}
uint32_t ui_resources_revision(void) { return ui_resources_init() ? m_revision : 0; }
uint8_t ui_resources_count(void) { return ui_resources_init() ? m_count : 0; }
const ui_component_t *ui_resource_at(uint8_t i) {
    return ui_resources_init() && i < m_count ? &m_components[i] : NULL;
}
const ui_component_t *ui_resource_component(ui_component_id_t id) {
    if (ui_resources_init())
        for (uint8_t i = 0; i < m_count; i++)
            if (m_components[i].id == id)
                return &m_components[i];
    return NULL;
}
uint8_t ui_resources_todo_capacity(uint8_t height, bool extended) {
    uint8_t maximum = 8;
    bool found = false;
    for (uint8_t i = 0; i < ui_resources_count(); i++) {
        const ui_component_t *c = &m_components[i];
        int16_t s[UI_MAX_PARAMS];
        if (c->kind != UI_KIND_TODO || (c->flags & UI_FLAG_HIDDEN))
            continue;
        if (!ui_resource_params(c, s))
            return 0;
        uint16_t n = c->h / (height + s[UI_TODO_ROW_PADDING]);
        (void)extended;
        if (n < maximum)
            maximum = (uint8_t)n;
        found = true;
    }
    return found ? maximum : 8;
}
bool ui_resources_clock_geom(bool full, uint16_t w, uint16_t h, clock_refresh_geom_t *g) {
    const ui_component_t *c = ui_resource_component(full ? UI_COMP_CLOCK_TIME : UI_COMP_SPLIT_TIME);
    int16_t params[UI_MAX_PARAMS];
    if (!g || w != 800 || h != 480 || !c || c->kind != UI_KIND_CLOCK ||
        !ui_resource_params(c, params) || !clock_geometry(c, params, g))
        return false;
    uint8_t mode = full ? 4U : 2U;
    /* Conditional visibility may differ between OLD and NEW. The timestamp
     * alone cannot reconstruct a face that the full scene did not draw. */
    g->partial_allowed = c->foreground == 0 && c->background == 1 && c->flags == 0 &&
                         (c->modes & mode);
    for (uint8_t i = 0; i < m_count; i++) {
        const ui_component_t *other = &m_components[i];
        if (other != c && (other->modes & mode) && !(other->flags & UI_FLAG_HIDDEN) &&
            c->x < other->x + other->w && c->x + c->w > other->x && c->y < other->y + other->h &&
            c->y + c->h > other->y) {
            if (other->flags & UI_FLAG_SYNC_ONLY)
                g->partial_sync_overlay = true;
            else
                g->partial_allowed = false;
        }
    }
    return true;
}
void ui_resources_invalidate(void) {
    m_revision = 0;
    m_count = 0;
    m_attempted = m_ready = false;
}
#if defined(UI_RESOURCES_HOST_TEST)
void ui_resources_host_reset(void) { ui_resources_invalidate(); }
#endif
