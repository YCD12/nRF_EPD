/*
 * 农历数据库等价性回归测试 - LCDB Equivalence Regression Test (host build)
 * ----------------------------------------------------------------------------
 * 逐日对比"旧固件算法路径"与"新数据库路径"的输出，覆盖农历日期、干支、
 * 二十四节气、节日、调休。资源通过 res_hal（主机端 res_hal_host.c）读取
 * tools/epd_resources.bin，与真机 res_hal.c → SPI Flash 路径同源。
 *
 * 编译:
 *   gcc -std=c11 -IGUI -IEPD -Itools -Itools/test \
 *       tools/res_hal_host.c tools/test/test_lunar_db.c GUI/Lunar.c -o /tmp/test_lunar_db
 * 运行:
 *   /tmp/test_lunar_db tools/epd_resources.bin
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Lunar.h"
#include "epd_ext_resources.h"
#include "res_hal.h"
#include "res_hal_host.h"

int is_leap(int yr); /* Lunar.c 内部工具函数，本测试直接复用 */

/* ------------------------- 旧路径（现有固件逻辑）复刻 ------------------------- */

typedef struct {
    uint8_t month;
    uint8_t day;
    char name[10];
} Festival;

static bool old_get_festival(uint16_t year, uint8_t mon, uint8_t day, uint8_t week,
                             struct Lunar_Date* L, char* out) {
    epd_ext_resource_entry_t e;
    Festival item;

    if (res_hal_get_info(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_LUNAR_FESTIVALS, &e) == NRF_SUCCESS) {
        uint32_t n = e.length / sizeof(Festival);
        for (uint32_t i = 0; i < n; i++) {
            if (res_hal_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_LUNAR_FESTIVALS,
                             i * sizeof(Festival), &item, sizeof(Festival)) == NRF_SUCCESS &&
                L->Month == item.month && L->Date == item.day) {
                strcpy(out, item.name);
                return true;
            }
        }
    }

    if (L->Month == 12 && L->Date == 29) {
        struct Lunar_Date nx;
        struct devtm tm = {year, mon, day, 0, 0, 0, week};
        transformTime(transformTimeStruct(&tm) + 86400, &tm);
        LUNAR_SolarToLunar(&nx, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
        if (nx.Month == 1 && nx.Date == 1) {
            strcpy(out, "除夕");
            return true;
        }
    }
    if (mon == 5 && week == 0 && day >= 8 && day <= 14) { strcpy(out, "母亲节"); return true; }
    if (mon == 6 && week == 0 && day >= 15 && day <= 21) { strcpy(out, "父亲节"); return true; }
    if (mon == 11 && week == 4 && day >= 22 && day <= 28) { strcpy(out, "感恩节"); return true; }

    if (res_hal_get_info(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_SOLAR_FESTIVALS, &e) == NRF_SUCCESS) {
        uint32_t n = e.length / sizeof(Festival);
        for (uint32_t i = 0; i < n; i++) {
            if (res_hal_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_SOLAR_FESTIVALS,
                             i * sizeof(Festival), &item, sizeof(Festival)) == NRF_SUCCESS &&
                mon == item.month && day == item.day) {
                strcpy(out, item.name);
                return true;
            }
        }
    }
    return false;
}

static uint8_t old_holiday_type(uint16_t year, uint8_t mon, uint8_t day) {
    uint16_t hdr[2];
    if (res_hal_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_ADJUSTMENTS, 0, hdr, sizeof(hdr)) !=
        NRF_SUCCESS) {
        return 0;
    }
    if (hdr[0] != year) return 0;
    for (uint16_t i = 0; i < hdr[1]; i++) {
        uint16_t v;
        if (res_hal_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_ADJUSTMENTS, 4U + (uint32_t)i * 2U,
                         &v, 2) != NRF_SUCCESS) {
            break;
        }
        if (((v >> 8) & 0xF) == mon && (v & 0xFF) == day) {
            return (((v >> 12) & 0xF) > 0) ? 2 : 1;
        }
    }
    return 0;
}

static uint8_t old_solar_term(uint16_t year, uint8_t mon, uint8_t day) {
    uint8_t jqd;
    if (GetJieQi(year, mon, day, &jqd) && jqd == day) {
        uint8_t jq = (uint8_t)((mon - 1) * 2);
        if (day >= 15) jq++;
        return (uint8_t)(jq + 1);
    }
    return 0;
}

/* ------------------------------- 对比主流程 ------------------------------- */

static uint8_t days_in_month(uint16_t year, uint8_t mon) {
    static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon == 2 && is_leap((int)year)) return 29;
    return d[mon - 1];
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "tools/epd_resources.bin";
    const uint16_t years[] = {2025, 2026, 2027};
    int total = 0, mismatch = 0, printed = 0;

    if (res_hal_host_init(path) != 0) {
        fprintf(stderr, "res_hal_host_init failed: %s\n", path);
        return 2;
    }

    for (size_t yi = 0; yi < sizeof(years) / sizeof(years[0]); yi++) {
        uint16_t y = years[yi];
        for (uint8_t mon = 1; mon <= 12; mon++) {
            uint8_t dmax = days_in_month(y, mon);
            for (uint8_t day = 1; day <= dmax; day++) {
                total++;
                uint8_t week = day_of_week_get(mon, day, y);

                struct Lunar_Date L;
                LUNAR_SolarToLunar(&L, y, mon, day);

                daily_record_t rec;
                if (lunar_db_get_record(y, mon, day, &rec) != 0) {
                    if (printed++ < 40) printf("[%04u-%02u-%02u] DB record read FAILED\n", y, mon, day);
                    mismatch++;
                    continue;
                }

                bool ok = true;
                char reason[128] = "";

                uint8_t rec_leap = (uint8_t)(rec.lunar_month >> 7);
                uint8_t rec_month = (uint8_t)(rec.lunar_month & 0x7F);
                if (L.IsLeap != rec_leap || L.Month != rec_month || L.Date != rec.lunar_day) {
                    ok = false;
                    snprintf(reason, sizeof(reason), "lunar old(%u,%u,leap%u) new(%u,%u,leap%u)",
                             L.Month, L.Date, L.IsLeap, rec_month, rec.lunar_day, rec_leap);
                }

                if (ok && ((L.Year % 10) != (rec.jiazi_year % 10) || (L.Year % 12) != (rec.jiazi_year % 12))) {
                    ok = false;
                    snprintf(reason, sizeof(reason), "jiazi oldYear%%10=%u %%12=%u newJZ=%u",
                             (unsigned)(L.Year % 10), (unsigned)(L.Year % 12), rec.jiazi_year);
                }

                uint8_t ost = old_solar_term(y, mon, day);
                if (ok && ost != rec.solar_term) {
                    ok = false;
                    snprintf(reason, sizeof(reason), "solar_term old=%u new=%u", ost, rec.solar_term);
                }

                uint8_t oht = old_holiday_type(y, mon, day);
                if (ok && oht != rec.holiday_type) {
                    ok = false;
                    snprintf(reason, sizeof(reason), "holiday old=%u new=%u", oht, rec.holiday_type);
                }

                char oldf[16] = "";
                bool has = old_get_festival(y, mon, day, week, &L, oldf);
                const char* of = has ? oldf : "";
                char newf[32] = "";
                lunar_db_get_string(y, rec.festival_offset, newf, sizeof(newf));
                if (ok && strcmp(of, newf) != 0) {
                    ok = false;
                    snprintf(reason, sizeof(reason), "festival old='%s' new='%s'", of, newf);
                }

                if (!ok) {
                    mismatch++;
                    if (printed++ < 40) printf("[%04u-%02u-%02u] %s\n", y, mon, day, reason);
                }
            }
        }
    }

    res_hal_host_shutdown();
    printf("\n==== LCDB equivalence: %d days checked, %d mismatch ====\n", total, mismatch);
    return mismatch ? 1 : 0;
}
