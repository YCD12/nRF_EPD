#include <assert.h>
#include <stdio.h>

#include "calendar_grid.h"

static void test_week_header_span_matches_full_and_split(void)
{
    calendar_week_header_metrics_t full = calendar_week_header_metrics(780, true);
    calendar_week_header_metrics_t split = calendar_week_header_metrics(398, true);
    calendar_week_header_metrics_t small = calendar_week_header_metrics(780, false);

    assert(full.col_w == 111);
    assert(full.remainder == 3);
    assert(full.row_h == 32);
    assert(split.col_w == 56);
    assert(split.remainder == 6);
    assert(split.row_h == 32);
    assert(small.row_h == 24);
}

static void test_full_month_grid_january_2026(void)
{
    /* Jan 1 2026 is Thursday (4). Sunday week start. */
    calendar_grid_rect_t rect = {10, 84, 780, 470};
    calendar_grid_input_t in = {4, 0, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    calendar_cell_pos_t day1 = calendar_grid_cell(&rect, &m, 0);
    calendar_cell_pos_t day4 = calendar_grid_cell(&rect, &m, 3); /* Sunday */

    assert(m.adjusted_first_day == 4);
    assert(m.month_day_rows == 5);
    assert(m.cell_w == 111);
    assert(m.cell_h == 77);
    assert(m.large);
    assert(day1.actual_week == 4);
    assert(day1.display_week == 4);
    assert(!day1.weekend);
    assert(day1.cr == 13);
    assert(day1.bx == 496);
    assert(day1.by == 112);
    assert(day4.weekend);
    assert(day4.actual_week == 0);
}

static void test_split_month_grid_uses_right_rect(void)
{
    calendar_grid_rect_t rect = {397, 136, 398, 420};
    calendar_grid_input_t in = {4, 0, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    calendar_cell_pos_t day1 = calendar_grid_cell(&rect, &m, 0);

    assert(m.cell_w == 56);
    assert(m.cell_h == 56);
    assert(m.month_day_rows == 5);
    assert(day1.bx == 397 + (56 - 26) / 2 + 4 * 56);
    assert(day1.by == 136 + (56 - 26) / 2 + 3);
}

static void test_product_panel_does_not_ship_helvB14(void)
{
    /* 800x480 is always large. helvB14 was only for height<=300. */
    assert(480 > GUI_LARGE_SCREEN_H);
    assert(GUI_HELVB14_ENABLED == 0);
}

static void test_annotation_text_sits_in_cell_top_right(void)
{
    calendar_grid_rect_t rect = {10, 84, 780, 470};
    calendar_grid_input_t in = {4, 0, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    int16_t x;
    int16_t y;

    calendar_annotation_corner_cursor(&rect, &m, 0, 24, 9, &x, &y);
    assert(x == 537); /* 10 + 4*111 + 111 - 4 - 24 */
    assert(y == 97);  /* 84 + 4 + 9 */
    assert(x + 24 <= 10 + 5 * 111);

    calendar_annotation_corner_cursor(&rect, &m, 3, 24, 9, &x, &y);
    assert(x == 93);  /* 10 + 0*111 + 111 - 4 - 24 */
    assert(y == 174); /* 84 + 77 + 4 + 9 */
}

static void test_monday_week_start_shifts_first_column(void)
{
    calendar_grid_rect_t rect = {10, 84, 780, 470};
    calendar_grid_input_t in = {4, 1, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    calendar_cell_pos_t day1 = calendar_grid_cell(&rect, &m, 0);

    assert(m.adjusted_first_day == 3);
    assert(day1.display_week == 3);
    assert(day1.actual_week == 4);
}

static void test_cell_origin_and_dot_share_same_origin(void)
{
    calendar_grid_rect_t rect = {10, 84, 780, 470};
    calendar_grid_input_t in = {4, 0, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    int16_t x;
    int16_t y;
    int16_t dot_x;
    int16_t dot_y;
    uint8_t dot_r;

    calendar_grid_cell_origin(&rect, &m, 0, &x, &y);
    assert(x == 10 + 4 * 111);
    assert(y == 84);
    calendar_annotation_dot_center(&rect, &m, 0, false, &dot_x, &dot_y, &dot_r);
    assert(dot_x == (int16_t)(x + 111 - 8));
    assert(dot_y == (int16_t)(y + 7));
    assert(dot_r == 3);
}

static void test_holiday_mark_moves_away_from_corner(void)
{
    calendar_grid_rect_t rect = {397, 136, 398, 420};
    calendar_grid_input_t in = {4, 0, 31, 480};
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    calendar_cell_pos_t cell = calendar_grid_cell(&rect, &m, 0);
    calendar_holiday_mark_t legacy = calendar_holiday_mark(&rect, &m, &cell, 0, false);
    calendar_holiday_mark_t avoid = calendar_holiday_mark(&rect, &m, &cell, 0, true);
    int16_t corner_x;
    int16_t corner_y;
    int16_t cell_x = (int16_t)(rect.x + cell.display_week * m.cell_w);
    int16_t cell_y = (int16_t)(rect.y + ((0 + m.adjusted_first_day) / 7) * m.cell_h);

    assert(legacy.text_x == (int16_t)(cell.bx + 31));
    assert(legacy.text_y == (int16_t)(cell.by + 3));
    assert(legacy.circle_x == (int16_t)(cell.bx + 36));
    assert(legacy.circle_y == (int16_t)(cell.by - 2));
    assert(legacy.circle_r == 10);

    calendar_annotation_corner_cursor(&rect, &m, 0, 36, 9, &corner_x, &corner_y);
    assert(avoid.text_x < (int16_t)(cell_x + m.cell_w / 2));
    assert(avoid.text_y > (int16_t)(cell_y + m.cell_h / 2));
    assert(avoid.text_y > (int16_t)(corner_y + 8));
    assert(avoid.circle_r == 10);
}

static void test_holiday_dot_is_separate_and_inside_every_cell(void)
{
    static const calendar_grid_rect_t rectangles[] = {
        {10, 84, 780, 470}, {397, 136, 398, 420},
        {10, 55, 406, 296}, {209, 55, 203, 296},
    };
    for (unsigned layout = 0; layout < 4; ++layout) {
        for (uint8_t first = 0; first < 7; ++first) {
            for (uint8_t start = 0; start < 7; ++start) {
                for (uint8_t days = 28; days <= 31; ++days) {
                    const calendar_grid_rect_t *rect = &rectangles[layout];
                    calendar_grid_input_t in = {first, start, days, layout < 2 ? 480 : 300};
                    calendar_grid_metrics_t m = calendar_grid_compute(rect, &in);
                    for (uint8_t day = 0; day < days; ++day) {
                        calendar_cell_pos_t cell = calendar_grid_cell(rect, &m, day);
                        calendar_holiday_mark_t holiday =
                            calendar_holiday_mark(rect, &m, &cell, day, false);
                        int16_t x, y, cell_x, cell_y, normal_x, normal_y;
                        uint8_t r, normal_r;
                        calendar_grid_cell_origin(rect, &m, day, &cell_x, &cell_y);
                        calendar_annotation_dot_center(rect, &m, day, true, &x, &y, &r);
                        calendar_annotation_dot_center(rect, &m, day, false,
                                                       &normal_x, &normal_y, &normal_r);
                        /* The optional today halo is also wholly within this cell. */
                        assert(x - r - 1 >= cell_x);
                        assert(y - r - 1 >= cell_y);
                        assert(x + r + 1 < cell_x + m.cell_w);
                        assert(y + r + 1 < cell_y + m.cell_h);
                        int32_t dx = x - holiday.circle_x;
                        int32_t dy = y - holiday.circle_y;
                        int32_t radii = r + 1 + holiday.circle_r;
                        assert(dx * dx + dy * dy > radii * radii);
                        assert(x + r + 1 < holiday.text_x);
                        /* Nonholiday dates keep the original right-corner position. */
                        assert(normal_x == cell_x + m.cell_w - (m.large ? 8 : 6));
                        assert(normal_y == y && normal_r == r);
                    }
                }
            }
        }
    }
}

int main(void)
{
    test_week_header_span_matches_full_and_split();
    test_full_month_grid_january_2026();
    test_split_month_grid_uses_right_rect();
    test_monday_week_start_shifts_first_column();
    test_annotation_text_sits_in_cell_top_right();
    test_cell_origin_and_dot_share_same_origin();
    test_holiday_mark_moves_away_from_corner();
    test_holiday_dot_is_separate_and_inside_every_cell();
    test_product_panel_does_not_ship_helvB14();
    puts("PASS: calendar grid");
    return 0;
}
