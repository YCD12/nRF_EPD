#ifndef __LUNAR_H
#define __LUNAR_H
#include <stdint.h>
#include <string.h>

/*
 * 时间基准
 */
#define YEAR0 (1900)       /* 基准年份 */
#define EPOCH_YR (1970)    /* Unix 时间戳起始年（1970-01-01 00:00:00） */
#define SEC_PER_DY (86400) /* 一天秒数 */
#define SEC_PER_HR (3600)  /* 一小时秒数 */

/* 公历显示窗口。表中保留 2024 行，以便 2025-01-01 至春节仍能解析为甲辰腊月。
 * 窗口外的年份复用 2025 年数据。 */
#define LUNAR_DISPLAY_YEAR_MIN 2025
#define LUNAR_DISPLAY_YEAR_MAX 2027

/**
 * @brief 公历时间
 */
typedef struct devtm {
    uint16_t tm_year; /* 年份（如 2025） */
    uint8_t tm_mon;   /* 月份（0-11，0 表示 1 月） */
    uint8_t tm_mday;  /* 日（1-31） */
    uint8_t tm_hour;  /* 时（0-23） */
    uint8_t tm_min;   /* 分（0-59） */
    uint8_t tm_sec;   /* 秒（0-59） */
    uint8_t tm_wday;  /* 星期（0-6，0 为星期日） */
} tm_t;

/**
 * @brief 农历日期
 */
struct Lunar_Date {
    uint8_t IsLeap;  /* 闰月标志（1:闰月, 0:平月） */
    uint8_t Date;    /* 农历日（1-30） */
    uint8_t Month;   /* 农历月（1-12） */
    uint16_t Year;   /* 农历年（如 2025） */
};

/*
 * 农历显示用中文字符串
 */
const char* LUNAR_MonthName(uint8_t index);
const char* LUNAR_LeapName(uint8_t index);
const char* LUNAR_DateName(uint8_t index);
const char* LUNAR_WeekdayName(uint8_t index);
const char* LUNAR_ZodiacName(uint8_t index);
const char* LUNAR_StemName(uint8_t index);
const char* LUNAR_BranchName(uint8_t index);
const char* LUNAR_JieQiName(uint8_t index);

/**
 * @brief 公历转农历
 * @param lunar 输出农历日期
 * @param solar_year 公历年（有效 2025-2027）
 * @param solar_month 公历月（1-12）
 * @param solar_date 公历日（1-31）
 * @note 超出有效年份时使用 2025 年农历数据
 */
void LUNAR_SolarToLunar(struct Lunar_Date* lunar, uint16_t solar_year, uint8_t solar_month, uint8_t solar_date);

/**
 * @brief 生肖索引（0-11），对应 Lunar_ZodiacString
 * @param lunar 农历日期
 * @return 生肖索引（0-11）
 * @note 年份 % 12；数组顺序从猴开始
 */
uint8_t LUNAR_GetZodiac(const struct Lunar_Date* lunar);

/**
 * @brief 天干索引（0-9），对应 Lunar_StemStrig
 * @param lunar 农历日期
 * @return 天干索引（0-9）
 * @note 年份 % 10；数组顺序从庚开始
 */
uint8_t LUNAR_GetStem(const struct Lunar_Date* lunar);

/**
 * @brief 地支索引（0-11），对应 Lunar_BranchStrig
 * @param lunar 农历日期
 * @return 地支索引（0-11）
 * @note 年份 % 12；数组顺序从申开始
 */
uint8_t LUNAR_GetBranch(const struct Lunar_Date* lunar);

/**
 * @brief 节气及距下一节气天数
 * @param myear 公历年
 * @param mmonth 公历月（1-12）
 * @param mday 公历日（1-31）
 * @param day 输出：距下一节气的天数
 * @return 节气索引（0-23）；0xFF 表示失败
 */
uint8_t GetJieQiStr(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* day);

/**
 * @brief 当前半月对应的节气日期
 * @param myear 公历年
 * @param mmonth 公历月（1-12）
 * @param mday 用于判断上半月/下半月
 * @param JQdate 输出节气日
 * @return 1 成功，0 失败
 * @note 每月两个节气：1-14 日取第一个，15-31 日取第二个
 */
uint8_t GetJieQi(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* JQdate);

/**
 * @brief Unix 时间戳转时间结构体（自 1970-01-01 起的秒数）
 * @param unix_time Unix 时间戳
 * @param result 输出时间结构体
 */
void transformTime(uint32_t unix_time, struct devtm* result);

/**
 * @brief 时间结构体转 Unix 时间戳
 * @param result 时间结构体
 * @return Unix 时间戳
 */
uint32_t transformTimeStruct(const struct devtm* result);

/**
 * @brief 指定月份 1 号是星期几（0-6，0 为星期日）
 * @param year 公历年
 * @param month 公历月（1-12）
 */
uint8_t get_first_day_week(uint16_t year, uint8_t month);

/**
 * @brief 指定月份最后一天（28/29/30/31）
 * @param year 公历年
 * @param month 公历月（1-12）
 */
uint8_t get_last_day(uint16_t year, uint8_t month);

/**
 * @brief 由年月日计算星期几（0-6，0 为星期日）
 * @param month 公历月（1-12）
 * @param day 公历日
 * @param year 公历年
 * @note 蔡勒公式变体
 */
unsigned char day_of_week_get(unsigned char month, unsigned char day, unsigned short year);

/**
 * @brief 指定月份天数
 * @param year 仅使用低 8 位（对 256 取模）
 * @param month 公历月（1-12）
 */
uint8_t thisMonthMaxDays(uint8_t year, uint8_t month);

#endif
