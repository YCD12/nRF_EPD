#ifndef UI_RESOURCES_H
#define UI_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>

#include "clock_layout.h"

typedef enum {
    UI_COMP_CAL_HEADER = 1,
    UI_COMP_CAL_WEEK,
    UI_COMP_CAL_GRID,
    UI_COMP_SPLIT_FRAME_TOP,
    UI_COMP_SPLIT_FRAME_BOTTOM,
    UI_COMP_SPLIT_SEPARATOR,
    UI_COMP_SPLIT_RIGHT_HEADER,
    UI_COMP_SPLIT_WEEK,
    UI_COMP_SPLIT_GRID,
    UI_COMP_SPLIT_LEFT_HEADER,
    UI_COMP_SPLIT_LEFT_TOP_LINE,
    UI_COMP_SPLIT_TIME,
    UI_COMP_SPLIT_LEFT_BOTTOM_LINE,
    UI_COMP_SPLIT_BOTTOM_INFO,
    UI_COMP_CLOCK_HEADER,
    UI_COMP_CLOCK_TOP_LINE,
    UI_COMP_CLOCK_TIME,
    UI_COMP_CLOCK_BOTTOM_LINE,
    UI_COMP_CLOCK_BOTTOM_INFO,
    UI_COMPONENT_COUNT
} ui_component_id_t;

typedef enum {
    UI_KIND_GROUP = 1,
    UI_KIND_LINE,
    UI_KIND_GRID,
    UI_KIND_CLOCK
} ui_component_kind_t;

typedef struct {
    /* Runtime view: fixed IDs/kinds and wire-only flags are validated on load,
     * then discarded. The external UIR1 record remains 20 bytes. */
    uint8_t font_primary;
    uint8_t font_secondary;
    uint8_t font_tertiary;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    int16_t value0;
    int16_t value1;
} ui_component_t;

bool ui_resources_init(void);
void ui_resources_invalidate(void);
bool ui_resources_ready(void);
uint32_t ui_resources_revision(void);
const ui_component_t *ui_resource_component(ui_component_id_t id);
bool ui_resources_clock_geom(bool fullscreen, uint16_t screen_w, uint16_t screen_h,
                             clock_refresh_geom_t *geometry);

#if defined(UI_RESOURCES_HOST_TEST)
void ui_resources_host_reset(void);
#endif

#endif
