/*
 * 农历数据库等价性回归测试 - LCDB Equivalence Regression Test (host build)
 * ----------------------------------------------------------------------------
 * 逐日对比"旧固件算法路径"(LUNAR_SolarToLunar / GetJieQi / GetFestival 逻辑)
 * 与"新数据库路径"(lunar_db_get_record / lunar_db_get_string) 的输出，覆盖
 * 农历日期、干支、二十四节气、节日、调休。两者对 2025-2027 全年必须完全一致，
 * 以此在切换 GUI.c 到数据驱动之前，证明预生成数据库与现有计算 100% 等价。
 *
 * 资源来自 tools/epd_resources.bin（由 build_ext_resources.py 生成），通过
 * 一个模拟的 epd_ext_resource_read/get_info 读取，路径与真机完全同源。
 *
 * 编译:
 *   gcc -std=c11 -IGUI -IEPD -Itools/test tools/test/test_lunar_db.c GUI/Lunar.c -o /tmp/test_lunar_db
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

int is_leap(int yr); /* Lunar.c 内部工具函数，本测试直接复用 */

/* ------------------------- 模拟外置 Flash 资源读取 ------------------------- */

#define IMG_BASE       0x1000UL
#define IMG_END        0x7F000UL
#define IMG_SIZE       (IMG_END - IMG_BASE)
#define MANIFEST_ADDR  0x7E000UL
#define ENTRY_STRIDE   16U
#define ENTRY_MAX      24U

static uint8_t g_img[IMG_SIZE];

typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} manifest_entry_t;

static manifest_entry_t g_entries[ENTRY_MAX];
static uint16_t g_entry_count;

static uint16_t rd_u16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    size_t n = fread(g_img, 1, IMG_SIZE, f);
    fclose(f);
    if (n < (MANIFEST_ADDR - IMG_BASE) + 8) {
        fprintf(stderr, "image too small: %zu bytes\n", n);
        return -1;
    }

    const uint8_t* m = g_img + (MANIFEST_ADDR - IMG_BASE);
    uint32_t magic = rd_u32(m);
    if (magic != EPD_EXT_RES_MAGIC) {
        fprintf(stderr, "bad manifest magic 0x%08X\n", magic);
        return -1;
    }
    g_entry_count = rd_u16(m + 6);
    if (g_entry_count > ENTRY_MAX) g_entry_count = ENTRY_MAX;
    const uint8_t* e = m + 8;
    for (uint16_t i = 0; i < g_entry_count; i++) {
        g_entries[i].type = rd_u16(e + 0);
        g_entries[i].id = rd_u16(e + 2);
        g_entries[i].offset = rd_u32(e + 4);
        g_entries[i].length = rd_u32(e + 8);
        g_entries[i].crc32 = rd_u32(e + 12);
        e += ENTRY_STRIDE;
    }
    return 0;
}

static const manifest_entry_t* find_entry(uint16_t type, uint16_t id) {
    for (uint16_t i = 0; i < g_entry_count; i++) {
        if (g_entries[i].type == type && g_entries[i].id == id) return &g_entries[i];
    }
    return NULL;
}

ret_code_t epd_ext_resource_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry) {
    const manifest_entry_t* e = find_entry(type, id);
    if (!e) return NRF_ERROR_NOT_FOUND;
    if (entry) {
        entry->type = e->type;
        entry->id = e->id;
        entry->offset = e->offset;
        entry->length = e->length;
        entry->crc32 = e->crc32;
    }
    return NRF_SUCCESS;
}

ret_code_t epd_ext_resource_read(uint16_t type, uint16_t id, uint32_t offset, void* data, size_t length) {
    const manifest_entry_t* e = find_entry(type, id);
    if (!e || data == NULL) return NRF_ERROR_NOT_FOUND;
    if (offset >= e->length) return NRF_ERROR_INVALID_PARAM;
    size_t avail = e->length - offset;
    size_t n = (length < avail) ? length : avail;
    memcpy(data, g_img + (e->offset - IMG_BASE) + offset, n);
    if (n < length) memset((uint8_t*)data + n, 0, length - n);
    return NRF_SUCCESS;
}

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

    if (epd_ext_resource_get_info(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_LUNAR_FESTIVALS, &e) == NRF_SUCCESS) {
        uint32_t n = e.length / sizeof(Festival);
        for (uint32_t i = 0; i < n; i++) {
            if (epd_ext_resource_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_LUNAR_FESTIVALS,
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

    if (epd_ext_resource_get_info(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_SOLAR_FESTIVALS, &e) == NRF_SUCCESS) {
        uint32_t n = e.length / sizeof(Festival);
        for (uint32_t i = 0; i < n; i++) {
            if (epd_ext_resource_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_SOLAR_FESTIVALS,
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
    if (epd_ext_resource_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_ADJUSTMENTS, 0, hdr, sizeof(hdr)) !=
        NRF_SUCCESS) {
        return 0;
    }
    if (hdr[0] != year) return 0;
    for (uint16_t i = 0; i < hdr[1]; i++) {
        uint16_t v;
        if (epd_ext_resource_read(EPD_EXT_RES_TYPE_HOLIDAY, EPD_EXT_HOLIDAY_ADJUSTMENTS, 4U + (uint32_t)i * 2U,
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
    if (load_image(path) != 0) return 2;

    const uint16_t years[] = {2025, 2026, 2027};
    int total = 0, mismatch = 0, printed = 0;

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

    printf("\n==== LCDB equivalence: %d days checked, %d mismatch ====\n", total, mismatch);
    return mismatch ? 1 : 0;
}
