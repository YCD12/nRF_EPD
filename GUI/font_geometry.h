#ifndef FONT_GEOMETRY_H
#define FONT_GEOMETRY_H

#include "u8g2_font.h"

typedef char font_geometry_wire_layout[(offsetof(u8g2_font_placeholder_t, advance) == 8U) ? 1 : -1];

/* Slot order is also the wire order. Keep geometry shared by GUI and store. */
extern const u8g2_font_placeholder_t font_slot_geometry[3];

/* Browser-generated TODO typography has room for italic overhang and full
 * ascenders/descenders. Keep these bytes identical to font_builder.js. */
#define TODO_FONT_SIZE_COUNT 6U
extern const uint8_t todo_font_sizes[TODO_FONT_SIZE_COUNT];
extern const u8g2_font_placeholder_t todo_font_geometry[TODO_FONT_SIZE_COUNT];

static inline const u8g2_font_placeholder_t *todo_font_metrics_for_size(uint8_t size)
{
    for (unsigned i = 0U; i < TODO_FONT_SIZE_COUNT; ++i) {
        if (todo_font_sizes[i] == size) return &todo_font_geometry[i];
    }
    return NULL;
}

#endif
