#include <assert.h>
#include <stdio.h>

#include "epd_clock_window.h"

static void test_full_time_first_page(void)
{
    epd_clock_page_push_t page;

    /* HH:MM window inside TIME_REFRESH: x=56, y=11, w=272. */
    assert(epd_clock_map_gfx_page(56, 11, 56, 180, 56, 11, 272, 74, &page));
    assert(page.hw_x == 56);
    assert(page.hw_y == 180);
    assert(page.src_stride == 34);
    assert(page.src_x == 0);
    assert(page.src_y == 0);
}

static void test_full_time_second_page(void)
{
    epd_clock_page_push_t page;

    assert(epd_clock_map_gfx_page(56, 11, 56, 180, 56, 85, 272, 54, &page));
    assert(page.hw_x == 56);
    assert(page.hw_y == 254);
    assert(page.src_stride == 34);
    assert(page.src_x == 0);
    assert(page.src_y == 0);
}

static void test_minute_ones_uses_packed_window(void)
{
    epd_clock_page_push_t page;

    /* After GFX_setWindow the page starts at (0,0); do not use canvas x=248. */
    assert(epd_clock_map_gfx_page(248, 11, 248, 180, 248, 11, 72, 128, &page));
    assert(page.hw_x == 248);
    assert(page.hw_y == 180);
    assert(page.src_stride == 9);
    assert(page.src_x == 0);
    assert(page.src_y == 0);
}

static void test_rejects_mismatched_page(void)
{
    epd_clock_page_push_t page;

    assert(!epd_clock_map_gfx_page(56, 11, 56, 180, 0, 11, 272, 74, &page));
    assert(!epd_clock_map_gfx_page(56, 11, 56, 180, 56, 10, 272, 74, &page));
    assert(!epd_clock_map_gfx_page(56, 11, 56, 180, 56, 11, 271, 74, &page));
    assert(!epd_clock_map_gfx_page(56, 11, 56, 180, 56, 11, 272, 74, NULL));
}

int main(void)
{
    test_full_time_first_page();
    test_full_time_second_page();
    test_minute_ones_uses_packed_window();
    test_rejects_mismatched_page();
    puts("PASS: clock window page mapping");
    return 0;
}
