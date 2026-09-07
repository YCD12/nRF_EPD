#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "clock_text.h"

static void test_lunar_date_concatenates_leap_month_day(void)
{
    char buf[16];

    clock_format_lunar_md(buf, sizeof(buf), "", "正月", "初一");
    assert(strcmp(buf, "正月初一") == 0);

    clock_format_lunar_md(buf, sizeof(buf), "闰", "六月", "十五");
    assert(strcmp(buf, "闰六月十五") == 0);
}

static void test_jieqi_today_uses_name_only(void)
{
    char line1[16];
    char line2[16];

    clock_format_jieqi_countdown(line1, sizeof(line1), line2, sizeof(line2), "小暑", 0);
    assert(strcmp(line1, "小暑") == 0);
    assert(line2[0] == '\0');
}

static void test_jieqi_countdown_two_lines(void)
{
    char line1[16];
    char line2[16];

    clock_format_jieqi_countdown(line1, sizeof(line1), line2, sizeof(line2), "小暑", 3);
    assert(strcmp(line1, "离小暑") == 0);
    assert(strcmp(line2, "还有3天") == 0);

    clock_format_jieqi_countdown(line1, sizeof(line1), line2, sizeof(line2), "冬至", 12);
    assert(strcmp(line1, "离冬至") == 0);
    assert(strcmp(line2, "还有12天") == 0);
}

static void test_ssid_tail_clamps_short_and_empty_names(void)
{
    char buf[5];

    clock_ssid_tail4(buf, sizeof(buf), "NRF_EPD");
    assert(strcmp(buf, "_EPD") == 0);

    clock_ssid_tail4(buf, sizeof(buf), "AB");
    assert(strcmp(buf, "AB") == 0);

    clock_ssid_tail4(buf, sizeof(buf), "");
    assert(buf[0] == '\0');

    clock_ssid_tail4(buf, sizeof(buf), NULL);
    assert(buf[0] == '\0');
}

int main(void)
{
    test_lunar_date_concatenates_leap_month_day();
    test_jieqi_today_uses_name_only();
    test_jieqi_countdown_two_lines();
    test_ssid_tail_clamps_short_and_empty_names();
    puts("PASS: clock text");
    return 0;
}
