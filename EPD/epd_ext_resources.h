#ifndef EPD_EXT_RESOURCES_H
#define EPD_EXT_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fm25q04b.h"
#include "sdk_errors.h"

/*
 * FM25Q04B resource layout:
 *   0x01000-0x7CFFF  resource data
 *   0x7E000-0x7EFFF  active manifest
 *   0x7F000-0x7FFFF  reserved
 *
 * The web updater writes all resource data first, verifies it, writes the
 * manifest sector last, then COMMIT validates and activates it in RAM.
 */
#define EPD_EXT_RES_MAIN_MANIFEST_ADDR     0x7E000UL
#define EPD_EXT_RES_DATA_START_ADDR        0x01000UL
#define EPD_EXT_RES_DATA_END_ADDR          0x7CFFFUL
#define EPD_EXT_RES_STAGING_MANIFEST_ADDR  0x7E000UL
#define EPD_EXT_RES_RESERVED_START_ADDR    0x7F000UL

#define EPD_EXT_RES_MAGIC                  0x45505253UL
#define EPD_EXT_RES_VERSION                1U
#define EPD_EXT_RES_MAX_ENTRIES            24U

#define EPD_EXT_RES_TYPE_FONT              1U
#define EPD_EXT_RES_TYPE_LUNAR             2U
#define EPD_EXT_RES_TYPE_HOLIDAY           3U
#define EPD_EXT_RES_TYPE_UI                4U
#define EPD_EXT_RES_TYPE_LUT               5U

/* UC8179 partial-refresh LUTs.
 * One resource per temperature tier (id = 0..10), each entry contains 6 tables
 * (LUTC, LUTWW, LUTKW, LUTWK, LUTKK, LUTBD) of 42 bytes each, concatenated in
 * that order. Total payload per tier = 252 bytes. */
#define EPD_EXT_LUT_TIER_TABLE_SIZE        42U
#define EPD_EXT_LUT_TIER_TABLE_COUNT       6U
#define EPD_EXT_LUT_TIER_PAYLOAD_SIZE      (EPD_EXT_LUT_TIER_TABLE_SIZE * EPD_EXT_LUT_TIER_TABLE_COUNT)
#define EPD_EXT_LUT_TIER_COUNT             11U

#define EPD_EXT_FONT_WQY9_LUNAR            1U
#define EPD_EXT_FONT_WQY12_LUNAR           2U
#define EPD_EXT_FONT_HELVB14_TN            3U
#define EPD_EXT_FONT_HELVB18_TN            4U

#define EPD_EXT_LUNAR_MONTH_NAMES          1U
#define EPD_EXT_LUNAR_LEAP_NAMES           2U
#define EPD_EXT_LUNAR_DATE_NAMES           3U
#define EPD_EXT_LUNAR_DAY_NAMES            4U
#define EPD_EXT_LUNAR_ZODIAC_NAMES         5U
#define EPD_EXT_LUNAR_STEM_NAMES           6U
#define EPD_EXT_LUNAR_BRANCH_NAMES         7U
#define EPD_EXT_LUNAR_JIEQI_NAMES          8U
#define EPD_EXT_LUNAR_MONTH_DAYS           9U
#define EPD_EXT_LUNAR_SOLAR_1_1            10U
#define EPD_EXT_LUNAR_JIEQI_YEAR_BITS      11U
#define EPD_EXT_LUNAR_JIEQI_BASE_DAYS      12U

#define EPD_EXT_HOLIDAY_SOLAR_FESTIVALS    1U
#define EPD_EXT_HOLIDAY_LUNAR_FESTIVALS    2U
#define EPD_EXT_HOLIDAY_ADJUSTMENTS        3U

#define EPD_EXT_UI_BUNDLE                  1U

typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} epd_ext_resource_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t resource_count;
    epd_ext_resource_entry_t entries[EPD_EXT_RES_MAX_ENTRIES];
    uint32_t crc32;
} epd_ext_resource_manifest_t;

typedef struct {
    bool flash_ready;
    bool manifest_valid;
    uint16_t resource_count;
    uint32_t manifest_crc32;
    uint32_t flash_total_bytes;
    uint32_t resource_area_bytes;
    uint32_t resource_used_bytes;
    uint32_t resource_free_bytes;
    fm25q04b_jedec_id_t jedec_id;
} epd_ext_resource_status_t;

ret_code_t epd_ext_resources_init(void);
ret_code_t epd_ext_resources_wake(void);
ret_code_t epd_ext_resources_sleep(void);

ret_code_t epd_ext_resources_get_status(epd_ext_resource_status_t* status);

ret_code_t epd_ext_resource_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry);
ret_code_t epd_ext_resource_read(uint16_t type, uint16_t id, uint32_t offset, void* data, size_t length);
ret_code_t epd_ext_resource_read_raw(uint32_t address, void* data, size_t length);

ret_code_t epd_ext_resource_erase(uint32_t address, size_t length);
ret_code_t epd_ext_resource_write(uint32_t address, const void* data, size_t length);
ret_code_t epd_ext_resource_commit_staging(void);

#endif
