#ifndef __LUNAR_H
#define __LUNAR_H
#include <stdint.h>
#include <string.h>

/*
 * 时间基准定义 - Time Reference Definitions
 */
#define YEAR0 (1900)        /* The first year - 基准年份（公元1900年） */
#define EPOCH_YR (1970)     /* EPOCH = Jan 1 1970 00:00:00 - Unix时间戳起始年份 */
#define SEC_PER_DY (86400)  /* Seconds per day - 一天的秒数 (24*60*60) */
#define SEC_PER_HR (3600)   /* Seconds per hour - 一小时的秒数 (60*60) */

/**
 * @brief 时间结构体 - Time Structure
 * @note 用于存储公历日期和时间信息
 */
typedef struct devtm {
    uint16_t tm_year;   /* Year - 年份 (如: 2025) */
    uint8_t tm_mon;     /* Month - 月份 (0-11, 0表示1月) */
    uint8_t tm_mday;    /* Day of month - 日期 (1-31) */
    uint8_t tm_hour;    /* Hour - 小时 (0-23) */
    uint8_t tm_min;     /* Minute - 分钟 (0-59) */
    uint8_t tm_sec;     /* Second - 秒 (0-59) */
    uint8_t tm_wday;    /* Day of week - 星期几 (0-6, 0表示星期日) */
} tm_t;

/**
 * @brief 农历日期结构体 - Lunar Calendar Date Structure
 * @note 用于存储农历日期信息
 */
struct Lunar_Date {
    uint8_t IsLeap;     /* Leap month flag - 闰月标志 (1:闰月, 0:平月) */
    uint8_t Date;       /* Lunar day - 农历日期 (1-30) */
    uint8_t Month;      /* Lunar month - 农历月份 (1-12) */
    uint16_t Year;      /* Lunar year - 农历年份 (如: 2025) */
};

/*
 * 农历字符串常量声明 - Lunar Calendar String Constants Declaration
 * 这些字符串用于显示农历日期的中文表示
 */
extern const char Lunar_MonthString[13][7];     /* 农历月份名称: 正月~腊月 */
extern const char Lunar_MonthLeapString[2][4];  /* 闰月标识: " "/闰 */
extern const char Lunar_DateString[31][7];      /* 农历日期名称: 初一~三十 */
extern const char Lunar_DayString[7][4];        /* 星期名称: 日/一/二/三/四/五/六 */
extern const char Lunar_ZodiacString[12][4];    /* 十二生肖: 鼠/牛/虎/兔/龙/蛇/马/羊/猴/鸡/狗/猪 */
extern const char Lunar_StemStrig[10][4];       /* 十天干: 甲/乙/丙/丁/戊/己/庚/辛/壬/癸 */
extern const char Lunar_BranchStrig[12][4];     /* 十二地支: 子/丑/寅/卯/辰/巳/午/未/申/酉/戌/亥 */
extern const char JieQiStr[24][7];              /* 二十四节气名称 */

/*
 * 农历转换函数声明 - Lunar Calendar Conversion Function Declarations
 */

/**
 * @brief 公历转农历 - Solar to Lunar Calendar Conversion
 * @param lunar 输出参数，存储转换后的农历日期
 * @param solar_year 公历年份 (有效范围: 2025-2029)
 * @param solar_month 公历月份 (1-12)
 * @param solar_date 公历日期 (1-31)
 * @note 超出有效年份范围时，将使用2025年的农历数据
 */
void LUNAR_SolarToLunar(struct Lunar_Date* lunar, uint16_t solar_year, uint8_t solar_month, uint8_t solar_date);

/**
 * @brief 获取生肖索引 - Get Chinese Zodiac Index
 * @param lunar 农历日期结构体
 * @return 生肖索引 (0-11)，用于索引 Lunar_ZodiacString 数组
 * @note 计算公式: 年份 % 12，注意数组顺序从猴开始
 */
uint8_t LUNAR_GetZodiac(const struct Lunar_Date* lunar);

/**
 * @brief 获取天干索引 - Get Heavenly Stem Index
 * @param lunar 农历日期结构体
 * @return 天干索引 (0-9)，用于索引 Lunar_StemStrig 数组
 * @note 计算公式: 年份 % 10，注意数组顺序从庚开始
 */
uint8_t LUNAR_GetStem(const struct Lunar_Date* lunar);

/**
 * @brief 获取地支索引 - Get Earthly Branch Index
 * @param lunar 农历日期结构体
 * @return 地支索引 (0-11)，用于索引 Lunar_BranchStrig 数组
 * @note 计算公式: 年份 % 12，注意数组顺序从申开始
 */
uint8_t LUNAR_GetBranch(const struct Lunar_Date* lunar);

/**
 * @brief 获取节气信息及距离天数 - Get Solar Term Info and Days Distance
 * @param myear 公历年份 (有效范围: 2025-2029)
 * @param mmonth 公历月份 (1-12)
 * @param mday 公历日期 (1-31)
 * @param day 输出参数，距离下一个节气的天数
 * @return 节气索引 (0-23)，用于索引 JieQiStr 数组；0xFF表示失败
 */
uint8_t GetJieQiStr(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* day);

/**
 * @brief 获取指定日期所在半月的节气日期 - Get Solar Term Date
 * @param myear 公历年份 (有效范围: 2025-2029)
 * @param mmonth 公历月份 (1-12)
 * @param mday 公历日期 (1-31)，用于判断上半月/下半月
 * @param JQdate 输出参数，节气的日期
 * @return 1:成功, 0:失败
 * @note 每月有两个节气，上半月(1-14日)返回第一个节气，下半月(15-31日)返回第二个节气
 */
uint8_t GetJieQi(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* JQdate);

/*
 * 时间转换函数声明 - Time Conversion Function Declarations
 */

/**
 * @brief Unix时间戳转时间结构体 - Convert Unix Timestamp to Time Structure
 * @param unix_time Unix时间戳（从1970年1月1日0时0分0秒起的秒数）
 * @param result 输出参数，存储转换后的时间结构体
 */
void transformTime(uint32_t unix_time, struct devtm* result);

/**
 * @brief 时间结构体转Unix时间戳 - Convert Time Structure to Unix Timestamp
 * @param result 时间结构体
 * @return Unix时间戳
 */
uint32_t transformTimeStruct(struct devtm* result);

/**
 * @brief 获取指定月份第一天是星期几 - Get Day of Week for First Day of Month
 * @param year 公历年份
 * @param month 公历月份 (1-12)
 * @return 星期几 (0-6, 0表示星期日)
 */
uint8_t get_first_day_week(uint16_t year, uint8_t month);

/**
 * @brief 获取指定月份的最后一天 - Get Last Day of Month
 * @param year 公历年份
 * @param month 公历月份 (1-12)
 * @return 该月最大日期 (28/29/30/31)
 */
uint8_t get_last_day(uint16_t year, uint8_t month);

/**
 * @brief 根据年月日计算星期几 - Calculate Day of Week from Date
 * @param month 公历月份 (1-12)
 * @param day 公历日期 (1-31)
 * @param year 公历年份
 * @return 星期几 (0-6, 0表示星期日)
 * @note 使用蔡勒公式(Zeller's Formula)的变体
 */
unsigned char day_of_week_get(unsigned char month, unsigned char day, unsigned short year);

/**
 * @brief 获取指定月份的天数 - Get Number of Days in Month
 * @param year 公历年份 (仅使用低8位，即对256取模)
 * @param month 公历月份 (1-12)
 * @return 该月的天数
 */
uint8_t thisMonthMaxDays(uint8_t year, uint8_t month);

#endif
