#ifndef UI_RUNTIME_H
#define UI_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "GUI.h"
#include "sdk_errors.h"

#define UI_MODE_LOW_BATTERY 250U

#define UI_TIME_SCALE_MIN 1
#define UI_TIME_SCALE_MAX 10
#define UI_TIME_DIGITS_MIN 1
#define UI_TIME_DIGITS_MAX 10

#define UI_FLAG_PARTIAL_ONLY 0x01U
#define UI_TEXT_FLAG_RIGHT_ALIGN 0x02U
#define UI_TEXT_FLAG_ZERO_PAD 0x04U
#define UI_TEXT_FLAG_WITH_ZODIAC 0x08U
#define UI_TEXT_FLAG_WEEK_LEADING_SPACE 0x10U

typedef enum {
    UI_COMP_WEEK_HEADER = 2,
    UI_COMP_MONTH_GRID = 3,
    UI_COMP_TIME_7SEG = 4,
    UI_COMP_BATTERY = 5,
    UI_COMP_LINE = 6,
    UI_COMP_LOW_BATTERY_TIP = 10,
    UI_COMP_TIME_SYNC_TIP = 11,
    UI_COMP_TEXT_FIELD = 12,
} ui_component_type_t;

typedef enum {
    UI_TEXT_DATE_YMD = 1,
    UI_TEXT_LUNAR_DATE = 2,
    UI_TEXT_TEMPERATURE = 3,
    UI_TEXT_LUNAR_YEAR = 4,
    UI_TEXT_WEEK_NUMBER = 5,
    UI_TEXT_JIEQI = 6,
    UI_TEXT_WEEKDAY = 7,
    UI_TEXT_TEMP_SSID = 8,
    UI_TEXT_SSID = 9,
} ui_text_data_id_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t data_id;
    uint8_t font_id;
    uint8_t flags;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t fg;
    uint16_t bg;
    int16_t p0;
    int16_t p1;
    int16_t p2;
    uint16_t text_offset;
} ui_component_record_t;

typedef struct __attribute__((packed)) {
    uint8_t mode_id;
    uint8_t update_policy;
    uint8_t component_start;
    uint8_t component_count;
    uint16_t partial_x;
    uint16_t partial_y;
    uint16_t partial_w;
    uint16_t partial_h;
    uint16_t flags;
    uint8_t reserved[2];
} ui_mode_record_t;

static inline uint16_t ui_runtime_clamp_u16(int16_t value, uint16_t min_value, uint16_t max_value) {
    if (value < (int16_t)min_value) return min_value;
    if (value > (int16_t)max_value) return max_value;
    return (uint16_t)value;
}

ret_code_t ui_runtime_load_render_cache(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                        ui_mode_record_t* mode,
                                        const ui_component_record_t** components,
                                        uint8_t* component_count);

bool ui_runtime_get_partial_rect(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                 uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h);

#endif
