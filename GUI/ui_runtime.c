#include "ui_runtime.h"

#include <string.h>

#include "../EPD/epd_ext_resources.h"
#include "../EPD/res_hal.h"

void ui_render_invalidate_month_cache(void);

#define UI_BUNDLE_MAGIC EPD_EXT_UI_BUNDLE_MAGIC
#define UI_BUNDLE_VERSION EPD_EXT_UI_BUNDLE_VERSION
#define UI_BUNDLE_SCHEMA_MAJOR EPD_EXT_UI_SCHEMA_MAJOR
#define UI_BUNDLE_SCHEMA_MINOR EPD_EXT_UI_SCHEMA_MINOR
#define UI_BUNDLE_HEADER_SIZE 28U
#define UI_BUNDLE_MODE_SIZE 16U
#define UI_BUNDLE_COMPONENT_SIZE 24U
#define UI_CACHE_MAX_MODES 8U
#define UI_RENDER_COMPONENT_CACHE_MAX 18U

#define UI_UPDATE_MANUAL GUI_UPDATE_MANUAL

typedef struct {
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint8_t mode_count;
    uint8_t component_count;
    uint16_t mode_offset;
    uint16_t component_offset;
    uint16_t string_offset;
    uint16_t string_length;
    uint8_t schema_major;
    uint8_t schema_minor;
    uint8_t required_color_depth;
} ui_bundle_header_t;

extern void app_feed_wdt(void);

static bool m_ui_cache_valid;
static uint16_t m_ui_cache_width;
static uint16_t m_ui_cache_height;
static ret_code_t m_ui_bundle_last_error;
static ui_bundle_header_t m_ui_cached_header;
static ui_mode_record_t m_ui_cached_modes[UI_CACHE_MAX_MODES];

static bool m_ui_render_cache_valid;
static uint8_t m_ui_render_cache_mode_id;
static uint16_t m_ui_render_cache_width;
static uint16_t m_ui_render_cache_height;
static ui_mode_record_t m_ui_render_cached_mode;
static ui_component_record_t m_ui_render_cached_components[UI_RENDER_COMPONENT_CACHE_MAX];
static uint8_t m_ui_render_cached_count;

static uint16_t ui_read_u16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t ui_read_i16(const uint8_t* data) {
    return (int16_t)ui_read_u16(data);
}

static uint32_t ui_read_u32(const uint8_t* data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static ret_code_t ui_bundle_read(uint32_t offset, void* data, size_t length) {
    return res_hal_read(EPD_EXT_RES_TYPE_UI, EPD_EXT_UI_BUNDLE, offset, data, length);
}

static bool ui_range_fits(uint32_t offset, uint32_t count, uint32_t item_size, uint32_t limit) {
    if (offset > limit) return false;
    if (item_size != 0 && count > ((limit - offset) / item_size)) return false;
    return true;
}

static bool ui_rect_fits_screen(int32_t x, int32_t y, int32_t w, int32_t h,
                                uint16_t screen_w, uint16_t screen_h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if ((uint32_t)x >= screen_w || (uint32_t)y >= screen_h) return false;
    if ((uint32_t)w > (uint32_t)screen_w - (uint32_t)x) return false;
    if ((uint32_t)h > (uint32_t)screen_h - (uint32_t)y) return false;
    return true;
}

static bool ui_partial_rect_valid(const ui_bundle_header_t* header, const ui_mode_record_t* mode) {
    if (mode->partial_w == 0 && mode->partial_h == 0) return true;
    if (mode->partial_w < 8 || mode->partial_h == 0) return false;
    return ui_rect_fits_screen(mode->partial_x, mode->partial_y, mode->partial_w, mode->partial_h,
                               header->width, header->height);
}

static bool ui_component_is_known(uint8_t type) {
    switch (type) {
        case UI_COMP_WEEK_HEADER:
        case UI_COMP_MONTH_GRID:
        case UI_COMP_TIME_7SEG:
        case UI_COMP_BATTERY:
        case UI_COMP_LINE:
        case UI_COMP_LOW_BATTERY_TIP:
        case UI_COMP_TIME_SYNC_TIP:
        case UI_COMP_TEXT_FIELD:
            return true;
        default:
            return false;
    }
}

static bool ui_component_valid(const ui_bundle_header_t* header, const ui_component_record_t* comp) {
    if (header == NULL || comp == NULL) return false;
    if (!ui_component_is_known(comp->type)) return false;
    if (header->string_length == 0 || comp->text_offset >= header->string_length) return false;

    switch (comp->type) {
        case UI_COMP_WEEK_HEADER:
        case UI_COMP_MONTH_GRID:
        case UI_COMP_LINE:
        case UI_COMP_LOW_BATTERY_TIP:
        case UI_COMP_TIME_SYNC_TIP:
            return ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height);

        case UI_COMP_TEXT_FIELD:
            if (!ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height)) {
                return false;
            }
            if (comp->data_id < UI_TEXT_DATE_YMD || comp->data_id > UI_TEXT_SSID) return false;
            return comp->p0 <= 0 || comp->p0 < (int16_t)header->height;

        case UI_COMP_TIME_7SEG:
            if (!ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height)) {
                return false;
            }
            return comp->p0 >= UI_TIME_SCALE_MIN && comp->p0 <= UI_TIME_SCALE_MAX &&
                   comp->p1 >= UI_TIME_DIGITS_MIN && comp->p1 <= UI_TIME_DIGITS_MAX;

        case UI_COMP_BATTERY:
            if (comp->w < 4 || comp->w > 80 || comp->h <= 0 || comp->h > 40) return false;
            if (comp->x < comp->w || comp->x + 2 > (int16_t)header->width) return false;
            return comp->y >= 0 && comp->y + 12 <= (int16_t)header->height;

        default:
            return false;
    }
}

static ret_code_t ui_read_header(ui_bundle_header_t* header, uint16_t screen_w, uint16_t screen_h,
                                 uint8_t epd_color_depth) {
    uint8_t raw[UI_BUNDLE_HEADER_SIZE];
    uint32_t magic;
    epd_ext_resource_entry_t entry;
    uint32_t mode_end;
    uint32_t component_end;
    ret_code_t ret;

    if (header == NULL) return NRF_ERROR_NULL;

    ret = res_hal_get_info(EPD_EXT_RES_TYPE_UI, EPD_EXT_UI_BUNDLE, &entry);
    if (ret != NRF_SUCCESS) return ret;
    if (entry.length < UI_BUNDLE_HEADER_SIZE) return NRF_ERROR_INVALID_LENGTH;

    ret = ui_bundle_read(0, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    magic = ui_read_u32(&raw[0]);
    if (magic != UI_BUNDLE_MAGIC) return NRF_ERROR_INVALID_DATA;

    header->version = ui_read_u16(&raw[4]);
    header->width = ui_read_u16(&raw[6]);
    header->height = ui_read_u16(&raw[8]);
    header->mode_count = raw[10];
    header->component_count = raw[11];
    header->mode_offset = ui_read_u16(&raw[12]);
    header->component_offset = ui_read_u16(&raw[14]);
    header->string_offset = ui_read_u16(&raw[16]);
    header->string_length = ui_read_u16(&raw[18]);
    header->schema_major = raw[20];
    header->schema_minor = raw[21];
    header->required_color_depth = raw[22];

    /* Legacy bundles (pre-P3) left bytes 20-22 as zero: treat as schema v1, skip color check. */
    if (header->schema_major == 0U) {
        header->schema_major = UI_BUNDLE_SCHEMA_MAJOR;
        header->schema_minor = 0U;
    }

    if (header->version != UI_BUNDLE_VERSION) {
        return NRF_ERROR_INVALID_DATA;
    }

    /* Schema major handshake: template newer than firmware parser -> reject (honest fallback). */
    if (header->schema_major > UI_BUNDLE_SCHEMA_MAJOR) {
        return NRF_ERROR_NOT_SUPPORTED;
    }

    /* Screen spec: target resolution must match active driver (optimization 8). */
    if (header->width != screen_w || header->height != screen_h) {
        return NRF_ERROR_INVALID_DATA;
    }

    /* Color depth: only enforced when bundle declares a non-zero requirement. */
    if (header->required_color_depth != 0U &&
        epd_color_depth != 0U &&
        header->required_color_depth != epd_color_depth) {
        return NRF_ERROR_INVALID_DATA;
    }

    if (header->mode_count == 0 || header->component_count == 0 ||
        header->string_length == 0 ||
        header->mode_offset < UI_BUNDLE_HEADER_SIZE ||
        header->component_offset < header->mode_offset ||
        header->string_offset < header->component_offset) {
        return NRF_ERROR_INVALID_DATA;
    }

    if (!ui_range_fits(header->mode_offset, header->mode_count, UI_BUNDLE_MODE_SIZE, entry.length) ||
        !ui_range_fits(header->component_offset, header->component_count, UI_BUNDLE_COMPONENT_SIZE, entry.length) ||
        !ui_range_fits(header->string_offset, header->string_length, 1U, entry.length)) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    mode_end = header->mode_offset + (uint32_t)header->mode_count * UI_BUNDLE_MODE_SIZE;
    component_end = header->component_offset + (uint32_t)header->component_count * UI_BUNDLE_COMPONENT_SIZE;
    if (mode_end > header->component_offset || component_end > header->string_offset) {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static ret_code_t ui_read_mode_at(const ui_bundle_header_t* header, uint8_t index, ui_mode_record_t* mode) {
    uint8_t raw[UI_BUNDLE_MODE_SIZE];
    ret_code_t ret;

    if (header == NULL || mode == NULL) return NRF_ERROR_NULL;
    if (index >= header->mode_count) return NRF_ERROR_NOT_FOUND;

    ret = ui_bundle_read(header->mode_offset + (uint32_t)index * UI_BUNDLE_MODE_SIZE, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    mode->mode_id = raw[0];
    mode->update_policy = raw[1];
    mode->component_start = raw[2];
    mode->component_count = raw[3];
    mode->partial_x = ui_read_u16(&raw[4]);
    mode->partial_y = ui_read_u16(&raw[6]);
    mode->partial_w = ui_read_u16(&raw[8]);
    mode->partial_h = ui_read_u16(&raw[10]);
    mode->flags = ui_read_u16(&raw[12]);

    if ((uint16_t)mode->component_start + mode->component_count > header->component_count) {
        return NRF_ERROR_INVALID_DATA;
    }
    if (!ui_partial_rect_valid(header, mode)) {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static ret_code_t ui_find_mode(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                               ui_bundle_header_t* header, ui_mode_record_t* mode) {
    ret_code_t ret;

    if (!m_ui_cache_valid || m_ui_cache_width != screen_w || m_ui_cache_height != screen_h) {
        ret = GUI_ReloadUICache(screen_w, screen_h, 0U);
        if (ret != NRF_SUCCESS) return ret;
    }

    if (header != NULL) {
        *header = m_ui_cached_header;
    }

    for (uint8_t i = 0; i < m_ui_cached_header.mode_count; i++) {
        if (m_ui_cached_modes[i].mode_id == mode_id) {
            if (mode != NULL) {
                *mode = m_ui_cached_modes[i];
            }
            return NRF_SUCCESS;
        }
    }

    return NRF_ERROR_NOT_FOUND;
}

ret_code_t GUI_ReloadUICache(uint16_t width, uint16_t height, uint8_t epd_color_depth) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;
    ret_code_t ret = ui_read_header(&header, width, height, epd_color_depth);

    m_ui_cache_valid = false;
    m_ui_render_cache_valid = false;
    ui_render_invalidate_month_cache();
    m_ui_bundle_last_error = ret;
    if (ret != NRF_SUCCESS) return ret;
    if (header.mode_count > UI_CACHE_MAX_MODES) {
        return NRF_ERROR_NO_MEM;
    }

    for (uint8_t i = 0; i < header.mode_count; i++) {
        ret = ui_read_mode_at(&header, i, &mode);
        if (ret != NRF_SUCCESS) return ret;
        m_ui_cached_modes[i] = mode;
        app_feed_wdt();
    }

    m_ui_cached_header = header;
    m_ui_cache_width = width;
    m_ui_cache_height = height;
    m_ui_cache_valid = true;
    m_ui_bundle_last_error = NRF_SUCCESS;
    return NRF_SUCCESS;
}

ret_code_t GUI_GetUIBundleLastError(void) {
    return m_ui_bundle_last_error;
}

static ret_code_t ui_read_component_raw(const ui_bundle_header_t* header, uint8_t index,
                                        ui_component_record_t* comp) {
    uint8_t raw[UI_BUNDLE_COMPONENT_SIZE];
    ret_code_t ret;

    if (header == NULL || comp == NULL) return NRF_ERROR_NULL;
    if (index >= header->component_count) return NRF_ERROR_NOT_FOUND;

    ret = ui_bundle_read(header->component_offset + (uint32_t)index * UI_BUNDLE_COMPONENT_SIZE, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    comp->type = raw[0];
    comp->data_id = raw[1];
    comp->font_id = raw[2];
    comp->flags = raw[3];
    comp->x = ui_read_i16(&raw[4]);
    comp->y = ui_read_i16(&raw[6]);
    comp->w = ui_read_i16(&raw[8]);
    comp->h = ui_read_i16(&raw[10]);
    comp->fg = ui_read_u16(&raw[12]);
    comp->bg = ui_read_u16(&raw[14]);
    comp->p0 = ui_read_i16(&raw[16]);
    comp->p1 = ui_read_i16(&raw[18]);
    comp->p2 = ui_read_i16(&raw[20]);
    comp->text_offset = ui_read_u16(&raw[22]);
    return NRF_SUCCESS;
}

static ret_code_t ui_load_mode_components(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                          ui_mode_record_t* mode,
                                          ui_component_record_t* components,
                                          uint8_t max_components,
                                          uint8_t* component_count) {
    ui_bundle_header_t header;
    ret_code_t ret;

    if (mode == NULL || components == NULL || component_count == NULL) {
        return NRF_ERROR_NULL;
    }

    *component_count = 0;
    ret = ui_find_mode(mode_id, screen_w, screen_h, &header, mode);
    if (ret != NRF_SUCCESS) return ret;

    bool skip_unknown = header.schema_minor > UI_BUNDLE_SCHEMA_MINOR;
    uint8_t loaded = 0;

    for (uint8_t i = 0; i < mode->component_count; i++) {
        ui_component_record_t comp;
        ret = ui_read_component_raw(&header, mode->component_start + i, &comp);
        if (ret != NRF_SUCCESS) return ret;

        if (!ui_component_is_known(comp.type)) {
            if (skip_unknown) {
                continue;
            }
            return NRF_ERROR_INVALID_DATA;
        }
        if (!ui_component_valid(&header, &comp)) {
            return NRF_ERROR_INVALID_DATA;
        }
        if (loaded >= max_components) {
            return NRF_ERROR_NO_MEM;
        }
        components[loaded++] = comp;
        app_feed_wdt();
    }

    if (loaded == 0U) {
        return NRF_ERROR_INVALID_DATA;
    }

    *component_count = loaded;
    return NRF_SUCCESS;
}

ret_code_t ui_runtime_load_render_cache(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                                 ui_mode_record_t* mode,
                                                 const ui_component_record_t** components,
                                                 uint8_t* component_count) {
    ret_code_t ret;

    if (mode == NULL || components == NULL || component_count == NULL) {
        return NRF_ERROR_NULL;
    }

    if (m_ui_render_cache_valid &&
        m_ui_render_cache_mode_id == mode_id &&
        m_ui_render_cache_width == screen_w &&
        m_ui_render_cache_height == screen_h) {
        *mode = m_ui_render_cached_mode;
        *components = m_ui_render_cached_components;
        *component_count = m_ui_render_cached_count;
        return NRF_SUCCESS;
    }

    ret = ui_load_mode_components(mode_id, screen_w, screen_h,
                                  &m_ui_render_cached_mode,
                                  m_ui_render_cached_components,
                                  UI_RENDER_COMPONENT_CACHE_MAX,
                                  &m_ui_render_cached_count);
    if (ret != NRF_SUCCESS) {
        m_ui_render_cache_valid = false;
        m_ui_render_cached_count = 0;
        return ret;
    }

    m_ui_render_cache_mode_id = mode_id;
    m_ui_render_cache_width = screen_w;
    m_ui_render_cache_height = screen_h;
    m_ui_render_cache_valid = true;

    *mode = m_ui_render_cached_mode;
    *components = m_ui_render_cached_components;
    *component_count = m_ui_render_cached_count;
    return NRF_SUCCESS;
}

bool ui_runtime_get_partial_rect(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                 uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;

    if (ui_find_mode(mode_id, screen_w, screen_h, &header, &mode) != NRF_SUCCESS) {
        return false;
    }
    if (mode.partial_w == 0 || mode.partial_h == 0) {
        return false;
    }

    if (x) *x = mode.partial_x;
    if (y) *y = mode.partial_y;
    if (w) *w = mode.partial_w;
    if (h) *h = mode.partial_h;
    return true;
}

bool GUI_ModeExists(uint8_t mode_id, uint16_t width, uint16_t height) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;
    return ui_find_mode(mode_id, width, height, &header, &mode) == NRF_SUCCESS;
}

uint8_t GUI_GetModeUpdatePolicy(uint8_t mode_id, uint16_t width, uint16_t height) {
    if (!m_ui_cache_valid || m_ui_cache_width != width || m_ui_cache_height != height) {
        return UI_UPDATE_MANUAL;
    }

    for (uint8_t i = 0; i < m_ui_cached_header.mode_count; i++) {
        if (m_ui_cached_modes[i].mode_id == mode_id) {
            return m_ui_cached_modes[i].update_policy;
        }
    }

    return UI_UPDATE_MANUAL;
}
