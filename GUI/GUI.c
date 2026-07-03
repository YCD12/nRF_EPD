#include "GUI.h"

#include <string.h>

#include "gui_porting_hal.h"
#include "Lunar.h"
#include "fonts.h"
#include "nrf_log.h"


// 带样式的printf宏，简化设置前景色、背景色、字体的操作
// Styled printf macro to simplify setting foreground, background color and font
#define GFX_printf_styled(gfx, fg, bg, font, ...) \
    GFX_setTextColor(gfx, fg, bg);                \
    GFX_setFont(gfx, font);                       \
    GFX_printf(gfx, __VA_ARGS__);

#define BATTERY_FULL_MV 3300
#define BATTERY_EMPTY_MV LOW_BATTERY_THRESHOLD_MV

#include "ui_runtime.h"

#define UI_UPDATE_DAILY GUI_UPDATE_DAILY
#define UI_UPDATE_MINUTE GUI_UPDATE_MINUTE

typedef struct {
    char label[10];
    uint8_t is_festival;
    uint8_t has_holiday_mark;
    uint8_t holiday_work;
} month_day_render_cache_t;

typedef struct {
    bool valid;
    uint16_t year;
    uint8_t month;
    uint8_t days;
    month_day_render_cache_t days_cache[31];
} month_render_cache_t;

#include "gui_porting_hal.h"

static month_render_cache_t m_month_render_cache;

void ui_render_invalidate_month_cache(void) {
    m_month_render_cache.valid = false;
}

static void DrawTimeSyncTip(Adafruit_GFX* gfx, int16_t screen_w, int16_t screen_h);
static void DrawLowBatteryTip(Adafruit_GFX* gfx, uint16_t voltage_mv, uint8_t epd_color, int16_t screen_w, int16_t screen_h);
static void DrawBattery(Adafruit_GFX* gfx, int16_t x, int16_t y, uint8_t iw, uint16_t voltage_mv, uint8_t epd_color);
static void DrawWeekHeaderArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                               int16_t screen_w, int16_t screen_h, uint8_t week_start);
static void DrawMonthDaysArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                              tm_t* tm, int16_t screen_w, int16_t screen_h, uint8_t week_start);
static void DrawTime(Adafruit_GFX* gfx, tm_t* tm, int16_t x, int16_t y, uint16_t cS, uint16_t nD);

static uint8_t BatteryPercentFromMv(uint16_t voltage_mv) {
    if (voltage_mv >= BATTERY_FULL_MV) {
        return 100;
    }
    if (voltage_mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    return (uint8_t)(((uint32_t)(voltage_mv - BATTERY_EMPTY_MV) * 100U +
                      (BATTERY_FULL_MV - BATTERY_EMPTY_MV) / 2U) /
                     (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

static void BatteryPercentToText(uint8_t percent, char* text, size_t text_size) {
    if (text == NULL || text_size < 3) {
        return;
    }

    if (percent >= 100 && text_size >= 5) {
        text[0] = '1';
        text[1] = '0';
        text[2] = '0';
        text[3] = '%';
        text[4] = '\0';
    } else if (percent >= 10 && text_size >= 4) {
        text[0] = (char)('0' + percent / 10);
        text[1] = (char)('0' + percent % 10);
        text[2] = '%';
        text[3] = '\0';
    } else {
        text[0] = (char)('0' + percent);
        text[1] = '%';
        text[2] = '\0';
    }
}

/**
 * @brief 获取指定日期的节日名称（数据驱动版）
 *        Get festival name for a specific date (data-driven)
 *
 * @param year    公历年份 | Gregorian year
 * @param mon     公历月份(1-12) | Gregorian month (1-12)
 * @param day     公历日期(1-31) | Gregorian day (1-31)
 * @param week    星期几(未使用，保留兼容签名) | Day of week (unused, kept for ABI)
 * @param Lunar   农历日期结构体指针(未使用，保留兼容签名) | Lunar date (unused)
 * @param festival 输出缓冲区(至少 10 字节) | Output buffer (>= 10 bytes)
 * @return true 找到节日 | Festival found
 *
 * 所有节日/规则节日/调休均已在打包端预计算并固化进 LCDB。此处仅按优先级读取：
 * 1. 数据库节日字符串(festival_offset)
 * 2. 二十四节气(solar_term)，清明补"节"字
 */
static bool GetFestival(uint16_t year, uint8_t mon, uint8_t day, uint8_t week, struct Lunar_Date* Lunar,
                        char* festival) {
    daily_record_t rec;

    (void)week;
    (void)Lunar;

    if (lunar_db_get_record(year, mon, day, &rec) != 0) {
        return false;
    }

    if (rec.festival_offset != 0xFFFF) {
        lunar_db_get_string(year, rec.festival_offset, festival, 10);
        return festival[0] != '\0';
    }

    if (rec.solar_term != 0) {
        LUNAR_GetJieQiName((uint8_t)(rec.solar_term - 1), festival, 10);
        if ((rec.solar_term - 1) == 6) {  // 清明既是节气又是节日
            strcat(festival, "节");
        }
        return true;
    }

    return false;
}

static void month_cache_copy_label(month_day_render_cache_t* cache, const char* label) {
    strncpy(cache->label, label, sizeof(cache->label) - 1U);
    cache->label[sizeof(cache->label) - 1U] = '\0';
}

static const month_render_cache_t* PrepareMonthRenderCache(uint16_t year, uint8_t month) {
    uint8_t month_days;

    if (m_month_render_cache.valid &&
        m_month_render_cache.year == year &&
        m_month_render_cache.month == month) {
        return &m_month_render_cache;
    }

    month_days = thisMonthMaxDays((uint8_t)year, month);
    if (month_days == 0 || month_days > 31) {
        m_month_render_cache.valid = false;
        return NULL;
    }

    memset(&m_month_render_cache, 0, sizeof(m_month_render_cache));
    m_month_render_cache.year = year;
    m_month_render_cache.month = month;
    m_month_render_cache.days = month_days;

    for (uint8_t day = 1; day <= month_days; day++) {
        month_day_render_cache_t* cache = &m_month_render_cache.days_cache[day - 1U];
        struct Lunar_Date day_lunar;
        daily_record_t day_rec;
        char label[10] = {0};
        uint8_t actual_week = (uint8_t)((get_first_day_week(year, month) + day - 1U) % 7U);

        gui_porting_feed_wdt();
        LUNAR_SolarToLunar(&day_lunar, year, month, day);

        if (GetFestival(year, month, day, actual_week, &day_lunar, label)) {
            cache->is_festival = 1U;
            month_cache_copy_label(cache, label);
        } else if (day_lunar.Date == 1) {
            char leap_name[4];
            char month_name[7];
            strcpy(label, LUNAR_GetLeapName(day_lunar.IsLeap, leap_name, sizeof(leap_name)));
            strcat(label, LUNAR_GetMonthName(day_lunar.Month, month_name, sizeof(month_name)));
            month_cache_copy_label(cache, label);
        } else {
            month_cache_copy_label(cache, LUNAR_GetDateName(day_lunar.Date, label, sizeof(label)));
        }

        /* 调休标志直接取自 LCDB 预计算记录：1=休(放假)，2=班(补班)。 */
        if (lunar_db_get_record(year, month, day, &day_rec) == 0 && day_rec.holiday_type != 0U) {
            cache->has_holiday_mark = 1U;
            cache->holiday_work = (day_rec.holiday_type == 2U) ? 1U : 0U;
        }
    }

    m_month_render_cache.valid = true;
    return &m_month_render_cache;
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
static void DrawTimeSyncTip(Adafruit_GFX* gfx, int16_t screen_w, int16_t screen_h) {
    const char* title = "SYNC TIME!";
    const char* url = "https://ycd12.github.io/EPD-nRF5_DYC/";

    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);

    // 计算提示框尺寸和位置 | Calculate box size and position
    int16_t fh = GFX_getFontHeight(gfx);
    int16_t box_w = GFX_getUTF8Width(gfx, url) + 20;  // URL宽度+边距 | URL width + padding
    int16_t box_h = fh * 2 + 20;                       // 两行文字+边距 | 2 lines + padding
    int16_t box_x = (screen_w - box_w) / 2;         // 水平居中 | Horizontally centered
    int16_t box_y = screen_h / 2 - box_h / 2;      // 垂直居中 | Vertically centered

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
static void DrawLowBatteryTip(Adafruit_GFX* gfx, uint16_t voltage_mv, uint8_t epd_color, int16_t screen_w, int16_t screen_h) {
    char volts_text[4];
    volts_text[0] = (char)('0' + (voltage_mv / 1000));
    volts_text[1] = '.';
    volts_text[2] = (char)('0' + ((voltage_mv % 1000) / 100));
    volts_text[3] = '\0';

    uint16_t warning_color = (epd_color == 2) ? GFX_RED : GFX_BLACK;
    int16_t W = screen_w;
    int16_t H = screen_h;

    int16_t box_w = (W >= 800) ? 360 : ((W >= 400) ? 240 : (W - 16));
    int16_t box_h = (H >= 480) ? 200 : ((H >= 300) ? 150 : (H - 16));
    int16_t box_x = (W - box_w) / 2;
    int16_t box_y = (H - box_h) / 2;

    // Draw double rounded border for a premium dialog box
    GFX_drawRoundRect(gfx, box_x, box_y, box_w, box_h, 8, warning_color);
    GFX_drawRoundRect(gfx, box_x + 2, box_y + 2, box_w - 4, box_h - 4, 6, warning_color);

    int16_t body_y, title_y, status_y, inst_y;
    int16_t body_w, body_h, term_w, term_r, fill_margin;
    const uint8_t* num_font;
    const char* inst_text;

    if (H >= 480) {
        body_w = 100; body_h = 48;
        term_w = 6; term_r = 2; fill_margin = 4;
        num_font = u8g2_font_helvB18_tn;
        inst_text = "Please change battery";

        body_y = box_y + 25;
        status_y = body_y + body_h + 32;
        title_y = status_y + 26;
        inst_y = title_y + 22;
    } else if (H >= 300) {
        body_w = 80; body_h = 40;
        term_w = 5; term_r = 2; fill_margin = 3;
        num_font = u8g2_font_helvB14_tn;
        inst_text = "Please change battery";

        body_y = box_y + 15;
        status_y = body_y + body_h + 26;
        title_y = status_y + 22;
        inst_y = title_y + 18;
    } else {
        body_w = 50; body_h = 24;
        term_w = 3; term_r = 1; fill_margin = 2;
        num_font = u8g2_font_helvB14_tn;
        inst_text = "Change battery";

        body_y = box_y + 10;
        title_y = body_y + body_h + 16;
        status_y = title_y + 24;
        inst_y = status_y + 18;
    }
    int16_t term_h = body_h / 3;

    // 1. Draw Battery Icon (Centered)
    int16_t body_x = box_x + (box_w - body_w) / 2;
    GFX_drawRoundRect(gfx, body_x, body_y, body_w, body_h, (H >= 300) ? 6 : 3, warning_color);
    GFX_fillRoundRect(gfx, body_x + body_w + ((H >= 300) ? 2 : 1), body_y + (body_h - term_h) / 2, term_w, term_h, term_r, warning_color);

    int16_t inner_h = body_h - fill_margin * 2;
    int16_t segment_w = (H >= 300) ? ((body_w - fill_margin * 2) / 8) : 4;
    if (segment_w < 4) segment_w = 4;
    GFX_fillRect(gfx, body_x + fill_margin, body_y + fill_margin, segment_w, inner_h, warning_color);

    // 2. Draw Battery Status Text (e.g. 2.7 V)
    GFX_setFont(gfx, num_font);
    int16_t w_num = GFX_getUTF8Width(gfx, volts_text);
    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    int16_t w_unit = GFX_getUTF8Width(gfx, " V");
    int16_t total_w = w_num + w_unit;
    int16_t start_x = box_x + (box_w - total_w) / 2;

    GFX_setFont(gfx, num_font);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, start_x, status_y);
    GFX_printf(gfx, "%s", volts_text);

    GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, start_x + w_num, status_y);
    GFX_printf(gfx, " V");

    // 3. Draw Warning Title (LOW POWER / POWER OFF)
    const char* title_text = (voltage_mv < CRITICAL_BATTERY_SHUTDOWN_MV) ? "POWER OFF" : "LOW POWER";
    GFX_setTextColor(gfx, warning_color, GFX_WHITE);
    int16_t title_w = GFX_getUTF8Width(gfx, title_text);
    GFX_setCursor(gfx, box_x + (box_w - title_w) / 2, title_y);
    GFX_printf(gfx, "%s", title_text);

    // 4. Draw Instruction Text
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    int16_t inst_w = GFX_getUTF8Width(gfx, inst_text);
    GFX_setCursor(gfx, box_x + (box_w - inst_w) / 2, inst_y);
    GFX_printf(gfx, "%s", inst_text);
}

/**
 * @brief 绘制电池图标和电量显示
 *        Draw battery icon and battery level display
 *
 * @param gfx        图形上下文指针 | Pointer to graphics context
 * @param x          右边界X坐标 | Right edge X coordinate
 * @param y          Y坐标 | Y coordinate
 * @param iw         图标宽度 | Icon width
 * @param voltage_mv 电压(毫伏)，内部换算为电量百分比 | Voltage in millivolts, displayed as battery percentage
 *
 * 电池百分比显示：满电=3.3V，空电=2.8V
 * Battery percentage display: Full=3.3V, Empty=2.8V
 */
static void DrawBattery(Adafruit_GFX* gfx, int16_t x, int16_t y, uint8_t iw, uint16_t voltage_mv, uint8_t epd_color) {
    x -= iw;  // 从右边界开始向左绘制 | Start drawing from right edge to left
    uint8_t fill_width = (iw > 4) ? (uint8_t)(iw - 4) : 0;
    uint8_t level = BatteryPercentFromMv(voltage_mv);

    uint16_t color = GFX_BLACK;
    if (level <= 20) {
        if (epd_color == 2) {
            color = GFX_RED;
        }
    }

    // 绘制电池外框 | Draw battery outline
    GFX_fillRect(gfx, x, y, iw, 10, GFX_WHITE);   // 白色填充 | White fill
    GFX_drawRect(gfx, x, y, iw, 10, color);       // 边框颜色 | Border color
    GFX_fillRect(gfx, x + iw, y + 4, 2, 2, color);  // 电池正极凸起 | Battery positive terminal
    // 根据电量绘制填充 | Draw fill based on battery level
    GFX_fillRect(gfx, x + 2, y + 2, (uint16_t)(fill_width * level / 100), 6, color);

    // 低电量时(<= 20%)，显示电量百分比和低电量提示 | Low battery (<= 20%), show percentage and low battery tip
    if (level <= 20) {
        char battery_text[16] = "LOW ";
        BatteryPercentToText(level, battery_text + 4, sizeof(battery_text) - 4);

        GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
        GFX_setTextColor(gfx, color, GFX_WHITE);
        int16_t text_w = GFX_getUTF8Width(gfx, battery_text);
        GFX_setCursor(gfx, x - text_w - 2, y + 9);
        GFX_printf(gfx, "%s", battery_text);
    }
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
static void DrawWeekHeaderArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                               int16_t screen_w, int16_t screen_h, uint8_t week_start) {
    if (area_w <= 0) area_w = screen_w - 2 * x;
    if (area_h <= 0) area_h = screen_h > 300 ? 32 : 24;

    GFX_setFont(gfx, screen_h > 300 ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
    uint8_t w = (uint8_t)(area_w / 7);  // 每列宽度 | Width per column
    uint8_t h = (uint8_t)area_h;        // 行高 | Row height
    uint8_t r = (uint8_t)(area_w % 7);  // 剩余像素分配给最后一列 | Remaining pixels for last column
    uint8_t fh = (h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;  // 垂直居中 | Vertical center
    char day_name[4];
    int16_t cw = GFX_getUTF8Width(gfx, LUNAR_GetDayName(0, day_name, sizeof(day_name)));
    for (int i = 0; i < 7; i++) {
        // 根据一周起始日设置调整显示顺序 | Adjust display order based on week start setting
        uint8_t day = (week_start + i) % 7;
        uint16_t bg = (day == 0 || day == 6) ? GFX_RED : GFX_BLACK;  // 周末用红色 | Weekends in red
        GFX_fillRect(gfx, x + i * w, y, i == 6 ? (w + r) : w, h, bg);
        GFX_setTextColor(gfx, GFX_WHITE, bg);
        GFX_setCursor(gfx, x + (w - cw) / 2 + i * w, y + fh);
        GFX_printf(gfx, "%s", LUNAR_GetDayName(day, day_name, sizeof(day_name)));
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
static void DrawMonthDaysArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                              tm_t* tm, int16_t screen_w, int16_t screen_h, uint8_t week_start) {
    uint16_t year = tm->tm_year + YEAR0;
    uint8_t month = tm->tm_mon + 1;
    const month_render_cache_t* month_cache = PrepareMonthRenderCache(year, month);

    // 获取本月1号是星期几 | Get which day of week is the 1st day of month
    uint8_t firstDayWeek = get_first_day_week(year, month);
    // 根据用户设置的一周起始日调整首日位置 | Adjust first day position based on user's week start setting
    int8_t adjustedFirstDay = (firstDayWeek - week_start + 7) % 7;
    // 获取本月天数 | Get number of days in this month
    uint8_t monthMaxDays = (month_cache != NULL) ? month_cache->days : thisMonthMaxDays((uint8_t)year, month);
    // 计算需要的行数 | Calculate required rows
    // 公式：1 + (总天数 - 第一周剩余天数 + 6) / 7
    uint8_t monthDayRows = 1 + (monthMaxDays - (7 - adjustedFirstDay) + 6) / 7;

    if (area_w <= 0) area_w = screen_w - x - 10;
    if (area_h <= 0) area_h = screen_h - y - 10;

    // 计算每个单元格的宽度和高度 | Calculate cell width and height
    int16_t bw = area_w / 7;
    int16_t bh = area_h / monthDayRows;
    bool large = screen_h > 300;

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
        gui_porting_feed_wdt();

        uint8_t day = i + 1;
        const month_day_render_cache_t* day_cache = (month_cache != NULL) ? &month_cache->days_cache[i] : NULL;

        // actualWeek: 实际星期几(0=周日) | Actual day of week (0=Sunday)
        int16_t actualWeek = (firstDayWeek + i) % 7;
        // displayWeek: 根据用户设置调整后的显示列 | Display column after user setting adjustment
        int16_t displayWeek = (adjustedFirstDay + i) % 7;
        bool weekend = (actualWeek == 0) || (actualWeek == 6);

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
        const char* festival = (day_cache != NULL && day_cache->label[0] != '\0') ? day_cache->label : "--";
        GFX_setFont(gfx, large ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
        GFX_setFontMode(gfx, 1);  // transparent | 透明模式
        if (day_cache != NULL && day_cache->is_festival != 0U) {
            if (day != tm->tm_mday) GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);  // 节日用红色 | Festivals in red
        }
        // 绘制节日/农历文字 | Draw festival/lunar text
        GFX_setCursor(gfx, bx + (2 * cr - GFX_getUTF8Width(gfx, festival)) / 2, gfx->ty + GFX_getFontHeight(gfx) + 3);
        GFX_printf(gfx, "%s", festival);

        // 绘制放假/调休标记 | Draw holiday/workday marker
        if (day_cache != NULL && day_cache->has_holiday_mark != 0U) {
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
            GFX_setTextColor(gfx, day_cache->holiday_work ? GFX_BLACK : GFX_RED, GFX_WHITE);
            GFX_setCursor(gfx, bx + (large ? 31 : 22), by + 3);
            GFX_printf(gfx, "%s", day_cache->holiday_work ? "班" : "休");
        }

        gui_porting_feed_wdt();
    }
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
void Draw7Number(Adafruit_GFX *gfx, int n, unsigned int xLoc, unsigned int yLoc, char cS, unsigned int fC, unsigned int bC, int nD) {
    unsigned char c_abs = (unsigned char)abs(cS);
    unsigned char c = c_abs > 10 ? 10 : (c_abs < 1) ? 1 : c_abs;
    unsigned char cnt_abs = (unsigned char)abs(nD);
    unsigned char cnt = cnt_abs > 10 ? 10 : (cnt_abs < 1) ? 1 : cnt_abs;
    unsigned int num = abs(n);
    unsigned int i, j, d;
    unsigned int S2 = 5 * c;
    unsigned int S3 = 2 * c;
    unsigned int S4 = 7 * c;
    unsigned int x1 = c + 1;
    unsigned int x2 = S3 + S2 + 1;
    unsigned int y1 = yLoc + x1;
    unsigned int y3 = yLoc + S3 + S4 + 1;

    unsigned int seg[7][3] = {
        {x1, yLoc, 1},
        {x2, y1, 0},
        {x2, y3 + x1, 0},
        {x1, (2 * y3) - yLoc, 1},
        {0, y3 + x1, 0},
        {0, y1, 0},
        {x1, y3, 1}
    };
    unsigned char nums[12] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x00, 0x40
    };

    d = S2 + (3 * S3) + 2;
    xLoc += cnt * d;

    for (; cnt > 0; cnt--) {
        i = (num > 9) ? num % 10 : ((!cnt) && (n < 0)) ? 11 : ((nD < 0) && (!num)) ? 10 : num;
        xLoc -= d;
        num /= 10;

        for (j = 0; j < 7; ++j) {
            unsigned int col = (nums[i] & (1 << j)) ? fC : bC;
            if (seg[j][2]) {
                // Horizontal segment
                unsigned int w_val = S2;
                unsigned int t_val = seg[j][1] + S3;
                unsigned int h_val = seg[j][1] + c;
                unsigned int a_val = xLoc + seg[j][0] + c;
                unsigned int b_val = seg[j][1];
                for (; b_val < h_val; b_val++, a_val--, w_val += 2) {
                    GFX_drawFastHLine(gfx, a_val, b_val, w_val, col);
                }
                for (; b_val < t_val; b_val++, a_val++, w_val -= 2) {
                    GFX_drawFastHLine(gfx, a_val, b_val, w_val, col);
                }
            } else {
                // Vertical segment
                unsigned int w_val = S4;
                unsigned int t_val = xLoc + seg[j][0] + S3;
                unsigned int h_val = xLoc + seg[j][0] + c;
                unsigned int b_val = xLoc + seg[j][0];
                unsigned int a_val = seg[j][1] + c;
                for (; b_val < h_val; b_val++, a_val--, w_val += 2) {
                    GFX_drawFastVLine(gfx, b_val, a_val, w_val, col);
                }
                for (; b_val < t_val; b_val++, a_val++, w_val -= 2) {
                    GFX_drawFastVLine(gfx, b_val, a_val, w_val, col);
                }
            }
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
    GFX_fillRect(gfx, x, y + (9 * cS) / 2 + 1, 2 * cS, 2 * cS, GFX_BLACK);
    GFX_fillRect(gfx, x, y + (27 * cS) / 2 + 3, 2 * cS, 2 * cS, GFX_BLACK);
    x += 4 * cS;
    // 绘制分钟 | Draw minutes
    Draw7Number(gfx, tm->tm_min, x, y, cS, GFX_BLACK, GFX_WHITE, nD);
}

static void ui_draw_date_ymd(Adafruit_GFX* gfx, int16_t x, int16_t y, tm_t* tm, uint8_t flags) {
    const char* fmt = (flags & UI_TEXT_FLAG_ZERO_PAD) ? "%02d" : "%d";

    GFX_setCursor(gfx, x, y);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_helvB18_tn, "%d", tm->tm_year + YEAR0);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "年");
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_helvB18_tn, fmt, tm->tm_mon + 1);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "月");
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_helvB18_tn, fmt, tm->tm_mday);
    GFX_printf_styled(gfx, GFX_BLACK, GFX_WHITE, u8g2_font_wqy12_t_lunar, "日");
}

static int16_t ui_temperature_width(Adafruit_GFX* gfx, int8_t temperature, const char* suffix) {
    int8_t temp = temperature;
    int16_t digit_width;
    int16_t width;

    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    digit_width = GFX_getUTF8Width(gfx, "0");
    width = digit_width;
    if (temp < 0) {
        width += GFX_getUTF8Width(gfx, "-");
        temp = -temp;
    }
    if (temp >= 10) width += digit_width;
    if (temp >= 100) width += digit_width;

    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    width += GFX_getUTF8Width(gfx, suffix);
    return width;
}

static void ui_draw_temperature(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int8_t temperature, uint8_t epd_color, uint8_t flags) {
    (void)epd_color;
    if ((flags & UI_TEXT_FLAG_RIGHT_ALIGN) && w > 0) {
        x += w - ui_temperature_width(gfx, temperature, "℃");
    }

    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_setCursor(gfx, x, y);
    GFX_printf(gfx, "%d", temperature);
    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_printf(gfx, "℃");
}

static void ui_draw_jieqi(Adafruit_GFX* gfx, int16_t x, int16_t y1, int16_t y2, int16_t w, tm_t* tm) {
    char jq_name[7];
    uint8_t days = 0;
    uint8_t jieqi = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &days);
    const char* jq_str;
    int16_t text_w;

    if (jieqi == 0xFF) return;

    GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    jq_str = LUNAR_GetJieQiName(jieqi % 24, jq_name, sizeof(jq_name));
    if (days == 0) {
        text_w = GFX_getUTF8Width(gfx, jq_str);
        GFX_setCursor(gfx, x + w - text_w, y1);
        GFX_printf(gfx, "%s", jq_str);
        return;
    }

    text_w = GFX_getUTF8Width(gfx, "离") + GFX_getUTF8Width(gfx, jq_str);
    GFX_setCursor(gfx, x + w - text_w, y1);
    GFX_printf(gfx, "离%s", jq_str);

    text_w = GFX_getUTF8Width(gfx, "还有") + GFX_getUTF8Width(gfx, "天");
    text_w += GFX_getUTF8Width(gfx, "0") * ((days >= 10) ? 2 : 1);
    GFX_setCursor(gfx, x + w - text_w, y2);
    GFX_printf(gfx, "还有%d天", days);
}

static const uint8_t* ui_text_font(uint8_t font_id) {
    return (font_id == 9U) ? u8g2_font_wqy9_t_lunar : u8g2_font_wqy12_t_lunar;
}

static char* ui_append_text(char* dst, char* end, const char* src) {
    while (dst < end && *src != '\0') {
        *dst++ = *src++;
    }
    if (dst <= end) {
        *dst = '\0';
    }
    return dst;
}

static char* ui_append_uint(char* dst, char* end, uint16_t value) {
    char tmp[5];
    uint8_t len = 0;

    do {
        tmp[len++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && len < sizeof(tmp));

    while (len != 0U && dst < end) {
        *dst++ = tmp[--len];
    }
    if (dst <= end) {
        *dst = '\0';
    }
    return dst;
}

static char* ui_append_int8(char* dst, char* end, int8_t value) {
    uint8_t magnitude;

    if (value < 0) {
        if (dst < end) *dst++ = '-';
        magnitude = (uint8_t)(-value);
    } else {
        magnitude = (uint8_t)value;
    }
    return ui_append_uint(dst, end, magnitude);
}

static bool ui_build_text_field(const ui_component_record_t* comp, tm_t* tm,
                                struct Lunar_Date* Lunar, gui_data_t* data,
                                char* text, size_t text_len, const uint8_t** font) {
    char* dst = text;
    char* end = text + text_len - 1U;
    char name_a[7];
    char name_b[7];
    char name_c[7];
    size_t ssid_len;

    if (text == NULL || text_len == 0U || font == NULL) return false;
    text[0] = '\0';
    *font = ui_text_font(comp->font_id);

    switch (comp->data_id) {
        case UI_TEXT_LUNAR_DATE:
            if (Lunar == NULL) return false;
            dst = ui_append_text(dst, end, LUNAR_GetLeapName(Lunar->IsLeap, name_a, sizeof(name_a)));
            dst = ui_append_text(dst, end, LUNAR_GetMonthName(Lunar->Month, name_b, sizeof(name_b)));
            (void)ui_append_text(dst, end, LUNAR_GetDateName(Lunar->Date, name_c, sizeof(name_c)));
            return true;

        case UI_TEXT_LUNAR_YEAR:
            if (Lunar == NULL) return false;
            dst = ui_append_text(dst, end, LUNAR_GetStemName(LUNAR_GetStem(Lunar), name_a, sizeof(name_a)));
            dst = ui_append_text(dst, end, LUNAR_GetBranchName(LUNAR_GetBranch(Lunar), name_b, sizeof(name_b)));
            if (comp->flags & UI_TEXT_FLAG_WITH_ZODIAC) {
                dst = ui_append_text(dst, end, LUNAR_GetZodiacName(LUNAR_GetZodiac(Lunar), name_c, sizeof(name_c)));
            }
            (void)ui_append_text(dst, end, "年");
            return true;

        case UI_TEXT_WEEK_NUMBER:
            if (comp->flags & UI_TEXT_FLAG_WEEK_LEADING_SPACE) dst = ui_append_text(dst, end, " ");
            dst = ui_append_uint(dst, end, GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday));
            (void)ui_append_text(dst, end, "周");
            return true;

        case UI_TEXT_WEEKDAY:
            dst = ui_append_text(dst, end, "星期");
            (void)ui_append_text(dst, end, LUNAR_GetDayName(tm->tm_wday, name_a, sizeof(name_a)));
            *font = u8g2_font_wqy9_t_lunar;
            return true;

        case UI_TEXT_TEMP_SSID:
            ssid_len = strlen(data->ssid);
            dst = ui_append_int8(dst, end, data->temperature);
            dst = ui_append_text(dst, end, "℃[");
            if (ssid_len > 4U) {
                dst = ui_append_text(dst, end, &data->ssid[ssid_len - 4U]);
            } else {
                dst = ui_append_text(dst, end, data->ssid);
            }
            (void)ui_append_text(dst, end, "]");
            *font = u8g2_font_wqy9_t_lunar;
            return true;

        case UI_TEXT_SSID:
            (void)ui_append_text(dst, end, data->ssid);
            *font = u8g2_font_wqy9_t_lunar;
            return true;

        default:
            return false;
    }
}

static void ui_draw_text_buffer(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w,
                                uint8_t flags, const uint8_t* font, const char* text) {
    GFX_setFont(gfx, font);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    if ((flags & UI_TEXT_FLAG_RIGHT_ALIGN) && w > 0) {
        x += w - GFX_getUTF8Width(gfx, text);
    }
    GFX_setCursor(gfx, x, y);
    (void)GFX_write(gfx, text, strlen(text));
}

static void ui_render_text_field(Adafruit_GFX* gfx, const ui_component_record_t* comp,
                                 tm_t* tm, struct Lunar_Date* Lunar, gui_data_t* data,
                                 int16_t x, int16_t y, int16_t y2) {
    char text[32];
    const uint8_t* font;
    uint8_t flags = comp->flags;
    int16_t width = comp->w;

    switch (comp->data_id) {
        case UI_TEXT_DATE_YMD:
            ui_draw_date_ymd(gfx, x, y, tm, comp->flags);
            return;
        case UI_TEXT_TEMPERATURE:
            ui_draw_temperature(gfx, x, y, comp->w, data->temperature, data->color, comp->flags);
            return;
        case UI_TEXT_JIEQI:
            ui_draw_jieqi(gfx, x, y, y2, comp->w, tm);
            return;
        default:
            break;
    }

    if (ui_build_text_field(comp, tm, Lunar, data, text, sizeof(text), &font)) {
        if (comp->data_id == UI_TEXT_TEMP_SSID) {
            flags |= UI_TEXT_FLAG_RIGHT_ALIGN;
            if (width > 2) width -= 2;
        }
        ui_draw_text_buffer(gfx, x, y, width, flags, font, text);
    }
}

static void __attribute__((noinline)) ui_render_component(Adafruit_GFX* gfx, const ui_component_record_t* comp,
                                tm_t* tm, struct Lunar_Date* lunar, gui_data_t* data,
                                int16_t origin_x, int16_t origin_y) {
    int16_t x = comp->x - origin_x;
    int16_t y = comp->y - origin_y;

    if (comp->flags & UI_FLAG_PARTIAL_ONLY) {
        return;
    }

    switch (comp->type) {
        case UI_COMP_WEEK_HEADER:
            DrawWeekHeaderArea(gfx, x, y, comp->w, comp->h, data->width, data->height, data->week_start);
            break;
        case UI_COMP_MONTH_GRID:
            DrawMonthDaysArea(gfx, x, y, comp->w, comp->h, tm, data->width, data->height, data->week_start);
            break;
        case UI_COMP_TIME_7SEG:
            DrawTime(gfx, tm, x, y,
                     ui_runtime_clamp_u16(comp->p0, UI_TIME_SCALE_MIN, UI_TIME_SCALE_MAX),
                     ui_runtime_clamp_u16(comp->p1, UI_TIME_DIGITS_MIN, UI_TIME_DIGITS_MAX));
            break;
        case UI_COMP_BATTERY:
            DrawBattery(gfx, x, y, (uint8_t)comp->w, data->voltage_mv, data->color);
            break;
        case UI_COMP_LINE:
            GFX_fillRect(gfx, x, y, comp->w, comp->h, comp->fg);
            break;
        case UI_COMP_TEXT_FIELD:
            ui_render_text_field(gfx, comp, tm, lunar, data, x, y,
                                 comp->p0 > 0 ? (int16_t)(comp->p0 - origin_y) : (int16_t)(y + 20));
            break;
        case UI_COMP_LOW_BATTERY_TIP:
            DrawLowBatteryTip(gfx, data->voltage_mv, data->color, data->width, data->height);
            break;
        case UI_COMP_TIME_SYNC_TIP:
            if (((tm->tm_year + YEAR0 == 2025) && (tm->tm_mon + 1 == 1)) ||
                !lunar_db_year_supported((uint16_t)(tm->tm_year + YEAR0))) {
                DrawTimeSyncTip(gfx, data->width, data->height);
            }
            break;
        default:
            break;
    }
}

static void ui_render_component_list(Adafruit_GFX* gfx, const ui_component_record_t* components,
                                     uint8_t component_count, tm_t* tm,
                                     struct Lunar_Date* lunar, gui_data_t* data,
                                     int16_t origin_x, int16_t origin_y) {
    for (uint8_t i = 0; i < component_count; i++) {
        ui_render_component(gfx, &components[i], tm, lunar, data, origin_x, origin_y);
        gui_porting_feed_wdt();
    }
}

void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data) {
    if (data->week_start > 6) data->week_start = 0;

    tm_t tm = {0};
    struct Lunar_Date Lunar;

    transformTime(data->timestamp, &tm);
    bool low_battery = (data->voltage_mv <= LOW_BATTERY_THRESHOLD_MV);
    uint8_t render_mode_id = low_battery ? UI_MODE_LOW_BATTERY : (uint8_t)data->mode;
    ui_mode_record_t cached_mode;
    const ui_component_record_t* cached_components = NULL;
    uint8_t cached_component_count = 0;
    ret_code_t ui_ret;

    if (!low_battery) {
        LUNAR_SolarToLunar(&Lunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
    }
    ui_ret = ui_runtime_load_render_cache(render_mode_id, data->width, data->height,
                                          &cached_mode, &cached_components,
                                          &cached_component_count);
    if (ui_ret != NRF_SUCCESS) {
        cached_components = NULL;
        cached_component_count = 0;
    }

    Adafruit_GFX gfx;
    int16_t ph = (__HEAP_SIZE - 512) / (data->width / 8);

    if (data->color == 2)
        GFX_begin_3c(&gfx, data->width, data->height, ph);
    else
        GFX_begin(&gfx, data->width, data->height, ph);

    GFX_firstPage(&gfx);
    do {
        GFX_fillScreen(&gfx, GFX_WHITE);

        if (low_battery) {
            ui_render_component_list(&gfx, cached_components, cached_component_count,
                                     &tm, &Lunar, data, 0, 0);
            continue;
        }

        ui_render_component_list(&gfx, cached_components, cached_component_count,
                                 &tm, &Lunar, data, 0, 0);
    } while (GFX_nextPage(&gfx, callback, callback_data));

    GFX_end(&gfx);
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
/* ==========================================================================
 * Resource-missing fallback screen.
 *
 * Drawn when the external FM25Q04B resource bundle is missing or invalid
 * (fresh DFU, blank Flash, manifest CRC mismatch). Self-contained: carries
 * its own 5x7 ASCII font so it works without any external Flash resource.
 *
 * Layout mirrors DrawTimeSyncTip — title "SYNC TIME!" + the sync URL.
 * ========================================================================== */

#define FALLBACK_SYNC_TITLE  "SYNC TIME!"
#define FALLBACK_SYNC_URL    "https://ycd12.github.io/EPD-nRF5_DYC/"
#define FALLBACK_GLYPH_W     5
#define FALLBACK_GLYPH_H     7
#define FALLBACK_GLYPH_ADV   6  /* 5 columns + 1 column spacing */

/* Sparse 5x7 font — only chars in FALLBACK_SYNC_TITLE + FALLBACK_SYNC_URL.
 * Last entry '?' is the fallback for unknown chars.
 * keys[]: 37B, glyphs[][5]: 185B = 222B total vs original 475B. */
static const uint8_t fallback_keys[] = {
    ' ','!','-','.','/',
    '1','2','5',':',
    'C','D','E','F','I','M','N','P','R','S','T','Y','_',
    'b','c','d','e','g','h','i','n','o','p','s','t','u','y',
    '?'
};
static const uint8_t fallback_glyphs[][FALLBACK_GLYPH_W] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x01,0x01}, /* 'F' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x03,0x04,0x78,0x04,0x03}, /* 'Y' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' fallback */
};

static void fallback_draw_char(Adafruit_GFX* gfx, int16_t x, int16_t y,
                               char ch, uint16_t color, uint8_t scale) {
    const uint8_t* glyph;
    uint8_t idx = (uint8_t)(sizeof(fallback_keys) - 1U); /* default: '?' */
    for (uint8_t i = 0; i < sizeof(fallback_keys) - 1U; i++) {
        if (fallback_keys[i] == (uint8_t)ch) { idx = i; break; }
    }
    glyph = fallback_glyphs[idx];
    for (uint8_t col = 0; col < FALLBACK_GLYPH_W; col++) {
        uint8_t bits = glyph[col];
        for (uint8_t row = 0; row < FALLBACK_GLYPH_H; row++) {
            if (bits & (1u << row)) {
                if (scale == 1) {
                    GFX_drawPixel(gfx, x + col, y + row, color);
                } else {
                    GFX_fillRect(gfx, x + col * scale, y + row * scale,
                                 scale, scale, color);
                }
            }
        }
    }
}

static int16_t fallback_text_width(const char* text, uint8_t scale) {
    int16_t len = (int16_t)strlen(text);
    if (len == 0) return 0;
    return len * FALLBACK_GLYPH_ADV * scale - scale;
}

static void fallback_draw_text(Adafruit_GFX* gfx, int16_t x, int16_t y,
                               const char* text, uint16_t color, uint8_t scale) {
    while (*text) {
        fallback_draw_char(gfx, x, y, *text++, color, scale);
        x += FALLBACK_GLYPH_ADV * scale;
    }
}

static void fallback_paint(Adafruit_GFX* gfx, int16_t screen_w, int16_t screen_h) {
    int16_t W = screen_w;
    int16_t H = screen_h;
    const uint8_t title_scale = 4;
    const uint8_t url_scale = 2;
    int16_t title_w = fallback_text_width(FALLBACK_SYNC_TITLE, title_scale);
    int16_t title_h = FALLBACK_GLYPH_H * title_scale;
    int16_t url_w = fallback_text_width(FALLBACK_SYNC_URL, url_scale);
    int16_t url_h = FALLBACK_GLYPH_H * url_scale;
    int16_t pad = 20;
    int16_t gap = 18;
    int16_t box_w = (title_w > url_w ? title_w : url_w) + pad * 2;
    int16_t box_h = title_h + gap + url_h + pad * 2;
    int16_t box_x = (W - box_w) / 2;
    int16_t box_y = (H - box_h) / 2;
    int16_t title_x = box_x + (box_w - title_w) / 2;
    int16_t url_x = box_x + (box_w - url_w) / 2;
    int16_t title_y = box_y + pad;
    int16_t url_y = title_y + title_h + gap;
    /* 提示屏统一用黑色：兜底渲染走全屏局刷路径(仅黑白)，红色在局刷下不显示。 */
    uint16_t title_color = GFX_BLACK;

    GFX_fillScreen(gfx, GFX_WHITE);
    GFX_fillRect(gfx, box_x, box_y, box_w, box_h, GFX_WHITE);
    GFX_drawRoundRect(gfx, box_x, box_y, box_w, box_h, 8, GFX_BLACK);
    GFX_drawRoundRect(gfx, box_x + 2, box_y + 2, box_w - 4, box_h - 4, 6, GFX_BLACK);

    fallback_draw_text(gfx, title_x, title_y, FALLBACK_SYNC_TITLE, title_color, title_scale);
    fallback_draw_text(gfx, url_x, url_y, FALLBACK_SYNC_URL, GFX_BLACK, url_scale);
    GFX_fillRect(gfx, url_x, url_y + url_h + 2, url_w, 2, GFX_BLACK);
}

void DrawResourceMissingScreen(gui_data_t* data, buffer_callback callback, void* cb_data) {
    Adafruit_GFX gfx;
    int16_t ph;

    if (data == NULL || callback == NULL) return;
    if (data->width == 0) return;

    // 与 DrawGUI 保持完全一致：ph 可能为负(如 __HEAP_SIZE=0 时为 -5)，
    // 交给 GFX_begin 的 "<=0 → max_height" 兜底。切勿钳成 1，否则三色屏
    // page_height/=2 会得到 0，触发 color==buffer 被误判为 4C 模式 + 除零，
    // 缓冲区与字节数全乱 → 屏幕黑白麻点。
    ph = (__HEAP_SIZE - 512) / (data->width / 8);

    if (data->color == 2) {
        GFX_begin_3c(&gfx, data->width, data->height, ph);
    } else {
        GFX_begin(&gfx, data->width, data->height, ph);
    }

    GFX_firstPage(&gfx);
    do {
        fallback_paint(&gfx, data->width, data->height);
    } while (GFX_nextPage(&gfx, callback, cb_data));

    GFX_end(&gfx);
}
