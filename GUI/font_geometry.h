#ifndef FONT_GEOMETRY_H
#define FONT_GEOMETRY_H

#include "u8g2_font.h"

typedef char font_geometry_wire_layout[(offsetof(u8g2_font_placeholder_t, advance) == 8U) ? 1 : -1];

/* Slot order is also the wire order. Keep geometry shared by GUI and store. */
static const u8g2_font_placeholder_t font_slot_geometry[3] = {
    {12, 12, 0, -2, 8, -2, 10, -1, 13},
    {16, 16, 0, -2, 11, -3, 12, -4, 17},
    {13, 22, 0, -3, 19, -5, 19, -5, 13},
};

#endif
