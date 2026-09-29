#include "epd_resources.h"
#include "font_store.h"
#include "fonts.h"
#include "calendar_resources.h"
#include "ui_resources.h"

static bool m_ready;

void epd_resources_invalidate(void)
{
    m_ready = false;
#if GUI_USE_EXTERNAL_FONTS
    font_store_invalidate();
#endif
    calendar_resources_invalidate();
    ui_resources_invalidate();
}

rs_result_t epd_resources_reload(void)
{
    epd_resources_invalidate();
    rs_result_t result = resource_store_mount();
    if (result != RS_OK) return result;
#if GUI_USE_EXTERNAL_FONTS
    if (!GUI_ExternalFontsInit()) return RS_INVALID;
    for (unsigned slot = 0; slot < FONT_STORE_SLOT_COUNT; ++slot)
        if (font_store_get_source((font_store_slot_t)slot) == NULL) return RS_NOT_FOUND;
#elif !GUI_INCLUDE_INTERNAL_FONTS
    return RS_NOT_FOUND;
#endif
    if (!calendar_resources_init() || !ui_resources_init()) return RS_INVALID;
    m_ready = true;
    return RS_OK;
}

bool epd_resources_ready(void) { return m_ready; }
