#ifndef CALENDAR_RESOURCES_H
#define CALENDAR_RESOURCES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAL_RES_MONTH_NAMES = 1, CAL_RES_LEAP_NAMES, CAL_RES_DATE_NAMES,
    CAL_RES_WEEKDAY_NAMES, CAL_RES_ZODIAC_NAMES, CAL_RES_STEM_NAMES,
    CAL_RES_BRANCH_NAMES, CAL_RES_JIEQI_NAMES, CAL_RES_LUNAR_MONTH_DAYS,
    CAL_RES_SOLAR_NEW_YEAR, CAL_RES_JIEQI_YEAR_BITS, CAL_RES_JIEQI_BASE_DAYS,
    CAL_RES_SOLAR_FESTIVALS, CAL_RES_LUNAR_FESTIVALS, CAL_RES_HOLIDAYS,
    CAL_RES_SPECIAL_FESTIVAL_NAMES,
    CAL_RES_SECTION_COUNT
} calendar_resource_section_t;

typedef enum {
    CAL_SPECIAL_NEW_YEARS_EVE = 0,
    CAL_SPECIAL_MOTHERS_DAY,
    CAL_SPECIAL_FATHERS_DAY,
    CAL_SPECIAL_THANKSGIVING,
    CAL_SPECIAL_QINGMING
} calendar_special_festival_t;

bool calendar_resources_init(void);
/* Retry failed initialization only; valid metadata remains cached. */
bool calendar_resources_retry_init(void);
void calendar_resources_invalidate(void);
bool calendar_resources_ready(void);
uint16_t calendar_resources_year_min(void);
uint16_t calendar_resources_year_max(void);
uint16_t calendar_resources_holiday_year(void);
bool calendar_resource_read(calendar_resource_section_t section, uint16_t index,
                            void *data, size_t length);
bool calendar_resource_text(calendar_resource_section_t section, uint16_t index,
                            char *text, size_t capacity);
bool calendar_resource_u32(calendar_resource_section_t section, uint16_t index,
                           uint32_t *value);
bool calendar_resource_u8(calendar_resource_section_t section, uint16_t index,
                          uint8_t *value);
bool calendar_resource_festival(calendar_resource_section_t section,
                                uint8_t month, uint8_t day,
                                char *name, size_t capacity);
bool calendar_resource_holiday(uint16_t year, uint8_t month, uint8_t day, bool *work);

#if defined(CALENDAR_RESOURCES_HOST_TEST)
void calendar_resources_host_reset(void);
#endif
#endif
