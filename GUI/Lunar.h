#ifndef __LUNAR_H
#define __LUNAR_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

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
    uint16_t Year;      /* Lunar year - 农历年份 (如: 2025)。数据库模式下临时充当六十甲子索引 (0-59) */
};

/**
 * @brief 单日农历记录 - Lunar Calendar Daily Record (12 bytes)
 * @note 与外置 Flash 中 LCDB (Type 6) 的每日记录二进制布局一一对应，
 *       由 tools/build_ext_resources.py 预生成。详见
 *       AiCoding/lunar_calendar_database_design_and_migration.md 第 3.4 节。
 */
typedef struct {
    uint8_t lunar_month;      /* 农历月份 (1-12)，最高位 Bit7=1 表示闰月 */
    uint8_t lunar_day;        /* 农历日期 (1-30) */
    uint8_t jiazi_year;       /* 六十甲子年索引 (0-59) */
    uint8_t jiazi_month;      /* 干支月 (预留) */
    uint8_t jiazi_day;        /* 干支日 (预留) */
    uint8_t solar_term;       /* 24节气索引 (0:无, 1-24 对应 LUNAR_GetJieQiName 索引+1) */
    uint8_t holiday_type;     /* 调休状态 (0:普通, 1:休, 2:班) */
    uint8_t reserved;         /* 对齐字节 */
    uint16_t festival_offset; /* 节日名在当年字符串池的偏移，0xFFFF 表示无 */
    uint16_t special_label;   /* 自定义标签偏移，0xFFFF 表示无 */
} daily_record_t;

/**
 * @brief 初始化农历数据库查询引擎 - Initialize LCDB query engine
 * @return 0:成功; 负值:失败 (魔数/资源缺失等)
 * @note 幂等，首次查询时会自动调用。
 */
int lunar_db_init(void);

/**
 * @brief 重置农历数据库查询引擎缓存 - Reset LCDB query engine caches
 * @note 外置资源热更新(RES_COMMIT)后调用，使新数据库无需重启即可生效。
 */
void lunar_db_reset(void);

/**
 * @brief 查询指定公历日期的每日农历记录 - Query daily record for a solar date
 * @param year 公历年份 (超出数据库范围时平稳降级为起始年份)
 * @param month 公历月份 (1-12)
 * @param day 公历日期 (1-31)
 * @param out_record 输出参数，存储查询到的记录
 * @return 0:成功; 负值:失败
 */
int lunar_db_get_record(uint16_t year, uint8_t month, uint8_t day, daily_record_t* out_record);

/**
 * @brief 读取当年字符串池中指定偏移的节日/标签文本 - Read a string from the year string pool
 * @param year 公历年份
 * @param offset 字符串在当年字符串池中的偏移 (来自 festival_offset / special_label)
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return 成功返回 buffer，失败返回空字符串
 */
const char* lunar_db_get_string(uint16_t year, uint16_t offset, char* buffer, size_t buffer_len);

/**
 * @brief 判断公历年份是否在 LCDB 覆盖范围内 - Whether a year has real LCDB data
 * @param year 公历年份
 * @return true:该年为精确数据; false:数据库缺失或该年超范围(被降级替代)
 * @note 用于"诚实降级"：为 false 时 UI 应提示数据待更新，而非静默显示替代数据。
 */
bool lunar_db_year_supported(uint16_t year);

const char* LUNAR_GetMonthName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetLeapName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetDateName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetDayName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetZodiacName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetStemName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetBranchName(uint8_t index, char* buffer, size_t buffer_len);
const char* LUNAR_GetJieQiName(uint8_t index, char* buffer, size_t buffer_len);

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
 * @return 生肖索引 (0-11)，用于 LUNAR_GetZodiacName()
 * @note 计算公式: 年份 % 12，注意数组顺序从猴开始
 */
uint8_t LUNAR_GetZodiac(const struct Lunar_Date* lunar);

/**
 * @brief 获取天干索引 - Get Heavenly Stem Index
 * @param lunar 农历日期结构体
 * @return 天干索引 (0-9)，用于 LUNAR_GetStemName()
 * @note 计算公式: 年份 % 10，注意数组顺序从庚开始
 */
uint8_t LUNAR_GetStem(const struct Lunar_Date* lunar);

/**
 * @brief 获取地支索引 - Get Earthly Branch Index
 * @param lunar 农历日期结构体
 * @return 地支索引 (0-11)，用于 LUNAR_GetBranchName()
 * @note 计算公式: 年份 % 12，注意数组顺序从申开始
 */
uint8_t LUNAR_GetBranch(const struct Lunar_Date* lunar);

/**
 * @brief 获取节气信息及距离天数 - Get Solar Term Info and Days Distance
 * @param myear 公历年份 (有效范围: 2025-2029)
 * @param mmonth 公历月份 (1-12)
 * @param mday 公历日期 (1-31)
 * @param day 输出参数，距离下一个节气的天数
 * @return 节气索引 (0-23)，用于 LUNAR_GetJieQiName()；0xFF表示失败
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
