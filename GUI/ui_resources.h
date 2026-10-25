#ifndef UI_RESOURCES_H
#define UI_RESOURCES_H
#include "clock_layout.h"
#include "ui_schema.h"
#include <stdbool.h>
#include <stdint.h>
#define UI_MAX_COMPONENTS 64U
#define UI_MAX_PARAMS 64U
#define UI_TEXT_BYTES 256U
#define UI_FLAG_HIDDEN 1U
#define UI_FLAG_SYNC_ONLY 2U
#define UI_FLAG_JIEQI_AHEAD 4U
#define UI_FLAG_JIEQI_TODAY 8U
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
    UI_COMP_TODO_SPLIT,
    UI_COMP_TODO_FULL,
    UI_COMPONENT_COUNT
} ui_component_id_t;
typedef enum {
    UI_KIND_TEXT = 1,
    UI_KIND_LINE,
    UI_KIND_GRID,
    UI_KIND_CLOCK,
    UI_KIND_WEEK,
    UI_KIND_TODO
} ui_component_kind_t;
#define UI_KIND_GROUP UI_KIND_TEXT
typedef struct {
    uint8_t modes, id, kind, flags;
    uint8_t font_primary, font_secondary, font_tertiary, align;
    int16_t x, y, w, h, value0, value1;
    uint8_t foreground, background, accent, reserved;
    uint16_t params_offset;
    uint8_t params_count, text_count;
    uint16_t text_offset, text_length;
} ui_component_t;
typedef char ui_component_wire_size[(sizeof(ui_component_t) == 32U) ? 1 : -1];
/* Local coordinates shared by validation, full rendering and partial refresh. */
typedef struct {
    int x, y, step, block, minute;
} ui_clock_layout_t;
void ui_resource_clock_layout(const ui_component_t *c, const int16_t *style,
                              ui_clock_layout_t *layout);
bool ui_resources_init(void);
/* Retry failed initialization only; valid metadata remains cached. */
bool ui_resources_retry_init(void);
void ui_resources_invalidate(void);
bool ui_resources_ready(void);
uint32_t ui_resources_revision(void);
uint8_t ui_resources_count(void);
const ui_component_t *ui_resource_at(uint8_t index);
const ui_component_t *ui_resource_component(ui_component_id_t id);
bool ui_resource_params(const ui_component_t *c, int16_t *values);
bool ui_resource_text(const ui_component_t *c, uint8_t index, char *text, uint16_t size);
uint8_t ui_resources_todo_capacity(uint8_t font_height, bool extended);
bool ui_resources_clock_geom(bool fullscreen, uint16_t screen_w, uint16_t screen_h,
                             clock_refresh_geom_t *geometry);
#if defined(UI_RESOURCES_HOST_TEST)
void ui_resources_host_reset(void);
#endif
#endif
