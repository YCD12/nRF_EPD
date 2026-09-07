#ifndef CALENDAR_GRID_H
#define CALENDAR_GRID_H

#include <stdbool.h>
#include <stdint.h>

#define GUI_LARGE_SCREEN_H 300

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t bottom;
} calendar_grid_rect_t;

typedef struct {
    uint8_t first_day_week;
    uint8_t week_start;
    uint8_t month_max_days;
    int16_t screen_height;
} calendar_grid_input_t;

typedef struct {
    uint8_t first_day_week;
    int8_t adjusted_first_day;
    uint8_t month_max_days;
    uint8_t month_day_rows;
    int16_t cell_w;
    int16_t cell_h;
    bool large;
} calendar_grid_metrics_t;

typedef struct {
    int16_t bx;
    int16_t by;
    int16_t cr;
    int16_t display_week;
    int16_t actual_week;
    bool weekend;
} calendar_cell_pos_t;

typedef struct {
    uint8_t col_w;
    uint8_t remainder;
    uint8_t row_h;
} calendar_week_header_metrics_t;

static inline calendar_week_header_metrics_t calendar_week_header_metrics(int16_t span_width,
                                                                         bool large)
{
    calendar_week_header_metrics_t m;

    m.col_w = (uint8_t)(span_width / 7);
    m.remainder = (uint8_t)(span_width % 7);
    m.row_h = large ? 32 : 24;
    return m;
}

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

#endif
