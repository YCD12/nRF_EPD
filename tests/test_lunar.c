#include <assert.h>
#include <stdio.h>

#include "Lunar.h"

static void test_display_range_is_2025_to_2027(void)
{
    assert(LUNAR_DISPLAY_YEAR_MIN == 2025);
    assert(LUNAR_DISPLAY_YEAR_MAX == 2027);
}

static void test_supported_new_years(void)
{
    struct Lunar_Date lunar;

    LUNAR_SolarToLunar(&lunar, 2025, 1, 29);
    assert(lunar.Year == 2025);
    assert(lunar.Month == 1);
    assert(lunar.Date == 1);
    assert(lunar.IsLeap == 0);

    LUNAR_SolarToLunar(&lunar, 2026, 2, 17);
    assert(lunar.Year == 2026);
    assert(lunar.Month == 1);
    assert(lunar.Date == 1);

    LUNAR_SolarToLunar(&lunar, 2027, 2, 6);
    assert(lunar.Year == 2027);
    assert(lunar.Month == 1);
    assert(lunar.Date == 1);
}

static void test_january_2025_before_cny_uses_2024_table(void)
{
    struct Lunar_Date lunar;

    LUNAR_SolarToLunar(&lunar, 2025, 1, 15);
    assert(lunar.Year == 2024);
    assert(lunar.Month == 12);
}

static void test_year_after_2027_does_not_use_2028_tables(void)
{
    struct Lunar_Date lunar;

    /* 2028-01-26 is 戊申正月初一 if 2028 rows remain. Display range
     * stops at 2027, so that date must fall back instead of becoming 2028. */
    LUNAR_SolarToLunar(&lunar, 2028, 1, 26);
    assert(lunar.Year != 2028);
}

static void test_august_11_2026_is_lunar_june_29(void)
{
    struct Lunar_Date lunar;

    LUNAR_SolarToLunar(&lunar, 2026, 8, 11);
    assert(lunar.Year == 2026);
    assert(lunar.Month == 6);
    assert(lunar.Date == 29);
    assert(lunar.IsLeap == 0);
}

static void test_august_11_2025_is_lunar_june_18(void)
{
    struct Lunar_Date lunar;

    LUNAR_SolarToLunar(&lunar, 2025, 8, 11);
    assert(lunar.Year == 2025);
    assert(lunar.Month == 6);
    assert(lunar.Date == 18);
    assert(lunar.IsLeap == 1);
}

int main(void)
{
    test_display_range_is_2025_to_2027();
    test_supported_new_years();
    test_january_2025_before_cny_uses_2024_table();
    test_year_after_2027_does_not_use_2028_tables();
    test_august_11_2025_is_lunar_june_18();
    test_august_11_2026_is_lunar_june_29();
    puts("PASS: lunar range");
    return 0;
}
