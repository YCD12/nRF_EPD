#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

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

static void assert_unix_time_matches_utc(uint32_t seconds)
{
    tm_t actual;
    time_t unix_time = (time_t)seconds;
    const struct tm *expected = gmtime(&unix_time);
    assert(expected != NULL);
    transformTime(seconds, &actual);
    assert(actual.tm_year == expected->tm_year);
    assert(actual.tm_mon == expected->tm_mon);
    assert(actual.tm_mday == expected->tm_mday);
    assert(actual.tm_hour == expected->tm_hour);
    assert(actual.tm_min == expected->tm_min);
    assert(actual.tm_sec == expected->tm_sec);
    assert(actual.tm_wday == expected->tm_wday);
}

static void test_unix_time_conversion(void)
{
    const uint32_t boundary[] = {
        0U, 1U, 951782399U, 951782400U, /* 2000 leap day */
        1709164799U, 1709164800U,       /* 2024 leap day */
        2147483647U, 4107542399U,      /* 2038 and 2100 */
        UINT32_MAX - 1U, UINT32_MAX,
    };
    for (size_t i = 0; i < sizeof(boundary) / sizeof(boundary[0]); ++i)
        assert_unix_time_matches_utc(boundary[i]);

    /* Check dates throughout the full 32-bit timestamp range, including both
     * ends of each sampled day against the host UTC library. */
    for (uint64_t day = 0; day * 86400U <= UINT32_MAX; day += 37U) {
        uint64_t midnight = day * 86400U;
        assert_unix_time_matches_utc((uint32_t)midnight);
        if (midnight + 86399U <= UINT32_MAX)
            assert_unix_time_matches_utc((uint32_t)(midnight + 86399U));
    }
}

int main(void)
{
    test_display_range_is_2025_to_2027();
    test_supported_new_years();
    test_january_2025_before_cny_uses_2024_table();
    test_year_after_2027_does_not_use_2028_tables();
    test_august_11_2025_is_lunar_june_18();
    test_august_11_2026_is_lunar_june_29();
    test_unix_time_conversion();
    puts("PASS: lunar range");
    return 0;
}
