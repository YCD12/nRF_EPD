#include "GUI.h"

#include <stdio.h>

#include "EPD_driver.h"
#include "Lunar.h"
#include "fonts.h"

// 计算数组元素个数的宏定义
// Macro to calculate array size
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// 带样式的printf宏，简化设置前景色、背景色、字体的操作
// Styled printf macro to simplify setting foreground, background color and font
#define GFX_printf_styled(gfx, fg, bg, font, ...) \
    GFX_setTextColor(gfx, fg, bg);                \
    GFX_setFont(gfx, font);                       \
    GFX_printf(gfx, __VA_ARGS__);

#define BATTERY_FULL_MV 3300

// 节日数据结构体
// Festival data structure
typedef struct {
    uint8_t month;      // 月份 | Month
    uint8_t day;        // 日期 | Day
    char name[10];      // 节日名称(3x3+1) 最多3个汉字 | Festival name (max 3 Chinese characters)
} Festival;

// 公历节日数据表
// Gregorian calendar festivals data table
static const Festival festivals[] = {
    {1, 1, "元旦节"},  {2, 14, "情人节"}, {3, 8, "妇女节"},  {3, 12, "植树节"},  {4, 1, "愚人节"},
    {5, 1, "劳动节"},  {5, 4, "青年节"},  {6, 1, "儿童节"},  {7, 1, "建党节"},   {8, 1, "建军节"},
    {9, 10, "教师节"}, {10, 1, "国庆节"}, {11, 1, "万圣节"}, {12, 24, "平安夜"}, {12, 25, "圣诞节"},
};

// 农历节日数据表
// Lunar calendar festivals data table
static const Festival festivals_lunar[] = {
    {1, 1, "春节"},    {1, 15, "元宵节"}, {2, 2, "龙抬头"},  {5, 5, "端午节"},  {7, 7, "七夕节"}, {7, 15, "中元节"},
    {8, 15, "中秋节"}, {9, 9, "重阳节"},  {10, 1, "寒衣节"}, {12, 8, "腊八节"}, {12, 30, "除夕"},
};

// 放假和调休数据，每年更新
// Holiday and workday adjustment data, updated annually
// 数据格式：0xWMMDD，其中W为工作日标志(1表示需要上班，0表示放假)，MM为月份，DD为日期
// Data format: 0xWMMDD, where W is workday flag (1=work, 0=holiday), MM is month, DD is day
#define HOLIDAY_YEAR 2026
static const uint16_t holidays[] = {
    0x0101, 0x0102, 0x0103, 0x1104, 0x120E, 0x020F, 0x0210, 0x0211, 0x0212, 0x0213, 0x0214, 0x0215, 0x0216,
    0x0217, 0x121C, 0x0404, 0x0405, 0x0406, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x1509, 0x0613, 0x0614,
    0x0615, 0x0919, 0x091A, 0x091B, 0x1914, 0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x1A0A,
};

/**
 * @brief 查询指定日期是否为法定节假日或调休工作日
 *        Query if a specific date is a legal holiday or adjusted workday
 *
 * @param mon  月份(1-12) | Month (1-12)
 * @param day  日期(1-31) | Day (1-31)
 * @param work 输出参数：true表示调休需上班，false表示放假 | Output: true=work, false=holiday
 * @return true 该日期在节假日表中 | Date found in holiday table
 * @return false 该日期为普通工作日 | Date is a normal working day
 */
static bool GetHoliday(uint8_t mon, uint8_t day, bool* work) {
    for (uint8_t i = 0; i < ARRAY_SIZE(holidays); i++) {
        // 提取月份：从高8位取低4位 | Extract month from bits 8-11
        // 提取日期：取低8位 | Extract day from low 8 bits
        if (((holidays[i] >> 8) & 0xF) == mon && (holidays[i] & 0xFF) == day) {
            // 提取工作日标志：取最高4位 | Extract workday flag from bits 12-15
            *work = ((holidays[i] >> 12) & 0xF) > 0;
            return true;
        }
    }
    return false;
}

/**
 * @brief 获取指定日期的节日名称
 *        Get festival name for a specific date
 *
 * @param year    公历年份 | Gregorian year
 * @param mon     公历月份(1-12) | Gregorian month (1-12)
 * @param day     公历日期(1-31) | Gregorian day (1-31)
 * @param week    星期几(0=周日, 6=周六) | Day of week (0=Sunday, 6=Saturday)
 * @param Lunar   农历日期结构体指针 | Pointer to lunar date structure
 * @param festival 输出缓冲区，用于存储节日名称 | Output buffer for festival name
 * @return true 找到节日 | Festival found
 * @return false 无节日 | No festival
 *
 * 查询优先级(Priority order)：
 * 1. 农历节日(Lunar festivals)
 * 2. 特殊计算的节日：除夕、母亲节、父亲节、感恩节 | Special calculated festivals
 * 3. 公历节日(Gregorian festivals)
 * 4. 二十四节气(24 Solar Terms)
 */
static bool GetFestival(uint16_t year, uint8_t mon, uint8_t day, uint8_t week, struct Lunar_Date* Lunar,
                        char* festival) {
    // 农历节日
    // Lunar calendar festivals
    for (uint8_t i = 0; i < ARRAY_SIZE(festivals_lunar); i++) {
        if (Lunar->Month == festivals_lunar[i].month && Lunar->Date == festivals_lunar[i].day) {
            strcpy(festival, festivals_lunar[i].name);
            return true;
        }
    }

    // 除夕：春节前一天（12/29 或 12/30），12/30 已在上面判断
    // New Year's Eve: day before Spring Festival (lunar 12/29 or 12/30)
    // 需要判断次日是否为正月初一，因为农历大小月不固定
    // Must check if next day is lunar 1/1, as lunar months vary in length
    if (Lunar->Month == 12 && Lunar->Date == 29) {
        struct Lunar_Date nextLunar;
        struct devtm tm = {year, mon, day, 0, 0, 0, week};
        // 计算次日的农历日期 | Calculate next day's lunar date
        transformTime(transformTimeStruct(&tm) + 86400, &tm);  // +86400秒(1天) | +86400s (1 day)
        LUNAR_SolarToLunar(&nextLunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
        if (nextLunar.Month == 1 && nextLunar.Date == 1) {
            strcpy(festival, "除夕");
            return true;
        }
    }
    // 母亲节: 五月第二个星期日
    // Mother's Day: 2nd Sunday of May
    if (mon == 5 && week == 0 && day >= 8 && day <= 14) {
        strcpy(festival, "母亲节");
        return true;
    }
    // 父亲节: 六月第三个星期日
    // Father's Day: 3rd Sunday of June
    if (mon == 6 && week == 0 && day >= 15 && day <= 21) {
        strcpy(festival, "父亲节");
        return true;
    }
    // 感恩节：十一月第四个星期四
    // Thanksgiving: 4th Thursday of November
    if (mon == 11 && week == 4 && day >= 22 && day <= 28) {
        strcpy(festival, "感恩节");
        return true;
    }

    // 公历节日
    // Gregorian calendar festivals
    for (uint8_t i = 0; i < ARRAY_SIZE(festivals); i++) {
        if (mon == festivals[i].month && day == festivals[i].day) {
            strcpy(festival, festivals[i].name);
            return true;
        }
    }

    // 二十四节气
    // 24 Solar Terms
    // 节气是中国传统历法的重要组成部分，用于指导农业生产
    // Solar terms are important components of Chinese traditional calendar for agricultural guidance
    uint8_t JQdate;
    if (GetJieQi(year, mon, day, &JQdate) && JQdate == day) {
        // 计算节气索引：每月2个节气，day>=15则为第二个节气
        // Calculate solar term index: 2 per month, if day>=15 then it's the 2nd one
        uint8_t JQ = (mon - 1) * 2;
        if (day >= 15) JQ++;
        strcpy(festival, JieQiStr[JQ]);
        if (JQ == 6)  // 清明
            // 清明既是节气又是节日，需要加"节"字 | Qingming is both solar term and festival
            strcat(festival, "节");

        return true;
    }

    return false;
}

/**
 * @brief 绘制时间同步提示框
 *        Draw time synchronization reminder box
 *
 * @param gfx  图形上下文指针 | Pointer to graphics context
 * @param data GUI数据指针 | Pointer to GUI data
 *
 * 用途：当系统时间未同步时(如2025年1月)，在屏幕中央显示提示框，
 *       引导用户访问网页进行时间同步
 * Purpose: When system time is not synced (e.g., Jan 2025), display a reminder box
 *          in screen center to guide users to web page for time synchronization
 */
static void DrawTimeSyncTip(Adafruit_GFX* gfx, gui_data_t* data) {
    const char* title = "SYNC TIME!";
    const char* url = "https://ycd12.github.io/EPD-nRF5_DYC/";

    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);

    // 计算提示框尺寸和位置 | Calculate box size and position
    int16_t fh = GFX_getFontHeight(gfx);
    int16_t box_w = GFX_getUTF8Width(gfx, url) + 20;  // URL宽度+边距 | URL width + padding
    int16_t box_h = fh * 2 + 20;                       // 两行文字+边距 | 2 lines + padding
    int16_t box_x = (data->width - box_w) / 2;         // 水平居中 | Horizontally centered
    int16_t box_y = data->height / 2 - box_h / 2;      // 垂直居中 | Vertically centered

    // 绘制白色背景框 | Draw white background box
    GFX_fillRect(gfx, box_x, box_y, box_w, box_h, GFX_WHITE);
    // 绘制圆角边框 | Draw rounded border
    GFX_drawRoundRect(gfx, box_x, box_y, box_w, box_h, 5, GFX_BLACK);
    // 绘制红色标题 | Draw red title
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_setCursor(gfx, box_x + (box_w - GFX_getUTF8Width(gfx, title)) / 2, box_y + 5 + fh);
    GFX_printf(gfx, title);
    // 绘制黑色URL | Draw black URL
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, box_x + 10, box_y + box_h - GFX_getFontAscent(gfx));
    GFX_printf(gfx, url);
}

/**
 * @brief 绘制低电量提示页
 *        Draw low battery warning page
 *
 * @param gfx  图形上下文指针 | Pointer to graphics context
 * @param data GUI数据指针 | Pointer to GUI data
 */
static void DrawLowBatteryTip(Adafruit_GFX* gfx, gui_data_t* data) {
    char voltage[12] = {0};
    const uint8_t* voltage_font = (data->height >= 300) ? u8g2_font_helvB18_tn : u8g2_font_helvB14_tn;
    const char* power_off_text = "power off";

    uint16_t volts = data->voltage_mv / 1000;
    uint16_t decivolts = (data->voltage_mv % 1000) / 100;
    snprintf(voltage, sizeof(voltage), "%d.%dV", volts, decivolts);

    int16_t body_w = data->width / 3;
    int16_t body_h = data->height / 6;
    if (body_w > 110) body_w = 110;
    if (body_h > 55) body_h = 55;
    if (body_w < 72) body_w = 72;
    if (body_h < 34) body_h = 34;

    int16_t terminal_w = body_w / 12;
    int16_t terminal_h = body_h / 3;
    int16_t body_x = (data->width - (body_w + terminal_w + 6)) / 2;
    int16_t body_y = (data->height - body_h) / 2 - body_h / 6;
    if (body_y < 10) body_y = 10;
    int16_t fill_margin = 5;
    int16_t inner_w = body_w - fill_margin * 2;
    int16_t inner_h = body_h - fill_margin * 2;
    int16_t fill_w = inner_w / 6;
    if (fill_w < 6) fill_w = 6;

    GFX_drawRoundRect(gfx, body_x, body_y, body_w, body_h, 8, GFX_BLACK);
    GFX_fillRoundRect(gfx, body_x + body_w + 4, body_y + (body_h - terminal_h) / 2, terminal_w, terminal_h, 3, GFX_BLACK);
    GFX_fillRect(gfx, body_x + fill_margin, body_y + fill_margin, fill_w, inner_h, GFX_BLACK);

    GFX_setFont(gfx, voltage_font);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    int16_t voltage_w = GFX_getUTF8Width(gfx, voltage);
    int16_t voltage_h = GFX_getFontHeight(gfx);
    GFX_setCursor(gfx,
                  body_x + (body_w - voltage_w) / 2,
                  body_y + (body_h + voltage_h) / 2 - 2);
    GFX_printf(gfx, "%s", voltage);

    if (data->voltage_mv < CRITICAL_BATTERY_SHUTDOWN_MV) {
        GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
        int16_t text_w = GFX_getUTF8Width(gfx, power_off_text);
        int16_t text_h = GFX_getFontHeight(gfx);
        GFX_setCursor(gfx,
                      body_x + (body_w - text_w) / 2,
                      body_y + body_h + text_h + 10);
        GFX_printf(gfx, "%s", power_off_text);
    }
}

/**
 * @brief 绘制电池图标和电压显示
 *        Draw battery icon and voltage display
 *
 * @param gfx        图形上下文指针 | Pointer to graphics context
 * @param x          右边界X坐标 | Right edge X coordinate
 * @param y          Y坐标 | Y coordinate
 * @param iw         图标宽度 | Icon width
 * @param voltage_mv 电压(毫伏) | Voltage in millivolts
 *
 * 电池百分比显示：满电=3.3V，空电=2.8V
 * Battery percentage display: Full=3.3V, Empty=2.8V
 */
static void DrawBattery(Adafruit_GFX* gfx, int16_t x, int16_t y, uint8_t iw, uint16_t voltage_mv) {
    x -= iw;  // 从右边界开始向左绘制 | Start drawing from right edge to left
    uint8_t fill_width = (iw > 4) ? (uint8_t)(iw - 4) : 0;
    uint8_t level = 0;
    if (voltage_mv >= BATTERY_FULL_MV) {
        level = 100;
    } else if (voltage_mv > LOW_BATTERY_THRESHOLD_MV) {
        level = (uint8_t)(((uint32_t)(voltage_mv - LOW_BATTERY_THRESHOLD_MV) * 100) /
                          (BATTERY_FULL_MV - LOW_BATTERY_THRESHOLD_MV));
    }

    // Format voltage as "X.XV" directly (e.g., "3.3V")
    // 直接格式化电压为"X.XV"格式(如"3.3V")
    uint16_t volts = voltage_mv / 1000;           // 整数部分(伏特) | Integer part (volts)
    uint16_t millivolts = (voltage_mv % 1000) / 100;  // 小数部分(0.1V) | Decimal part

    // 绘制电压文字 | Draw voltage text
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_setCursor(gfx, x - GFX_getUTF8Width(gfx, "3.2V") - 2, y + 9);
    GFX_printf(gfx, "%d.%dV", volts, millivolts);
    // 绘制电池外框 | Draw battery outline
    GFX_fillRect(gfx, x, y, iw, 10, GFX_WHITE);   // 白色填充 | White fill
    GFX_drawRect(gfx, x, y, iw, 10, GFX_BLACK);   // 黑色边框 | Black border
    GFX_fillRect(gfx, x + iw, y + 4, 2, 2, GFX_BLACK);  // 电池正极凸起 | Battery positive terminal
    // 根据电量绘制填充 | Draw fill based on battery level
    GFX_fillRect(gfx, x + 2, y + 2, (uint16_t)(fill_width * level / 100), 6, GFX_BLACK);
}

/**
 * @brief Calculate ISO 8601 week number using pure integer arithmetic.
 *        使用纯整数算法计算ISO 8601周数
 *
 * @param year  Year since 1900 (e.g., tm_year from tm_t) | 自1900年起的年份
 * @param mon   Month (0-11) | 月份(0-11)
 * @param mday  Day of month (1-31) | 日期(1-31)
 * @param wday  Day of week (0=Sunday, 6=Saturday) | 星期几(0=周日, 6=周六)
 * @return      Week number (1-53) | 周数(1-53)
 *
 * ISO 8601 rules:
 * - Week 1 is the week containing the first Thursday of the year
 * - Monday is the first day of the week
 *
 * ISO 8601 规则说明：
 * - 第1周是包含当年第一个星期四的那周
 * - 星期一是一周的第一天(不是周日)
 * - 12月底的几天可能属于下一年的第1周
 * - 1月初的几天可能属于上一年的第52或53周
 *
 * 算法原理：
 * 找到当前日期所在周的星期四，根据该星期四在年中的位置计算周数
 * 因为ISO 8601规定包含1月4日的那周是第1周，而1月4日一定在第1周的星期四
 */
static uint8_t GetWeekOfYear(uint8_t year, uint8_t mon, uint8_t mday, uint8_t wday) {
    // Convert parameters to actual year and 1-based month
    // 将参数转换为实际年份和1基月份
    uint16_t actual_year = (uint16_t)year + YEAR0;
    uint8_t month = mon + 1;  // Convert 0-11 to 1-12 | 将0-11转换为1-12

    // Days in each month (non-leap year)
    // 每月天数(非闰年)
    static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Check if leap year
    // 检查是否为闰年：能被4整除且不能被100整除，或能被400整除
    uint8_t is_leap = ((actual_year % 4 == 0) && (actual_year % 100 != 0)) || (actual_year % 400 == 0);

    // Calculate day of year (1-366)
    // 计算当前是一年中的第几天(1-366)
    uint16_t day_of_year = mday;
    for (uint8_t i = 0; i < month - 1; i++) {
        day_of_year += days_in_month[i];
        if (i == 1 && is_leap) {  // February in leap year | 闰年2月
            day_of_year += 1;
        }
    }

    // Convert Sunday=0 to Monday=0 system for ISO week calculation
    // 将"周日=0"系统转换为"周一=0"系统用于ISO周计算
    // wday: 0=Sun, 1=Mon, ..., 6=Sat -> iso_wday: 0=Mon, 1=Tue, ..., 6=Sun
    uint8_t iso_wday = (wday + 6) % 7;

    // Calculate the week number using ISO 8601 algorithm
    // Find the Thursday of the current week (Thursday determines the week number)
    // Days from Monday: iso_wday, Days to Thursday: 3 - iso_wday
    // 使用ISO 8601算法计算周数
    // 找到当前周的星期四(星期四决定周数)
    // 距周一的天数: iso_wday，距星期四的天数: 3 - iso_wday
    int16_t thursday_day_of_year = (int16_t)day_of_year + (3 - (int16_t)iso_wday);

    // Handle edge cases for year boundaries
    // 处理跨年边界情况
    if (thursday_day_of_year < 1) {
        // This week belongs to the previous year
        // Calculate the last week of previous year
        // 当前周属于上一年，需要计算上一年的最后一周
        uint16_t prev_year = actual_year - 1;
        uint8_t prev_is_leap = ((prev_year % 4 == 0) && (prev_year % 100 != 0)) || (prev_year % 400 == 0);
        uint16_t prev_year_days = prev_is_leap ? 366 : 365;
        thursday_day_of_year += prev_year_days;
        // Week number of previous year's last week (52 or 53)
        // 返回上一年最后一周的周数(52或53)
        return (thursday_day_of_year - 1) / 7 + 1;
    }

    uint16_t year_days = is_leap ? 366 : 365;
    if (thursday_day_of_year > year_days) {
        // This week belongs to the next year - it's week 1
        // 当前周属于下一年，返回第1周
        return 1;
    }

    // Normal case: calculate week number
    // 正常情况：计算周数
    return (thursday_day_of_year - 1) / 7 + 1;
}

/**
 * @brief 绘制日期头部信息(用于MODE_CALENDAR模式)
 *        Draw date header information (for MODE_CALENDAR mode)
 *
 * @param gfx   图形上下文 | Graphics context
 * @param x     起始X坐标 | Start X coordinate
 * @param y     起始Y坐标 | Start Y coordinate
 * @param tm    公历时间结构体 | Gregorian time structure
 * @param Lunar 农历日期结构体 | Lunar date structure
 * @param data  GUI数据 | GUI data
 *
 * 显示内容(Display content)：
 * - 第1行：年月(红色数字+黑色汉字) + 农历日期 + ISO周数
 * - 第2行：农历干支年 + 生肖
 * - 右上角：电池图标+电压 + WiFi名称
 */
static void DrawDateHeader(Adafruit_GFX* gfx, int16_t x, int16_t y, tm_t* tm, struct Lunar_Date* Lunar,
                           gui_data_t* data) {
    // 绘制公历年月(大字体) | Draw Gregorian year and month (large font)
    GFX_setCursor(gfx, x, y - 2);
    GFX_printf_styled(gfx, GFX_RED, GFX_WHITE, u8g2_font_helvB18_tn, "%d", tm->tm_year + YEAR0);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "年");
    GFX_printf_styled(gfx, GFX_RED, GFX_WHITE, u8g2_font_helvB18_tn, "%d", tm->tm_mon + 1);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "月");

    int16_t tx = gfx->tx;  // 记录当前文字位置 | Save current text position
    int16_t ty = y;

    // 绘制农历日期和周数 | Draw lunar date and week number
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_setCursor(gfx, tx, ty);
    if (Lunar->IsLeap) GFX_printf(gfx, " ");  // 闰月前加空格对齐 | Add space for leap month alignment
    GFX_printf(gfx, "%s%s%s", Lunar_MonthLeapString[Lunar->IsLeap], Lunar_MonthString[Lunar->Month],
               Lunar_DateString[Lunar->Date]);
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_printf(gfx, " [%d周]", GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday));

    // 绘制农历干支年和生肖 | Draw lunar stem-branch year and zodiac
    GFX_setCursor(gfx, tx, ty - 14);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_printf(gfx, " %s%s年", Lunar_StemStrig[LUNAR_GetStem(Lunar)], Lunar_BranchStrig[LUNAR_GetBranch(Lunar)]);
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_printf(gfx, " [%s]", Lunar_ZodiacString[LUNAR_GetZodiac(Lunar)]);

    // 绘制右侧信息：电池和WiFi名称 | Draw right side info: battery and WiFi name
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    DrawBattery(gfx, data->width - 10 - 2, data->height > 300 ? 16 : 6, 20, data->voltage_mv);
    GFX_setCursor(gfx, data->width - GFX_getUTF8Width(gfx, data->ssid) - 10, y);
    GFX_printf(gfx, "%s", data->ssid);
}

/**
 * @brief 绘制星期头部栏
 *        Draw week header bar
 *
 * @param gfx  图形上下文 | Graphics context
 * @param x    起始X坐标 | Start X coordinate
 * @param y    起始Y坐标 | Start Y coordinate
 * @param data GUI数据(包含一周起始日设置) | GUI data (contains week start setting)
 *
 * 显示7个星期标签，周末(周六/周日)用红色背景显示
 * Displays 7 weekday labels, weekends (Sat/Sun) shown with red background
 */
static void DrawWeekHeader(Adafruit_GFX* gfx, int16_t x, int16_t y, gui_data_t* data) {
    GFX_setFont(gfx, data->height > 300 ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
    uint8_t w = (data->width - 2 * x) / 7;  // 每列宽度 | Width per column
    uint8_t h = data->height > 300 ? 32 : 24;  // 行高 | Row height
    uint8_t r = (data->width - 2 * x) % 7;     // 剩余像素分配给最后一列 | Remaining pixels for last column
    uint8_t fh = (h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;  // 垂直居中 | Vertical center
    int16_t cw = GFX_getUTF8Width(gfx, Lunar_DayString[0]);
    for (int i = 0; i < 7; i++) {
        // 根据一周起始日设置调整显示顺序 | Adjust display order based on week start setting
        uint8_t day = (data->week_start + i) % 7;
        uint16_t bg = (day == 0 || day == 6) ? GFX_RED : GFX_BLACK;  // 周末用红色 | Weekends in red
        GFX_fillRect(gfx, x + i * w, y, i == 6 ? (w + r) : w, h, bg);
        GFX_setTextColor(gfx, GFX_WHITE, bg);
        GFX_setCursor(gfx, x + (w - cw) / 2 + i * w, y + fh);
        GFX_printf(gfx, "%s", Lunar_DayString[day]);
    }
}

/**
 * @brief 绘制月份日历的所有日期单元格
 *        Draw all date cells for the monthly calendar
 *
 * @param gfx   图形上下文 | Graphics context
 * @param x     起始X坐标 | Start X coordinate
 * @param y     起始Y坐标 | Start Y coordinate
 * @param tm    公历时间结构体 | Gregorian time structure
 * @param Lunar 农历日期结构体(会被函数修改) | Lunar date structure (will be modified)
 * @param data  GUI数据 | GUI data
 *
 * 功能说明(Features)：
 * - 计算月历行数和单元格尺寸 | Calculate calendar rows and cell sizes
 * - 绘制网格分隔线(大屏幕) | Draw grid lines (large screen)
 * - 为每一天绘制：日期数字、农历/节日、休假标记
 * - For each day draws: date number, lunar date/festival, holiday marker
 * - 当天用红色圆圈高亮显示 | Current day highlighted with red circle
 * - 周末日期用红色显示 | Weekends shown in red
 */
static void DrawMonthDays(Adafruit_GFX* gfx, int16_t x, int16_t y, tm_t* tm, struct Lunar_Date* Lunar,
                          gui_data_t* data) {
    // 获取本月1号是星期几 | Get which day of week is the 1st day of month
    uint8_t firstDayWeek = get_first_day_week(tm->tm_year + YEAR0, tm->tm_mon + 1);
    // 根据用户设置的一周起始日调整首日位置 | Adjust first day position based on user's week start setting
    int8_t adjustedFirstDay = (firstDayWeek - data->week_start + 7) % 7;
    // 获取本月天数 | Get number of days in this month
    uint8_t monthMaxDays = thisMonthMaxDays(tm->tm_year + YEAR0, tm->tm_mon + 1);
    // 计算需要的行数 | Calculate required rows
    // 公式：1 + (总天数 - 第一周剩余天数 + 6) / 7
    uint8_t monthDayRows = 1 + (monthMaxDays - (7 - adjustedFirstDay) + 6) / 7;

    // 计算每个单元格的宽度和高度 | Calculate cell width and height
    int16_t bw = (data->width - x - 10) / 7;
    int16_t bh = (data->height - y - 10) / monthDayRows;
    bool large = data->height > 300;

    // 大屏幕绘制网格分隔线(虚线) | Draw grid lines (dotted) for large screens
    if (large) {
        // 水平线 | Horizontal lines
        for (uint8_t i = 1; i < monthDayRows; i++)
            GFX_drawDottedLine(gfx, x, y + i * bh, x + 7 * bw - 1, y + i * bh, GFX_BLACK, 1, 5);
        // 垂直线 | Vertical lines
        for (uint8_t i = 1; i < 7; i++)
            GFX_drawDottedLine(gfx, x + i * bw, y, x + i * bw, y + monthDayRows * bh - 1, GFX_BLACK, 1, 5);
    }

    // 遍历绘制每一天 | Iterate through each day of the month
    for (uint8_t i = 0; i < monthMaxDays; i++) {
        uint16_t year = tm->tm_year + YEAR0;
        uint8_t month = tm->tm_mon + 1;
        uint8_t day = i + 1;

        // actualWeek: 实际星期几(0=周日) | Actual day of week (0=Sunday)
        int16_t actualWeek = (firstDayWeek + i) % 7;
        // displayWeek: 根据用户设置调整后的显示列 | Display column after user setting adjustment
        int16_t displayWeek = (adjustedFirstDay + i) % 7;
        bool weekend = (actualWeek == 0) || (actualWeek == 6);

        // 计算该天的农历日期 | Calculate lunar date for this day
        LUNAR_SolarToLunar(Lunar, year, month, day);

        // 计算日期圆圈的半径和位置 | Calculate circle radius and position for date
        int16_t cr = large ? 13 : 10;  // 圆圈半径 | Circle radius
        int16_t bx = x + (bw - 2 * cr) / 2 + displayWeek * bw;  // 单元格内居中X | Cell-centered X
        int16_t by = y + (bh - 2 * cr) / 2 + (i + adjustedFirstDay) / 7 * bh + 3;  // Y坐标 | Y coordinate

        // 当天用红色圆圈高亮 | Highlight current day with red circle
        if (day == tm->tm_mday) {
            GFX_fillCircle(gfx, bx + cr, by + cr - 3, 2 * cr, GFX_RED);
            GFX_setTextColor(gfx, GFX_WHITE, GFX_RED);
        } else {
            // 周末用红色文字 | Weekends in red text
            GFX_setTextColor(gfx, weekend ? GFX_RED : GFX_BLACK, GFX_WHITE);
        }

        // 绘制日期数字 | Draw date number
        GFX_setFont(gfx, large ? u8g2_font_helvB18_tn : u8g2_font_helvB14_tn);
        GFX_setCursor(gfx, bx + (2 * cr - GFX_getUTF8Widthf(gfx, "%d", day)) / 2, by - (cr - GFX_getFontHeight(gfx)));
        GFX_printf(gfx, "%d", day);

        // 准备节日/农历信息 | Prepare festival/lunar information
        char festival_buf[10] = {0};
        const char* festival;
        GFX_setFont(gfx, large ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
        GFX_setFontMode(gfx, 1);  // transparent | 透明模式
        if (GetFestival(year, month, day, actualWeek, Lunar, festival_buf)) {
            // 有节日：显示节日名称 | Has festival: show festival name
            festival = festival_buf;
            if (day != tm->tm_mday) GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);  // 节日用红色 | Festivals in red
        } else {
            if (Lunar->Date == 1) {
                // 农历初一：显示月份 | Lunar 1st day: show month name
                // snprintf优化：仅在需要合成字符串时使用 | snprintf optimization: only use when composing strings
                snprintf(festival_buf, sizeof(festival_buf), "%s%s", Lunar_MonthLeapString[Lunar->IsLeap],
                         Lunar_MonthString[Lunar->Month]);
                festival = festival_buf;
            } else {
                // 普通日子：直接使用指针，避免复制 | Normal day: use direct pointer, avoid copying
                festival = Lunar_DateString[Lunar->Date];  // Direct pointer, no copy
            }
        }
        // 绘制节日/农历文字 | Draw festival/lunar text
        GFX_setCursor(gfx, bx + (2 * cr - GFX_getUTF8Width(gfx, festival)) / 2, gfx->ty + GFX_getFontHeight(gfx) + 3);
        GFX_printf(gfx, "%s", festival);

        // 绘制放假/调休标记 | Draw holiday/workday marker
        bool work = false;
        if (year == HOLIDAY_YEAR && GetHoliday(month, day, &work)) {
            // 如果是当天，在红色圆圈内绘制白底红圈标记 | If current day, draw white-filled red circle marker
            if (day == tm->tm_mday) {
                uint16_t rx = bx + (large ? 36 : 27);
                uint16_t ry = by - 2;
                uint8_t cr = large ? 10 : 8;
                GFX_fillCircle(gfx, rx, ry, cr, GFX_WHITE);
                GFX_drawCircle(gfx, rx, ry, cr, GFX_RED);
            }
            GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
            // 调休上班用黑色，休假用红色 | Workday in black, holiday in red
            GFX_setTextColor(gfx, work ? GFX_BLACK : GFX_RED, GFX_WHITE);
            GFX_setCursor(gfx, bx + (large ? 31 : 22), by + 3);
            GFX_printf(gfx, "%s", work ? "班" : "休");
        }
    }
}

/**
 * @brief 绘制完整日历界面(MODE_CALENDAR模式)
 *        Draw complete calendar interface (MODE_CALENDAR mode)
 *
 * @param gfx   图形上下文 | Graphics context
 * @param tm    公历时间 | Gregorian time
 * @param Lunar 农历日期 | Lunar date
 * @param data  GUI数据 | GUI data
 *
 * 组合调用三个子函数绘制完整月历
 * Combines three sub-functions to draw complete monthly calendar
 */
static void DrawCalendar(Adafruit_GFX* gfx, tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data) {
    bool large = data->height > 300;
    DrawDateHeader(gfx, 10, large ? 38 : 28, tm, Lunar, data);  // 绘制头部 | Draw header
    DrawWeekHeader(gfx, 10, large ? 44 : 32, data);             // 绘制星期栏 | Draw week header
    DrawMonthDays(gfx, 10, large ? 84 : 64, tm, Lunar, data);   // 绘制日历主体 | Draw calendar body
}

// clang-format off
/* Routine to Draw Large 7-Segment formated number
   Contributed by William Zaggle.
   7段数码管样式数字绘制函数
   由 William Zaggle 贡献

   参数说明(Parameters):
   int n - The number to be displayed | 要显示的数字
   int xLoc = The x location of the upper left corner of the number | 数字左上角的X坐标
   int yLoc = The y location of the upper left corner of the number | 数字左上角的Y坐标
   int cS = The size of the number. | 数字的缩放因子
   fC is the foreground color of the number | 数字的前景色
   bC is the background color of the number (prevents having to clear previous space) | 背景色(避免需要清除之前的空间)
   nD is the number of digit spaces to occupy (must include space for minus sign for numbers < 0).
      数字占用的位数(负数需包含减号的空间)

   尺寸计算(Size calculation):
   width: nD*(11*cS+2)-2*cS
   height: 20*cS+4

   https://forum.arduino.cc/t/fast-7-segment-number-display-for-tft/296619/4
*/
static void Draw7Number(Adafruit_GFX *gfx, int n, unsigned int xLoc, unsigned int yLoc, char cS, unsigned int fC, unsigned int bC, int nD) {
    unsigned int num=abs(n),i,t,w,col,h,a,b,j=1,d=0,S2=5*cS,S3=2*cS,S4=7*cS,x1=cS+1,x2=S3+S2+1,y1=yLoc+x1,y3=yLoc+S3+S4+1;
    unsigned int seg[7][3]={{x1,yLoc,1},{x2,y1,0},{x2,y3+x1,0},{x1,(2*y3)-yLoc,1},{0,y3+x1,0},{0,y1,0},{x1,y3,1}};
    unsigned char nums[12]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F,0x00,0x40},c=(c=abs(cS))>10?10:(c<1)?1:c,cnt=(cnt=abs(nD))>10?10:(cnt<1)?1:cnt;
    for (xLoc+=cnt*(d=S2+(3*S3)+2);cnt>0;cnt--){
      for (i=(num>9)?num%10:((!cnt)&&(n<0))?11:((nD<0)&&(!num))?10:num,xLoc-=d,num/=10,j=0;j<7;++j){
        col=(nums[i]&(1<<j))?fC:bC;
        if (seg[j][2])for(w=S2,t=seg[j][1]+S3,h=seg[j][1]+cS,a=xLoc+seg[j][0]+cS,b=seg[j][1];b<h;b++,a--,w+=2)GFX_drawFastHLine(gfx,a,b,w,col);
        else for(w=S4,t=xLoc+seg[j][0]+S3,h=xLoc+seg[j][0]+cS,b=xLoc+seg[j][0],a=seg[j][1]+cS;b<h;b++,a--,w+=2)GFX_drawFastVLine(gfx,b,a,w,col);
        for (;b<t;b++,a++,w-=2)seg[j][2]?GFX_drawFastHLine(gfx,a,b,w,col):GFX_drawFastVLine(gfx,b,a,w,col);
        }
    }
}
// clang-format on

/**
 * @brief 绘制时钟时间显示(HH:MM格式，7段数码管样式)
 *        Draw clock time display (HH:MM format, 7-segment style)
 *
 * @param gfx 图形上下文 | Graphics context
 * @param tm  时间结构体 | Time structure
 * @param x   起始X坐标 | Start X coordinate
 * @param y   起始Y坐标 | Start Y coordinate
 * @param cS  缩放因子 | Scale factor
 * @param nD  每个数字的位数(通常为2) | Digits per number (usually 2)
 */
static void DrawTime(Adafruit_GFX* gfx, tm_t* tm, int16_t x, int16_t y, uint16_t cS, uint16_t nD) {
    // 绘制小时 | Draw hours
    Draw7Number(gfx, tm->tm_hour, x, y, cS, GFX_BLACK, GFX_WHITE, nD);
    x += (nD * (11 * cS + 2) - 2 * cS) + 2 * cS;
    // 绘制冒号(两个方块) | Draw colon (two squares)
    GFX_fillRect(gfx, x, y + 4.5 * cS + 1, 2 * cS, 2 * cS, GFX_BLACK);
    GFX_fillRect(gfx, x, y + 13.5 * cS + 3, 2 * cS, 2 * cS, GFX_BLACK);
    x += 4 * cS;
    // 绘制分钟 | Draw minutes
    Draw7Number(gfx, tm->tm_min, x, y, cS, GFX_BLACK, GFX_WHITE, nD);
}

// Split screen layout constants for 800x480 screen
// 800x480屏幕的分屏布局常量
// Left side: 392 pixels (must be multiple of 8 for byte alignment)
// 左侧：392像素(必须是8的倍数以便字节对齐)
// Right side: 408 pixels
// 右侧：408像素
#define SPLIT_LEFT_WIDTH 392
#define SPLIT_RIGHT_START 392

  // Time-only partial refresh area (for MODE_CLOCK_CALENDAR)
  // Width is 384 (not 392) to leave space for the separator line at x=390
  // This prevents the partial refresh from affecting the separator line
  // Y position and height adjusted for layout with 60px top/bottom margins
  // Content area: y=60 to y=420 (360px height)
  // Time starts at y=182 (calculated: 128 + (360-128-124)/2), height=124, ends at y=306
  // 时钟局部刷新区域(用于MODE_CLOCK_CALENDAR模式)
  // 宽度是384(不是392)，为x=390处的分隔线留出空间
  // 这样可以防止局部刷新影响分隔线
  // Y位置和高度根据60px上下边距的布局进行调整
  // 内容区域：y=60到y=420 (高度360px)
  // 时间从y=182开始(计算：128 + (360-128-124)/2)，高度=124，结束于y=306
  #define TIME_REFRESH_X      0    // Start from left edge | 从左边缘开始
  #define TIME_REFRESH_Y      169  // Adjusted so time top aligns at y=182 | 调整使时间顶部对齐到y=182
  #define TIME_REFRESH_WIDTH  384  // Leave 8px margin for separator line (at x=390) | 为分隔线留8px边距
  #define TIME_REFRESH_HEIGHT 150  // Time display height (124 + 26px margin) | 时间显示高度(124+26px边距)

// State for dynamic time refresh window (used by partial refresh)
// These track the last chosen sub-window inside the TIME_REFRESH area.
// 动态时间刷新窗口的状态(用于局部刷新)
// 这些变量追踪TIME_REFRESH区域内最后选择的子窗口
static uint32_t s_last_time_refresh_ts = 0;               // 上次刷新的时间戳 | Last refresh timestamp
static uint16_t s_time_window_x = 0;                      // local X inside TIME_REFRESH area | TIME_REFRESH区域内的本地X坐标
static uint16_t s_time_window_y = 0;                      // local Y inside TIME_REFRESH area | TIME_REFRESH区域内的本地Y坐标
static uint16_t s_time_window_w = TIME_REFRESH_WIDTH;    // width of current refresh window | 当前刷新窗口的宽度
static uint16_t s_time_window_h = TIME_REFRESH_HEIGHT;   // height of current refresh window | 当前刷新窗口的高度

// Draw calendar on the right side of the split screen (starting at x=SPLIT_RIGHT_START)
// 在分屏的右侧绘制日历(从x=SPLIT_RIGHT_START开始)
/**
 * @brief 在分屏右侧绘制日历界面
 *        Draw calendar interface on right side of split screen
 *
 * @param gfx   图形上下文 | Graphics context
 * @param tm    公历时间 | Gregorian time
 * @param Lunar 农历日期 | Lunar date
 * @param data  GUI数据 | GUI data
 *
 * 布局特点(Layout features)：
 * - 从x=392px开始，宽度408px | Starts at x=392px, width 408px
 * - 垂直分隔线连接时钟侧 | Vertical separator line connecting clock side
 * - 顶部显示WiFi名称和电池信息 | Top shows WiFi name and battery info
 * - 标准月历布局：星期头+日期网格 | Standard calendar layout: week header + date grid
 */
static void DrawCalendar_Right(Adafruit_GFX* gfx, tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data) {
    int16_t offset_x = SPLIT_RIGHT_START;  // 右侧起始X坐标 | Right side start X
    int16_t right_width = data->width - SPLIT_RIGHT_START;  // 右侧宽度 | Right side width
    bool large = data->height > 300;

    // Draw vertical separator line (shortened to match reserved bottom area)
    // Line from y=70 to y=420 (60px margin at top and bottom)
    // 绘制垂直分隔线(缩短以匹配保留的底部区域)
    // 线从y=70到y=420(顶部和底部各60px边距)
    int16_t separator_top = 70;
    int16_t separator_bottom = large ? 420 : data->height - 10;
    GFX_drawFastVLine(gfx, offset_x - 2, separator_top, separator_bottom - separator_top, GFX_BLACK);

    // Top header: BT name on left, battery+voltage on right (same line)
    // Use wqy9 font for both to ensure consistent alignment
    // 顶部头：左侧蓝牙名称，右侧电池+电压(同一行)
    // 两者都使用wqy9字体以确保一致对齐
    int16_t header_y = large ? 86 : 78;  // +60 for top margin | 顶部边距+60

    // Set text color before drawing battery (DrawBattery doesn't set its own color)
    // 在绘制电池前设置文字颜色(DrawBattery不设置自己的颜色)
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);

    // Battery icon + voltage on right side
    // DrawBattery uses wqy9 font, battery icon height=10, voltage text baseline at y+9
    // 右侧电池图标+电压
    // DrawBattery使用wqy9字体，电池图标高度=10，电压文字基线在y+9
    DrawBattery(gfx, data->width - 10 - 2, header_y - 9, 20, data->voltage_mv);

    // BT name on left side of calendar area (use wqy9 to match voltage font)
    // 日历区域左侧的蓝牙名称(使用wqy9以匹配电压字体)
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, offset_x + 8, header_y);
    GFX_printf(gfx, "%s", data->ssid);

    // Week header for right side - positioned below the header
    // 右侧的星期头 - 位于头部下方
    int16_t week_y = large ? 96 : 86;  // +60 for top margin | 顶部边距+60
    int16_t week_x = offset_x + 5;
    GFX_setFont(gfx, large ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
    uint8_t w = (right_width - 10) / 7;  // 每列宽度 | Width per column
    uint8_t h = large ? 32 : 24;         // 星期头高度 | Week header height
    uint8_t r = (right_width - 10) % 7;  // 剩余像素 | Remaining pixels
    uint8_t fh = (h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;
    int16_t cw = GFX_getUTF8Width(gfx, Lunar_DayString[0]);
    for (int i = 0; i < 7; i++) {
        uint8_t day = (data->week_start + i) % 7;
        uint16_t bg = (day == 0 || day == 6) ? GFX_RED : GFX_BLACK;
        GFX_fillRect(gfx, week_x + i * w, week_y, i == 6 ? (w + r) : w, h, bg);
        GFX_setTextColor(gfx, GFX_WHITE, bg);
        GFX_setCursor(gfx, week_x + (w - cw) / 2 + i * w, week_y + fh);
        GFX_printf(gfx, "%s", Lunar_DayString[day]);
    }

    // Month days for right side - positioned below week header
    int16_t days_x = offset_x + 5;
    int16_t days_y = large ? 136 : 116;  // +60 for top margin

    uint8_t firstDayWeek = get_first_day_week(tm->tm_year + YEAR0, tm->tm_mon + 1);
    int8_t adjustedFirstDay = (firstDayWeek - data->week_start + 7) % 7;
    uint8_t monthMaxDays = thisMonthMaxDays(tm->tm_year + YEAR0, tm->tm_mon + 1);
    uint8_t monthDayRows = 1 + (monthMaxDays - (7 - adjustedFirstDay) + 6) / 7;

    // Match clock side layout - leave bottom area reserved (y=420-480)
    // Content area: y=60 to y=420 (360px height with 60px top/bottom margins)
    int16_t calendar_bottom = large ? 420 : data->height - 10;  // Leave 60px at bottom
    int16_t bw = (right_width - 10) / 7;
    int16_t bh = (calendar_bottom - days_y) / monthDayRows;

    if (large) {
        for (uint8_t i = 1; i < monthDayRows; i++)
            GFX_drawDottedLine(gfx, days_x, days_y + i * bh, days_x + 7 * bw - 1, days_y + i * bh, GFX_BLACK, 1, 5);
        for (uint8_t i = 1; i < 7; i++)
            GFX_drawDottedLine(gfx, days_x + i * bw, days_y, days_x + i * bw, days_y + monthDayRows * bh - 1, GFX_BLACK, 1, 5);
    }

    struct Lunar_Date dayLunar;
    for (uint8_t i = 0; i < monthMaxDays; i++) {
        uint16_t year = tm->tm_year + YEAR0;
        uint8_t month = tm->tm_mon + 1;
        uint8_t day = i + 1;

        int16_t actualWeek = (firstDayWeek + i) % 7;
        int16_t displayWeek = (adjustedFirstDay + i) % 7;
        bool weekend = (actualWeek == 0) || (actualWeek == 6);

        LUNAR_SolarToLunar(&dayLunar, year, month, day);

        int16_t cr = large ? 13 : 10;
        int16_t bx = days_x + (bw - 2 * cr) / 2 + displayWeek * bw;
        int16_t by = days_y + (bh - 2 * cr) / 2 + (i + adjustedFirstDay) / 7 * bh + 3;

        if (day == tm->tm_mday) {
            GFX_fillCircle(gfx, bx + cr, by + cr - 3, 2 * cr, GFX_RED);
            GFX_setTextColor(gfx, GFX_WHITE, GFX_RED);
        } else {
            GFX_setTextColor(gfx, weekend ? GFX_RED : GFX_BLACK, GFX_WHITE);
        }

        GFX_setFont(gfx, large ? u8g2_font_helvB18_tn : u8g2_font_helvB14_tn);
        GFX_setCursor(gfx, bx + (2 * cr - GFX_getUTF8Widthf(gfx, "%d", day)) / 2, by - (cr - GFX_getFontHeight(gfx)));
        GFX_printf(gfx, "%d", day);

        char festival_buf[10] = {0};
        const char* festival;
        GFX_setFont(gfx, large ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
        GFX_setFontMode(gfx, 1);
        if (GetFestival(year, month, day, actualWeek, &dayLunar, festival_buf)) {
            festival = festival_buf;
            if (day != tm->tm_mday) GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
        } else {
            if (dayLunar.Date == 1) {
                snprintf(festival_buf, sizeof(festival_buf), "%s%s", Lunar_MonthLeapString[dayLunar.IsLeap],
                         Lunar_MonthString[dayLunar.Month]);
                festival = festival_buf;
            } else {
                festival = Lunar_DateString[dayLunar.Date];  // Direct pointer, no copy
            }
        }
        GFX_setCursor(gfx, bx + (2 * cr - GFX_getUTF8Width(gfx, festival)) / 2, gfx->ty + GFX_getFontHeight(gfx) + 3);
        GFX_printf(gfx, "%s", festival);

        bool work = false;
        if (year == HOLIDAY_YEAR && GetHoliday(month, day, &work)) {
            if (day == tm->tm_mday) {
                uint16_t rx = bx + (large ? 36 : 27);
                uint16_t ry = by - 2;
                uint8_t cr2 = large ? 10 : 8;
                GFX_fillCircle(gfx, rx, ry, cr2, GFX_WHITE);
                GFX_drawCircle(gfx, rx, ry, cr2, GFX_RED);
            }
            GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
            GFX_setTextColor(gfx, work ? GFX_BLACK : GFX_RED, GFX_WHITE);
            GFX_setCursor(gfx, bx + (large ? 31 : 22), by + 3);
            GFX_printf(gfx, "%s", work ? "班" : "休");
        }
    }
}

// Draw clock on the left side of the split screen (for partial refresh)
// width_limit: the width of the left area (should be SPLIT_LEFT_WIDTH)
// Layout: Top line at y=68 (aligned with calendar week header bottom)
//         Time display centered between top line and bottom line
//         Bottom line at y=300, bottom info below, leaving space for future content
// 在分屏的左侧绘制时钟(用于局部刷新)
// width_limit: 左侧区域的宽度(应该是SPLIT_LEFT_WIDTH)
// 布局：顶部线在y=68(与日历星期头底部对齐)
//       时间显示在顶部线和底部线之间居中
//       底部线在y=300，底部信息在下方，为未来内容留出空间
/**
 * @brief 在分屏左侧绘制时钟界面
 *        Draw clock interface on left side of split screen
 *
 * @param gfx         图形上下文 | Graphics context
 * @param tm          公历时间 | Gregorian time
 * @param Lunar       农历日期 | Lunar date
 * @param data        GUI数据 | GUI data
 * @param width_limit 左侧区域宽度 | Left area width
 *
 * 布局设计(Layout design)：
 * - 上下各60px边距 | 60px margin at top and bottom
 * - 顶部：年月日 + 农历日期 + 温度 | Top: YMD + lunar date + temperature
 * - 中部：大号7段数码管时间显示 | Middle: Large 7-segment time display
 * - 底部：干支年/生肖 + 周数 + 节气倒计时 | Bottom: Stem-branch year + week number + solar term countdown
 * - 只使用黑色(局部刷新不支持彩色) | Only use black (partial refresh doesn't support color)
 */
static void DrawClock_Left(Adafruit_GFX* gfx, tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data, uint16_t width_limit) {
    // Use black for partial refresh (no red support)
    // 局部刷新使用黑色(不支持红色)
    uint16_t text_color = GFX_BLACK;

    int16_t padding = 15;

    // Layout constants for compact design with 60px top/bottom margins
    // Content area: y=60 to y=420 (360px height)
    // 紧凑设计的布局常量，上下各60px边距
    // 内容区域：y=60到y=420 (高度360px)
    int16_t top_line_y = 128;      // Aligned with calendar week header bottom (+60) | 与日历星期头底部对齐
    int16_t bottom_line_y = 360;   // Moved up to leave space at bottom (+60) | 上移以在底部留空间
    int16_t bottom_info_y1 = 380;  // First line of bottom info (+60) | 底部信息第一行
    int16_t bottom_info_y2 = 404;  // Second line of bottom info (+60) | 底部信息第二行

    // === Line 1: Year/Month/Day with larger font ===
    // Use helvB18 for numbers and wqy12 for Chinese characters
    // === 第1行：年/月/日，使用大字体 ===
    // 数字使用helvB18，汉字使用wqy12
    GFX_setCursor(gfx, padding, 92);  // +60 for top margin | 顶部边距+60
    GFX_setTextColor(gfx, text_color, GFX_WHITE);
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_printf(gfx, "%d", tm->tm_year + YEAR0);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "年");
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_printf(gfx, "%d", tm->tm_mon + 1);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "月");
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_printf(gfx, "%d", tm->tm_mday);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "日");

    // === Line 2: Lunar date on left, Temperature on right ===
    // === 第2行：左侧农历日期，右侧温度 ===
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_setCursor(gfx, padding, 115);  // +60 for top margin | 顶部边距+60
    GFX_printf(gfx, "%s%s%s", Lunar_MonthLeapString[Lunar->IsLeap],
               Lunar_MonthString[Lunar->Month], Lunar_DateString[Lunar->Date]);

    // Temperature on right side with larger number font
    // 右侧温度，数字使用大字体
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    // Calculate temperature number width without snprintf
    // 计算温度数字宽度，不使用snprintf(优化性能)
    int8_t temp = data->temperature;
    int16_t digit_width = GFX_getUTF8Width(gfx, "0");
    int16_t temp_num_width = digit_width;  // At least one digit | 至少一位数字
    if (temp < 0) {
        temp_num_width += GFX_getUTF8Width(gfx, "-");  // 负号宽度 | Minus sign width
        temp = -temp;
    }
    if (temp >= 10) temp_num_width += digit_width;  // 十位 | Tens digit
    if (temp >= 100) temp_num_width += digit_width;  // 百位(不太可能) | Hundreds digit (unlikely)
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    int16_t degree_width = GFX_getUTF8Width(gfx, "℃");
    int16_t total_temp_width = temp_num_width + degree_width;

    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_setCursor(gfx, width_limit - padding - total_temp_width, 115);  // +60 for top margin | 顶部边距+60
    GFX_printf(gfx, "%d", data->temperature);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "℃");

    // Top horizontal line (aligned with calendar week header bottom)
    // 顶部水平线(与日历星期头底部对齐)
    GFX_drawFastHLine(gfx, padding, top_line_y, width_limit - 2 * padding, GFX_BLACK);

    // === Center: Time display ===
    // === 中部：时间显示 ===
    uint16_t cS = 6;  // Scale factor for 7-segment display | 7段数码管缩放因子
    uint16_t nD = 2;  // Number of digits | 数字位数
    uint16_t time_width = 2 * (nD * (11 * cS + 2) - 2 * cS) + 4 * cS;  // 272
    uint16_t time_height = 20 * cS + 4;  // 124

    // Center time between top line and bottom line
    // 在顶部线和底部线之间居中时间
    int16_t available_height = bottom_line_y - top_line_y;
    int16_t time_x = (TIME_REFRESH_WIDTH - time_width) / 2;
    int16_t time_y = top_line_y + (available_height - time_height) / 2;
    DrawTime(gfx, tm, time_x, time_y, cS, nD);

    // Bottom horizontal line (moved up)
    // 底部水平线(上移)
    GFX_drawFastHLine(gfx, padding, bottom_line_y, width_limit - 2 * padding, GFX_BLACK);

    // === Bottom section: Left side - Lunar year + Week number ===
    // Use wqy12 font for bottom text
    // === 底部区域：左侧 - 农历干支年 + 周数 ===
    // 底部文字使用wqy12字体
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_setTextColor(gfx, text_color, GFX_WHITE);

    // Left column: Lunar year on first line, Week number on second line
    // 左列：第一行农历干支年，第二行周数
    GFX_setCursor(gfx, padding, bottom_info_y1);
    GFX_printf(gfx, "%s%s年", Lunar_StemStrig[LUNAR_GetStem(Lunar)], Lunar_BranchStrig[LUNAR_GetBranch(Lunar)]);

    GFX_setCursor(gfx, padding, bottom_info_y2);
    GFX_printf(gfx, "%d周", GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday));

    // === Bottom section: Right side - Next solar term countdown ===
    // === 底部区域：右侧 - 下一个节气倒计时 ===
    uint8_t days_to_jieqi = 0;
    uint8_t next_jieqi = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &days_to_jieqi);

    if (days_to_jieqi == 0) {
        // Today is the solar term - show only solar term name
        // 今天就是节气日 - 只显示节气名称
        const char* jq_str = JieQiStr[next_jieqi % 24];
        GFX_setCursor(gfx, width_limit - padding - GFX_getUTF8Width(gfx, jq_str), bottom_info_y1);
        GFX_printf(gfx, "%s", jq_str);
    } else {
        // Right column: "离xx" on first line, "还有xx天" on second line
        // 右列：第一行"离xx"，第二行"还有xx天"
        const char* jq_name = JieQiStr[next_jieqi % 24];
        int16_t jq_width = GFX_getUTF8Width(gfx, "离") + GFX_getUTF8Width(gfx, jq_name);
        GFX_setCursor(gfx, width_limit - padding - jq_width, bottom_info_y1);
        GFX_printf(gfx, "离%s", jq_name);

        // Calculate width for "还有X天" - estimate digit width using "8" as reference
        // 计算"还有X天"的宽度 - 使用"8"作为参考估算数字宽度(避免snprintf)
        int16_t days_width = GFX_getUTF8Width(gfx, "还有") + GFX_getUTF8Width(gfx, "8") +
                             (days_to_jieqi >= 10 ? GFX_getUTF8Width(gfx, "8") : 0) + GFX_getUTF8Width(gfx, "天");
        GFX_setCursor(gfx, width_limit - padding - days_width, bottom_info_y2);
        GFX_printf(gfx, "还有%d天", days_to_jieqi);
    }

}

// Draw the full split screen with both clock and calendar (for initial full refresh)
// 绘制完整的分屏界面，包含时钟和日历(用于初始的全屏刷新)
/**
 * @brief 绘制完整的分屏时钟+日历界面(MODE_CLOCK_CALENDAR模式)
 *        Draw complete split screen clock + calendar interface (MODE_CLOCK_CALENDAR mode)
 *
 * @param gfx   图形上下文 | Graphics context
 * @param tm    公历时间 | Gregorian time
 * @param Lunar 农历日期 | Lunar date
 * @param data  GUI数据 | GUI data
 *
 * 布局(Layout)：
 * - 左侧(0-392px)：时钟显示，支持后续局部刷新 | Left (0-392px): Clock, supports partial refresh
 * - 右侧(392-800px)：月历显示，保持静态 | Right (392-800px): Calendar, remains static
 * - 中间有垂直分隔线 | Vertical separator line in middle
 * - 顶部和底部有装饰横线 | Decorative horizontal lines at top and bottom
 *
 * 该函数用于首次全屏刷新，之后时钟部分使用DrawGUI_ClockOnly进行局部刷新
 * This function is used for initial full refresh, then clock part uses DrawGUI_ClockOnly for partial refresh
 */
static void DrawClockCalendar_Full(Adafruit_GFX* gfx, tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data) {
    // Draw horizontal lines at top and bottom of screen (30 pixels from edges)
    // 在屏幕顶部和底部绘制水平线(距边缘30像素)
    GFX_fillRect(gfx, 15, 45, data->width - 30, 2, GFX_BLACK);                 // Top line (2px thick) | 顶部线(2px粗)
    GFX_fillRect(gfx, 15, data->height - 45, data->width - 30, 2, GFX_BLACK);  // Bottom line (2px thick) | 底部线(2px粗)

    // Draw calendar on the right side
    // 在右侧绘制日历
    DrawCalendar_Right(gfx, tm, Lunar, data);

    // Draw clock on the left side (full color for initial draw)
    // 在左侧绘制时钟(初次绘制使用全彩色)
    DrawClock_Left(gfx, tm, Lunar, data, SPLIT_LEFT_WIDTH);
}

/**
 * @brief 绘制全屏时钟界面(MODE_CLOCK模式)
 *        Draw full screen clock interface (MODE_CLOCK mode)
 *
 * @param gfx   图形上下文 | Graphics context
 * @param tm    公历时间 | Gregorian time
 * @param Lunar 农历日期 | Lunar date
 * @param data  GUI数据 | GUI data
 *
 * 注意：此模式也支持局部刷新，因此只使用黑色而不使用红色
 * Note: This mode also supports partial refresh, so only use black instead of red
 */
static void DrawClock(Adafruit_GFX* gfx, tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data) {
    // For partial refresh (clock mode), use black instead of red
    // because partial refresh only supports black/white
    // 为了支持局部刷新(时钟模式)，使用黑色代替红色
    // 因为局部刷新只支持黑白两色
    uint16_t text_color = GFX_BLACK;  // Changed from GFX_RED to GFX_BLACK | 从GFX_RED改为GFX_BLACK

    uint8_t padding = data->height > 300 ? 100 : 40;
    GFX_setCursor(gfx, padding, 36);
    GFX_printf_styled(gfx, text_color, GFX_WHITE, u8g2_font_helvB18_tn, "%d", tm->tm_year + YEAR0);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "年");
    GFX_printf_styled(gfx, text_color, GFX_WHITE, u8g2_font_helvB18_tn, "%02d", tm->tm_mon + 1);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "月");
    GFX_printf_styled(gfx, text_color, GFX_WHITE, u8g2_font_helvB18_tn, "%02d", tm->tm_mday);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "日 ");

    GFX_setCursor(gfx, padding, 58);
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_printf(gfx, "星期%s", Lunar_DayString[tm->tm_wday]);
    GFX_setCursor(gfx, 138, 58);
    GFX_printf(gfx, "%s%s%s", Lunar_MonthLeapString[Lunar->IsLeap], Lunar_MonthString[Lunar->Month],
               Lunar_DateString[Lunar->Date]);

    DrawBattery(gfx, data->width - padding, 25, 20, data->voltage_mv);

    char ssid[5] = {0};
    int16_t ssid_len = strlen(data->ssid);
    int16_t sw = GFX_getUTF8Width(gfx, "25℃[1234]");
    memcpy(ssid, &data->ssid[ssid_len - 4], 4);
    GFX_setCursor(gfx, data->width - padding - sw - 2, 58);
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_printf(gfx, "%d℃[%s]", data->temperature, ssid);

    GFX_drawFastHLine(gfx, padding - 10, 68, data->width - 2 * (padding - 10), GFX_BLACK);

    uint16_t cS = data->height / 45;
    uint16_t nD = 2;
    uint16_t time_width = 2 * (nD * (11 * cS + 2) - 2 * cS) + 4 * cS;
    uint16_t time_height = 20 * cS + 4;
    int16_t time_x = (data->width - time_width) / 2;
    int16_t time_y = (68 + (data->height - 68)) / 2 - time_height / 2;
    DrawTime(gfx, tm, time_x, time_y, cS, nD);

    GFX_drawFastHLine(gfx, padding - 10, data->height - 68, data->width - 2 * (padding - 10), GFX_BLACK);

    GFX_setCursor(gfx, padding, data->height - 68 + 30);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "%s%s", Lunar_StemStrig[LUNAR_GetStem(Lunar)], Lunar_BranchStrig[LUNAR_GetBranch(Lunar)]);
    GFX_setTextColor(gfx, text_color, GFX_WHITE);
    GFX_printf(gfx, "%s", Lunar_ZodiacString[LUNAR_GetZodiac(Lunar)]);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_printf(gfx, "年");

    GFX_setCursor(gfx, padding, data->height - 68 + 30 + 20);
    GFX_printf(gfx, " %d周", GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday));

    uint8_t day = 0;
    uint8_t JQday = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &day);
    if (day == 0) {
        GFX_setCursor(gfx, data->width - GFX_getUTF8Width(gfx, "小暑") - padding, data->height - 68 + 30);
        GFX_setTextColor(gfx, text_color, GFX_WHITE);
        GFX_printf(gfx, "%s", JieQiStr[JQday % 24]);
    } else {
        GFX_setCursor(gfx, data->width - GFX_getUTF8Width(gfx, "离小暑") - padding, data->height - 68 + 30);
        GFX_printf(gfx, "离");
        GFX_setTextColor(gfx, text_color, GFX_WHITE);
        GFX_printf(gfx, "%s", JieQiStr[JQday % 24]);
        GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
        // Calculate width without snprintf: "还有" + digits + "天"
        int16_t day_width = GFX_getUTF8Width(gfx, "还有") + GFX_getUTF8Width(gfx, "天");
        day_width += GFX_getUTF8Width(gfx, "0") * ((day >= 10) ? 2 : 1);
        GFX_setCursor(gfx, data->width - day_width - padding, data->height - 68 + 30 + 20);
        GFX_printf(gfx, "还有%d天", day);
    }
}

/**
 * @brief GUI主绘制函数 - 绘制完整界面
 *        Main GUI drawing function - draws complete interface
 *
 * @param data          GUI数据指针 | Pointer to GUI data
 * @param callback      缓冲区回调函数 | Buffer callback function
 * @param callback_data 回调数据 | Callback data
 *
 * 功能(Features)：
 * - 根据显示模式选择不同的界面布局 | Select different layouts based on display mode
 * - 支持黑白/三色/四色屏幕 | Support B/W, 3-color, 4-color screens
 * - 自动计算缓冲区大小以适应可用内存 | Auto calculate buffer size to fit available memory
 * - 使用分页机制处理大屏幕 | Use paging mechanism for large screens
 *
 * 显示模式(Display modes)：
 * - MODE_CALENDAR: 全屏月历 | Full screen calendar
 * - MODE_CLOCK: 全屏时钟 | Full screen clock
 * - MODE_CLOCK_CALENDAR: 分屏时钟+日历 | Split screen clock + calendar
 */
void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data) {
    // 验证一周起始日参数 | Validate week start parameter
    if (data->week_start > 6) data->week_start = 0;

    tm_t tm = {0};
    struct Lunar_Date Lunar;

    // 将UNIX时间戳转换为tm结构 | Convert UNIX timestamp to tm structure
    transformTime(data->timestamp, &tm);

    Adafruit_GFX gfx;
    // 计算页面高度：根据可用堆内存动态调整 | Calculate page height: dynamically adjust based on available heap
    // __HEAP_SIZE是总堆大小，减去512字节保留空间，再除以每行需要的字节数(width/8)
    int16_t ph = (__HEAP_SIZE - 512) / (data->width / 8);

    // 根据颜色模式初始化GFX | Initialize GFX based on color mode
    if (data->color == 2)
        GFX_begin_3c(&gfx, data->width, data->height, ph);  // 三色屏幕 | 3-color screen
    else if (data->color == 3)
        GFX_begin_4c(&gfx, data->width, data->height, ph);  // 四色屏幕 | 4-color screen
    else
        GFX_begin(&gfx, data->width, data->height, ph);     // 黑白屏幕 | B/W screen

    GFX_firstPage(&gfx);
    do {
        // 填充白色背景 | Fill white background
        GFX_fillScreen(&gfx, GFX_WHITE);

        if (data->voltage_mv <= LOW_BATTERY_THRESHOLD_MV) {
            DrawLowBatteryTip(&gfx, data);
            continue;
        }

        // 计算农历日期 | Calculate lunar date
        LUNAR_SolarToLunar(&Lunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);

        // 根据显示模式绘制不同界面 | Draw different interfaces based on display mode
        switch (data->mode) {
            case MODE_CALENDAR:
                DrawCalendar(&gfx, &tm, &Lunar, data);  // 全屏日历 | Full screen calendar
                break;
            case MODE_CLOCK:
                DrawClock(&gfx, &tm, &Lunar, data);     // 全屏时钟 | Full screen clock
                break;
            case MODE_CLOCK_CALENDAR:
                DrawClockCalendar_Full(&gfx, &tm, &Lunar, data);  // 分屏模式 | Split screen mode
                break;
            default:
                break;
        }
        // 如果是2025年1月且未同步时间，显示同步提示 | Show sync reminder if Jan 2025 and time not synced
        if ((data->mode == MODE_CALENDAR || data->mode == MODE_CLOCK) &&
            (tm.tm_year + YEAR0 == 2025 && tm.tm_mon + 1 == 1)) {
            DrawTimeSyncTip(&gfx, data);
        }
    } while (GFX_nextPage(&gfx, callback, callback_data));  // 分页绘制 | Draw with paging

    GFX_end(&gfx);
}

// Draw only the time portion for partial refresh in MODE_CLOCK_CALENDAR
// This draws to a small area containing only the 7-segment time display
// 仅绘制时间部分用于MODE_CLOCK_CALENDAR模式的局部刷新
// 该函数只绘制包含7段数码管样式时间显示的小区域
/**
 * @brief 绘制时钟时间用于局部刷新(MODE_CLOCK_CALENDAR模式)
 *        Draw clock time for partial refresh (MODE_CLOCK_CALENDAR mode)
 *
 * @param data          GUI数据指针 | Pointer to GUI data
 * @param callback      缓冲区回调函数 | Buffer callback function
 * @param callback_data 回调数据 | Callback data
 *
 * 优化说明(Optimization)：
 * - 只绘制时间显示区域，不绘制其他内容 | Only draw time area, not other content
 * - 配合GFX_setWindow使用，可进一步缩小刷新范围到单个数字 | With GFX_setWindow, can further reduce to single digit
 * - 仅使用黑白模式(局部刷新不支持彩色) | Only use B/W mode (partial refresh doesn't support color)
 * - 大幅减少刷新面积，延长屏幕寿命 | Greatly reduce refresh area, extend screen life
 */
  void DrawGUI_ClockOnly(gui_data_t* data, buffer_callback callback, void* callback_data) {
      tm_t tm = {0};
      transformTime(data->timestamp, &tm);

      Adafruit_GFX gfx;
      // Use small time-only area for minimal partial refresh
      // 使用小的时间专用区域以实现最小局部刷新
      int16_t refresh_width = TIME_REFRESH_WIDTH;
      int16_t refresh_height = TIME_REFRESH_HEIGHT;
      int16_t ph = (__HEAP_SIZE - 512) / (refresh_width / 8);

      // For partial refresh, we only use black/white (no color layer)
      // 局部刷新只使用黑白模式(不使用彩色层)
      GFX_begin(&gfx, refresh_width, refresh_height, ph);
      // Limit drawing to the last computed time window so only the
      // changed digit (or full time area) is written to EPD RAM.
      // 限制绘制到上次计算的时间窗口，这样只有变化的数字(或完整时间区域)被写入EPD RAM
      GFX_setWindow(&gfx, s_time_window_x, s_time_window_y, s_time_window_w, s_time_window_h);

    GFX_firstPage(&gfx);
    do {
        GFX_fillScreen(&gfx, GFX_WHITE);

        // Draw only the time display, centered in the refresh area
        // The refresh area starts at TIME_REFRESH_X, TIME_REFRESH_Y on screen
        // But GFX coordinates start at 0,0 for this buffer
        // 只绘制时间显示，在刷新区域内居中
        // 刷新区域在屏幕上从TIME_REFRESH_X, TIME_REFRESH_Y开始
        // 但GFX坐标从此缓冲区的0,0开始
        uint16_t cS = 6;  // Scale factor for 7-segment display | 7段数码管缩放因子
        uint16_t nD = 2;  // Number of digits | 数字位数
        uint16_t time_width = 2 * (nD * (11 * cS + 2) - 2 * cS) + 4 * cS;  // 272
        uint16_t time_height = 20 * cS + 4;  // 124

      // Center the time in the refresh buffer
      // 在刷新缓冲区内居中时间
      int16_t time_x = (refresh_width - time_width) / 2;
      int16_t time_y = (refresh_height - time_height) / 2;

          DrawTime(&gfx, &tm, time_x, time_y, cS, nD);

      } while (GFX_nextPage(&gfx, callback, callback_data));

      GFX_end(&gfx);
  }

  // Get the time refresh area parameters for EPD_service
  // 获取时间刷新区域参数供EPD服务使用
  /**
   * @brief 计算并返回时间显示的局部刷新区域
   *        Calculate and return partial refresh area for time display
   *
   * @param timestamp 当前UNIX时间戳 | Current UNIX timestamp
   * @param x         输出：刷新区域X坐标 | Output: refresh area X coordinate
   * @param y         输出：刷新区域Y坐标 | Output: refresh area Y coordinate
   * @param w         输出：刷新区域宽度 | Output: refresh area width
   * @param h         输出：刷新区域高度 | Output: refresh area height
   *
   * 智能优化策略(Smart optimization strategy)：
   * 1. 默认刷新整个HH:MM时间显示区域
   * 2. 如果仅分钟个位数字变化(如12:08->12:09)，只刷新最右侧的一个数字
   * 3. 这样可以将刷新面积减少到原来的约1/4，大幅降低功耗和屏幕磨损
   *
   * 刷新区域动态调整(Dynamic refresh area adjustment)：
   * - 完整时间区域：约272x124像素
   * - 仅个位数字：约68x124像素
   * - 决策条件：小时十位、小时个位、分钟十位都未变化
   */
  void GetTimeRefreshArea(uint32_t timestamp, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
      // Base time area (centered HH:MM inside TIME_REFRESH_*)
      // 基础时间区域(TIME_REFRESH_*内居中的HH:MM)
      const uint16_t base_x = TIME_REFRESH_X;
      const uint16_t base_y = TIME_REFRESH_Y;
      const uint16_t base_w = TIME_REFRESH_WIDTH;
      const uint16_t base_h = TIME_REFRESH_HEIGHT;

      // 7-segment layout parameters (must match DrawTime/Draw7Number)
      // 7段数码管布局参数(必须与DrawTime/Draw7Number匹配)
      const uint16_t cS = 6;      // scale factor | 缩放因子
      const uint16_t nD = 2;      // digits per number (HH / MM) | 每个数字的位数
      const uint16_t digit_block_width = nD * (11 * cS + 2) - 2 * cS;  // width of "HH" or "MM" | "HH"或"MM"的宽度
      const uint16_t time_width = 2 * digit_block_width + 4 * cS;      // nominal HH:MM width (used for centering) | HH:MM总宽度

      // Default window: full HH:MM area (smaller than TIME_REFRESH_* to avoid touching calendar)
      // 默认窗口：完整的HH:MM区域(小于TIME_REFRESH_*以避免触及日历)
      uint16_t window_x_local = (base_w - time_width) / 2;
      uint16_t window_w = time_width;

    // Vertically, restrict to just the digits area (time_height), centered,
    // but add a small margin above/below to clean residual pixels.
    // 垂直方向，限制在数字区域(time_height)，居中，
    // 但在上下添加小边距以清除残留像素
    uint16_t time_height = 20 * cS + 4;  // must match DrawGUI_ClockOnly | 必须与DrawGUI_ClockOnly匹配
    int16_t window_y_local = (base_h - time_height) / 2;
    int16_t window_h = time_height;
    const int16_t margin_y = 2;
    // Expand upwards if there is room | 如果有空间则向上扩展
    if (window_y_local > margin_y) {
        window_y_local -= margin_y;
        window_h += margin_y;
    }
    // Expand downwards if there is room | 如果有空间则向下扩展
    if (window_y_local + window_h + margin_y <= (int16_t)base_h) {
        window_h += margin_y;
    }

      // Decide whether we can shrink to only the minute ones digit.
      // We do this only when:
      //  - we have a valid previous timestamp,
      //  - time moves forward by a reasonable amount,
      //  - and only the minute ones digit changes (e.g. 00->01, 01->02, ... 08->09).
      // 判断是否可以缩小到只刷新分钟个位数字
      // 仅在以下条件都满足时才缩小：
      //  - 有有效的上次时间戳
      //  - 时间正常向前推进(不是大跳跃)
      //  - 仅分钟个位数字变化(如00->01, 01->02, ..., 08->09)
      if (s_last_time_refresh_ts != 0 && timestamp > s_last_time_refresh_ts &&
          (timestamp - s_last_time_refresh_ts) <= 600) {  // ignore large jumps | 忽略大跳跃(>10分钟)
          // 计算上次和当前的小时和分钟 | Calculate previous and current hours and minutes
          uint32_t prev_minutes = (s_last_time_refresh_ts / 60) % (24 * 60);
          uint32_t curr_minutes = (timestamp / 60) % (24 * 60);

          uint8_t prev_hour = prev_minutes / 60;
          uint8_t prev_min = prev_minutes % 60;
          uint8_t curr_hour = curr_minutes / 60;
          uint8_t curr_min = curr_minutes % 60;

          // 分解为十位和个位 | Break down into tens and ones
          uint8_t prev_ht = prev_hour / 10;  // 小时十位 | Hour tens
          uint8_t prev_hu = prev_hour % 10;  // 小时个位 | Hour ones
          uint8_t prev_mt = prev_min / 10;   // 分钟十位 | Minute tens
          uint8_t prev_mu = prev_min % 10;   // 分钟个位 | Minute ones

          uint8_t curr_ht = curr_hour / 10;
          uint8_t curr_hu = curr_hour % 10;
          uint8_t curr_mt = curr_min / 10;
          uint8_t curr_mu = curr_min % 10;

          // 仅分钟个位变化 | Only minute ones digit changed
          bool only_minute_ones_changed =
              (prev_ht == curr_ht) && (prev_hu == curr_hu) &&
              (prev_mt == curr_mt) && (prev_mu != curr_mu);

          if (only_minute_ones_changed) {
              // Compute local X for the minute block and then its ones digit.
              // DrawTime layout:
              //   - "HH" at time_x
              //   - colon after HH + 2*cS
              //   - "MM" starts at time_x + digit_block_width + 6*cS
              // 计算分钟块的本地X坐标，然后是个位数字的位置
              // DrawTime布局：
              //   - "HH"位于time_x
              //   - HH后的冒号占2*cS
              //   - "MM"从time_x + digit_block_width + 6*cS开始
              uint16_t minute_block_start = window_x_local + digit_block_width + 6 * cS;
              uint16_t digit_single_width = 11 * cS + 2;  // 单个数字的宽度 | Width of single digit
              // Minute ones digit occupies the right part of the MM block.
              // 分钟个位数字占据MM块的右侧部分
              uint16_t minute_ones_left = minute_block_start + (digit_block_width - digit_single_width);

              // 缩小刷新窗口为仅分钟个位数字 | Shrink refresh window to minute ones digit only
              window_x_local = minute_ones_left;
              window_w = digit_single_width;
          }
      }

      // Persist the chosen window for DrawGUI_ClockOnly (GFX_setWindow)
      // 保存选定的窗口供DrawGUI_ClockOnly使用(GFX_setWindow)
      s_last_time_refresh_ts = timestamp;
      s_time_window_x = window_x_local;
    s_time_window_y = (uint16_t)window_y_local;
    s_time_window_w = window_w;
    s_time_window_h = (uint16_t)window_h;

      // Return global refresh region for EPD_service
      // 返回全局刷新区域坐标供EPD服务使用
    *x = base_x + window_x_local;
    *y = base_y + (uint16_t)window_y_local;
    *w = window_w;
    *h = (uint16_t)window_h;
  }

// Get the base origin (top-left) of the time refresh area on the full screen
// 获取时间刷新区域在全屏上的基准原点(左上角)
/**
 * @brief 返回时间刷新区域的基准起点坐标
 *        Return the base origin coordinates of time refresh area
 *
 * @param x 输出X坐标 | Output X coordinate
 * @param y 输出Y坐标 | Output Y coordinate
 */
void GetTimeRefreshOrigin(uint16_t* x, uint16_t* y) {
    if (x) *x = TIME_REFRESH_X;
    if (y) *y = TIME_REFRESH_Y;
}
