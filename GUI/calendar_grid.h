#ifndef CALENDAR_GRID_H
#define CALENDAR_GRID_H

#include <stdbool.h>
#include <stdint.h>

#define GUI_LARGE_SCREEN_H 300 /* 高于此用大格/大字 */
/* 本产品 800x480，不链接小屏用的 helvB14。 */
#define GUI_HELVB14_ENABLED 0

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t bottom; /* 日期区底边（不含星期栏） */
} calendar_grid_rect_t;

typedef struct {
    uint8_t first_day_week; /* 本月 1 号星期（0=日） */
    uint8_t week_start;     /* 栏首星期 */
    uint8_t month_max_days;
    int16_t screen_height;
} calendar_grid_input_t;

typedef struct {
    uint8_t first_day_week;
    int8_t adjusted_first_day; /* 相对 week_start 的列偏移 */
    uint8_t month_max_days;
    uint8_t month_day_rows;
    int16_t cell_w;
    int16_t cell_h;
    bool large;
} calendar_grid_metrics_t;

typedef struct {
    int16_t bx;           /* 日期圆左上 */
    int16_t by;
    int16_t cr;           /* 圆半径相关半宽 */
    int16_t display_week; /* 显示列 0–6 */
    int16_t actual_week;  /* 真实星期 0=日 */
    bool weekend;
} calendar_cell_pos_t;

typedef struct {
    int16_t text_x;
    int16_t text_y;
    int16_t circle_x;
    int16_t circle_y;
    uint8_t circle_r;
} calendar_holiday_mark_t;

typedef struct {
    uint8_t col_w;
    uint8_t remainder; /* 最后一列多出的像素 */
    int16_t row_h;
} calendar_week_header_metrics_t;

/* 星期栏：7 列均分，余数加在最后一列。 */
static inline calendar_week_header_metrics_t calendar_week_header_metrics(int16_t span_width,
                                                                         bool large)
{
    calendar_week_header_metrics_t m;

    m.col_w = (uint8_t)(span_width / 7);
    m.remainder = (uint8_t)(span_width % 7);
    m.row_h = large ? 32 : 24;
    return m;
}

/* 按矩形和 week_start 算行列与格子尺寸。 */
static inline calendar_grid_metrics_t calendar_grid_compute(const calendar_grid_rect_t *rect,
                                                            const calendar_grid_input_t *in)
{
    calendar_grid_metrics_t m;

    m.first_day_week = in->first_day_week;
    m.adjusted_first_day = (int8_t)((in->first_day_week - in->week_start + 7) % 7);
    m.month_max_days = in->month_max_days;
    m.month_day_rows =
        (uint8_t)(1 + (in->month_max_days - (7 - m.adjusted_first_day) + 6) / 7);
    m.cell_w = (int16_t)(rect->width / 7);
    m.cell_h = (int16_t)((rect->bottom - rect->y) / m.month_day_rows);
    m.large = in->screen_height > GUI_LARGE_SCREEN_H;
    return m;
}

/* day_index 从 0 起，对应本月 1 日。 */
static inline calendar_cell_pos_t calendar_grid_cell(const calendar_grid_rect_t *rect,
                                                     const calendar_grid_metrics_t *m,
                                                     uint8_t day_index)
{
    calendar_cell_pos_t cell;

    cell.actual_week = (int16_t)((m->first_day_week + day_index) % 7);
    cell.display_week = (int16_t)((m->adjusted_first_day + day_index) % 7);
    cell.weekend = (cell.actual_week == 0) || (cell.actual_week == 6);
    cell.cr = m->large ? 13 : 10;
    cell.bx = (int16_t)(rect->x + (m->cell_w - 2 * cell.cr) / 2 + cell.display_week * m->cell_w);
    cell.by = (int16_t)(rect->y + (m->cell_h - 2 * cell.cr) / 2 +
                        ((day_index + m->adjusted_first_day) / 7) * m->cell_h + 3);
    return cell;
}

static inline void calendar_grid_cell_origin(const calendar_grid_rect_t *rect,
                                             const calendar_grid_metrics_t *m,
                                             uint8_t day_index,
                                             int16_t *x,
                                             int16_t *y)
{
    int16_t col = (int16_t)((m->adjusted_first_day + day_index) % 7);
    int16_t row = (int16_t)((day_index + m->adjusted_first_day) / 7);

    if (x != NULL) {
        *x = (int16_t)(rect->x + col * m->cell_w);
    }
    if (y != NULL) {
        *y = (int16_t)(rect->y + row * m->cell_h);
    }
}

/* 短标签右对齐画在格子右上角。x/y 是基线坐标。 */
static inline void calendar_annotation_corner_cursor(const calendar_grid_rect_t *rect,
                                                     const calendar_grid_metrics_t *m,
                                                     uint8_t day_index,
                                                     int16_t text_w,
                                                     int16_t font_ascent,
                                                     int16_t *x,
                                                     int16_t *y)
{
    int16_t cell_x;
    int16_t cell_y;
    int16_t pad = m->large ? 4 : 3;
    int16_t cursor_x;

    calendar_grid_cell_origin(rect, m, day_index, &cell_x, &cell_y);
    cursor_x = (int16_t)(cell_x + m->cell_w - pad - text_w);

    if (cursor_x < (int16_t)(cell_x + pad)) {
        cursor_x = (int16_t)(cell_x + pad);
    }
    if (x != NULL) {
        *x = cursor_x;
    }
    if (y != NULL) {
        *y = (int16_t)(cell_y + pad + font_ascent);
    }
}

/* 默认画在日期圆右侧；右上角有短标签时改到格子左下，避免叠字。 */
static inline calendar_holiday_mark_t calendar_holiday_mark(const calendar_grid_rect_t *rect,
                                                            const calendar_grid_metrics_t *m,
                                                            const calendar_cell_pos_t *cell,
                                                            uint8_t day_index,
                                                            bool avoid_corner)
{
    calendar_holiday_mark_t mark;

    mark.circle_r = m->large ? 10 : 8;
    if (avoid_corner) {
        int16_t cell_x;
        int16_t cell_y;
        int16_t pad = m->large ? 6 : 4;

        calendar_grid_cell_origin(rect, m, day_index, &cell_x, &cell_y);

        mark.text_x = (int16_t)(cell_x + pad);
        mark.text_y = (int16_t)(cell_y + m->cell_h - pad);
        mark.circle_x = (int16_t)(cell_x + pad + mark.circle_r);
        mark.circle_y = (int16_t)(mark.text_y - 2);
    } else {
        mark.text_x = (int16_t)(cell->bx + (m->large ? 31 : 22));
        mark.text_y = (int16_t)(cell->by + 3);
        mark.circle_x = (int16_t)(cell->bx + (m->large ? 36 : 27));
        mark.circle_y = (int16_t)(cell->by - 2);
    }
    return mark;
}

/* 休/班保留日期右侧位置，标注红点改到左上角，避免字和当日白圆遮住红点。 */
static inline void calendar_annotation_dot_center(const calendar_grid_rect_t *rect,
                                                  const calendar_grid_metrics_t *m,
                                                  uint8_t day_index,
                                                  bool has_holiday,
                                                  int16_t *x,
                                                  int16_t *y,
                                                  uint8_t *r)
{
    int16_t cell_x;
    int16_t cell_y;

    calendar_grid_cell_origin(rect, m, day_index, &cell_x, &cell_y);
    if (x != NULL) {
        int16_t pad = m->large ? 8 : 6;
        *x = (int16_t)(cell_x + (has_holiday ? pad : m->cell_w - pad));
    }
    if (y != NULL) {
        *y = (int16_t)(cell_y + (m->large ? 7 : 5));
    }
    if (r != NULL) {
        *r = m->large ? 3 : 2;
    }
}

#endif
