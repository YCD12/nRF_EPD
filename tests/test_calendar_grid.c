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

int main(void)
{
    test_week_header_span_matches_full_and_split();
    test_full_month_grid_january_2026();
    test_split_month_grid_uses_right_rect();
    test_monday_week_start_shifts_first_column();
    test_product_panel_does_not_ship_helvB14();
    puts("PASS: calendar grid");
    return 0;
}
