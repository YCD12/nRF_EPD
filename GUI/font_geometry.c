#include "font_geometry.h"

/* Single definitions keep each wire-compatible geometry table in Flash once. */
const u8g2_font_placeholder_t font_slot_geometry[3] = {
    {12, 12, 0, -2, 8, -2, 10, -1, 13},
    {16, 16, 0, -2, 11, -3, 12, -4, 17},
    {13, 22, 0, -3, 19, -5, 19, -5, 13},
};

const uint8_t todo_font_sizes[TODO_FONT_SIZE_COUNT] = {9, 12, 16, 20, 24, 32};
const u8g2_font_placeholder_t todo_font_geometry[TODO_FONT_SIZE_COUNT] = {
    {15, 16, -3, -4, 12, -4, 12, -4, 13},
    {20, 22, -4, -6, 16, -6, 16, -6, 17},
    {28, 30, -6, -8, 22, -8, 22, -8, 23},
    {34, 36, -7, -9, 27, -9, 27, -9, 28},
    {40, 43, -8, -11, 32, -11, 32, -11, 33},
    {54, 58, -11, -15, 43, -15, 43, -15, 44},
};
