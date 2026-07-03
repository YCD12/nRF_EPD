#ifndef _GUI_PORTING_HAL_H
#define _GUI_PORTING_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 看门狗喂狗接口 - Watchdog feeding interface
 *        解耦 GUI 核心与具体硬件平台的看门狗实现
 */
void gui_porting_feed_wdt(void);

/* =============================================================================
 * 资源相关的结构体与常量声明（解耦 EPD 物理依赖）
 * ============================================================================= */

#define EPD_EXT_RES_TYPE_FONT              1U
#define EPD_EXT_RES_TYPE_LUNAR             2U
#define EPD_EXT_RES_TYPE_HOLIDAY           3U
#define EPD_EXT_RES_TYPE_UI                4U
#define EPD_EXT_RES_TYPE_LUNAR_DB          6U

#define EPD_EXT_LUNAR_DB_ENTRY             1U
#define EPD_EXT_LUNAR_DB_MAGIC             0x4C434442UL  /* "LCDB" */

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

#define EPD_EXT_UI_BUNDLE_MAGIC            0x314C4955UL
#define EPD_EXT_UI_BUNDLE_VERSION          1U
#define EPD_EXT_UI_SCHEMA_MAJOR            1U
#define EPD_EXT_UI_SCHEMA_MINOR            0U

typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} epd_ext_resource_entry_t;

/* =============================================================================
 * 电池电量配置常量
 * ============================================================================= */
#define LOW_BATTERY_THRESHOLD_MV       2800      // 低电量阈值(毫伏)
#define CRITICAL_BATTERY_SHUTDOWN_MV   2700      // 临界关机阈值(毫伏)

/* =============================================================================
 * L0 资源读取 HAL 接口声明（由底层平台实现提供链接）
 * ============================================================================= */
ret_code_t res_hal_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry);
ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // _GUI_PORTING_HAL_H
