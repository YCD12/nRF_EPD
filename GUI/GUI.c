#include "GUI.h"

#include <string.h>

#include "EPD_driver.h"
#include "../EPD/epd_ext_resources.h"
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

#define UI_BUNDLE_MAGIC 0x314C4955UL
#define UI_BUNDLE_VERSION 1U
#define UI_BUNDLE_HEADER_SIZE 28U
#define UI_BUNDLE_MODE_SIZE 16U
#define UI_BUNDLE_COMPONENT_SIZE 24U
#define UI_CACHE_MAX_MODES 8U
#define UI_RENDER_COMPONENT_CACHE_MAX 18U

#define UI_MODE_LOW_BATTERY 250U

#define UI_TIME_SCALE_MIN 1
#define UI_TIME_SCALE_MAX 10
#define UI_TIME_DIGITS_MIN 1
#define UI_TIME_DIGITS_MAX 10

#define UI_UPDATE_MANUAL GUI_UPDATE_MANUAL
#define UI_UPDATE_DAILY GUI_UPDATE_DAILY
#define UI_UPDATE_MINUTE GUI_UPDATE_MINUTE

#define UI_FLAG_PARTIAL_ONLY 0x01U
#define UI_TEXT_FLAG_RIGHT_ALIGN 0x02U
#define UI_TEXT_FLAG_ZERO_PAD 0x04U
#define UI_TEXT_FLAG_WITH_ZODIAC 0x08U
#define UI_TEXT_FLAG_WEEK_LEADING_SPACE 0x10U

typedef enum {
    UI_COMP_WEEK_HEADER = 2,
    UI_COMP_MONTH_GRID = 3,
    UI_COMP_TIME_7SEG = 4,
    UI_COMP_BATTERY = 5,
    UI_COMP_LINE = 6,
    UI_COMP_LOW_BATTERY_TIP = 10,
    UI_COMP_TIME_SYNC_TIP = 11,
    UI_COMP_TEXT_FIELD = 12,
} ui_component_type_t;

typedef enum {
    UI_TEXT_DATE_YMD = 1,
    UI_TEXT_LUNAR_DATE = 2,
    UI_TEXT_TEMPERATURE = 3,
    UI_TEXT_LUNAR_YEAR = 4,
    UI_TEXT_WEEK_NUMBER = 5,
    UI_TEXT_JIEQI = 6,
    UI_TEXT_WEEKDAY = 7,
    UI_TEXT_TEMP_SSID = 8,
    UI_TEXT_SSID = 9,
} ui_text_data_id_t;

typedef struct {
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint8_t mode_count;
    uint8_t component_count;
    uint16_t mode_offset;
    uint16_t component_offset;
    uint16_t string_offset;
    uint16_t string_length;
} ui_bundle_header_t;

typedef struct {
    uint8_t mode_id;
    uint8_t update_policy;
    uint8_t component_start;
    uint8_t component_count;
    uint16_t partial_x;
    uint16_t partial_y;
    uint16_t partial_w;
    uint16_t partial_h;
    uint16_t flags;
} ui_mode_record_t;

typedef struct {
    uint8_t type;
    uint8_t data_id;
    uint8_t font_id;
    uint8_t flags;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t fg;
    uint16_t bg;
    int16_t p0;
    int16_t p1;
    int16_t p2;
    uint16_t text_offset;
} ui_component_record_t;

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

extern void app_feed_wdt(void);

static bool m_ui_cache_valid;
static uint16_t m_ui_cache_width;
static uint16_t m_ui_cache_height;
static ui_bundle_header_t m_ui_cached_header;
static ui_mode_record_t m_ui_cached_modes[UI_CACHE_MAX_MODES];

static bool m_ui_render_cache_valid;
static uint8_t m_ui_render_cache_mode_id;
static uint16_t m_ui_render_cache_width;
static uint16_t m_ui_render_cache_height;
static ui_mode_record_t m_ui_render_cached_mode;
static ui_component_record_t m_ui_render_cached_components[UI_RENDER_COMPONENT_CACHE_MAX];
static uint8_t m_ui_render_cached_count;
static month_render_cache_t m_month_render_cache;

static void DrawTimeSyncTip(Adafruit_GFX* gfx, gui_data_t* data);
static void DrawLowBatteryTip(Adafruit_GFX* gfx, gui_data_t* data);
static void DrawBattery(Adafruit_GFX* gfx, int16_t x, int16_t y, uint8_t iw, gui_data_t* data);
static void DrawWeekHeaderArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                               gui_data_t* data);
static void DrawMonthDaysArea(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t area_w, int16_t area_h,
                              tm_t* tm, gui_data_t* data);
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

        app_feed_wdt();
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
    char volts_text[4];
    volts_text[0] = (char)('0' + (data->voltage_mv / 1000));
    volts_text[1] = '.';
    volts_text[2] = (char)('0' + ((data->voltage_mv % 1000) / 100));
    volts_text[3] = '\0';

    uint16_t warning_color = (data->color == 2) ? GFX_RED : GFX_BLACK;
    int16_t W = (int16_t)data->width;
    int16_t H = (int16_t)data->height;

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
    const char* title_text = (data->voltage_mv < CRITICAL_BATTERY_SHUTDOWN_MV) ? "POWER OFF" : "LOW POWER";
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
static void DrawBattery(Adafruit_GFX* gfx, int16_t x, int16_t y, uint8_t iw, gui_data_t* data) {
    x -= iw;  // 从右边界开始向左绘制 | Start drawing from right edge to left
    uint8_t fill_width = (iw > 4) ? (uint8_t)(iw - 4) : 0;
    uint8_t level = BatteryPercentFromMv(data->voltage_mv);

    uint16_t color = GFX_BLACK;
    if (level <= 20) {
        if (data->color == 2) {
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
                               gui_data_t* data) {
    if (area_w <= 0) area_w = data->width - 2 * x;
    if (area_h <= 0) area_h = data->height > 300 ? 32 : 24;

    GFX_setFont(gfx, data->height > 300 ? u8g2_font_wqy12_t_lunar : u8g2_font_wqy9_t_lunar);
    uint8_t w = (uint8_t)(area_w / 7);  // 每列宽度 | Width per column
    uint8_t h = (uint8_t)area_h;        // 行高 | Row height
    uint8_t r = (uint8_t)(area_w % 7);  // 剩余像素分配给最后一列 | Remaining pixels for last column
    uint8_t fh = (h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;  // 垂直居中 | Vertical center
    char day_name[4];
    int16_t cw = GFX_getUTF8Width(gfx, LUNAR_GetDayName(0, day_name, sizeof(day_name)));
    for (int i = 0; i < 7; i++) {
        // 根据一周起始日设置调整显示顺序 | Adjust display order based on week start setting
        uint8_t day = (data->week_start + i) % 7;
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
                              tm_t* tm, gui_data_t* data) {
    uint16_t year = tm->tm_year + YEAR0;
    uint8_t month = tm->tm_mon + 1;
    const month_render_cache_t* month_cache = PrepareMonthRenderCache(year, month);

    // 获取本月1号是星期几 | Get which day of week is the 1st day of month
    uint8_t firstDayWeek = get_first_day_week(year, month);
    // 根据用户设置的一周起始日调整首日位置 | Adjust first day position based on user's week start setting
    int8_t adjustedFirstDay = (firstDayWeek - data->week_start + 7) % 7;
    // 获取本月天数 | Get number of days in this month
    uint8_t monthMaxDays = (month_cache != NULL) ? month_cache->days : thisMonthMaxDays((uint8_t)year, month);
    // 计算需要的行数 | Calculate required rows
    // 公式：1 + (总天数 - 第一周剩余天数 + 6) / 7
    uint8_t monthDayRows = 1 + (monthMaxDays - (7 - adjustedFirstDay) + 6) / 7;

    if (area_w <= 0) area_w = data->width - x - 10;
    if (area_h <= 0) area_h = data->height - y - 10;

    // 计算每个单元格的宽度和高度 | Calculate cell width and height
    int16_t bw = area_w / 7;
    int16_t bh = area_h / monthDayRows;
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
        app_feed_wdt();

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

        app_feed_wdt();
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

void GUI_MarkTimeRefreshTimestamp(uint32_t timestamp) {
    s_last_time_refresh_ts = timestamp;
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

static void ui_draw_temperature(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, gui_data_t* data, uint8_t flags) {
    if ((flags & UI_TEXT_FLAG_RIGHT_ALIGN) && w > 0) {
        x += w - ui_temperature_width(gfx, data->temperature, "℃");
    }

    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setFont(gfx, u8g2_font_helvB18_tn);
    GFX_setCursor(gfx, x, y);
    GFX_printf(gfx, "%d", data->temperature);
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
            ui_draw_temperature(gfx, x, y, comp->w, data, comp->flags);
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

static uint16_t ui_read_u16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t ui_read_i16(const uint8_t* data) {
    return (int16_t)ui_read_u16(data);
}

static uint32_t ui_read_u32(const uint8_t* data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static ret_code_t ui_bundle_read(uint32_t offset, void* data, size_t length) {
    return epd_ext_resource_read(EPD_EXT_RES_TYPE_UI, EPD_EXT_UI_BUNDLE, offset, data, length);
}

static bool ui_range_fits(uint32_t offset, uint32_t count, uint32_t item_size, uint32_t limit) {
    if (offset > limit) return false;
    if (item_size != 0 && count > ((limit - offset) / item_size)) return false;
    return true;
}

static bool ui_rect_fits_screen(int32_t x, int32_t y, int32_t w, int32_t h,
                                uint16_t screen_w, uint16_t screen_h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if ((uint32_t)x >= screen_w || (uint32_t)y >= screen_h) return false;
    if ((uint32_t)w > (uint32_t)screen_w - (uint32_t)x) return false;
    if ((uint32_t)h > (uint32_t)screen_h - (uint32_t)y) return false;
    return true;
}

static bool ui_partial_rect_valid(const ui_bundle_header_t* header, const ui_mode_record_t* mode) {
    if (mode->partial_w == 0 && mode->partial_h == 0) return true;
    if (mode->partial_w < 8 || mode->partial_h == 0) return false;
    return ui_rect_fits_screen(mode->partial_x, mode->partial_y, mode->partial_w, mode->partial_h,
                               header->width, header->height);
}

static uint16_t ui_clamp_u16(int16_t value, uint16_t min_value, uint16_t max_value) {
    if (value < (int16_t)min_value) return min_value;
    if (value > (int16_t)max_value) return max_value;
    return (uint16_t)value;
}

static bool ui_component_valid(const ui_bundle_header_t* header, const ui_component_record_t* comp) {
    if (header == NULL || comp == NULL) return false;
    if (header->string_length == 0 || comp->text_offset >= header->string_length) return false;

    switch (comp->type) {
        case UI_COMP_WEEK_HEADER:
        case UI_COMP_MONTH_GRID:
        case UI_COMP_LINE:
        case UI_COMP_LOW_BATTERY_TIP:
        case UI_COMP_TIME_SYNC_TIP:
            return ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height);

        case UI_COMP_TEXT_FIELD:
            if (!ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height)) {
                return false;
            }
            if (comp->data_id < UI_TEXT_DATE_YMD || comp->data_id > UI_TEXT_SSID) return false;
            return comp->p0 <= 0 || comp->p0 < (int16_t)header->height;

        case UI_COMP_TIME_7SEG:
            if (!ui_rect_fits_screen(comp->x, comp->y, comp->w, comp->h, header->width, header->height)) {
                return false;
            }
            return comp->p0 >= UI_TIME_SCALE_MIN && comp->p0 <= UI_TIME_SCALE_MAX &&
                   comp->p1 >= UI_TIME_DIGITS_MIN && comp->p1 <= UI_TIME_DIGITS_MAX;

        case UI_COMP_BATTERY:
            if (comp->w < 4 || comp->w > 80 || comp->h <= 0 || comp->h > 40) return false;
            if (comp->x < comp->w || comp->x + 2 > (int16_t)header->width) return false;
            return comp->y >= 0 && comp->y + 12 <= (int16_t)header->height;

        default:
            return false;
    }
}

static ret_code_t ui_read_header(ui_bundle_header_t* header, uint16_t screen_w, uint16_t screen_h) {
    uint8_t raw[UI_BUNDLE_HEADER_SIZE];
    uint32_t magic;
    epd_ext_resource_entry_t entry;
    uint32_t mode_end;
    uint32_t component_end;
    ret_code_t ret;

    if (header == NULL) return NRF_ERROR_NULL;

    ret = epd_ext_resource_get_info(EPD_EXT_RES_TYPE_UI, EPD_EXT_UI_BUNDLE, &entry);
    if (ret != NRF_SUCCESS) return ret;
    if (entry.length < UI_BUNDLE_HEADER_SIZE) return NRF_ERROR_INVALID_LENGTH;

    ret = ui_bundle_read(0, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    magic = ui_read_u32(&raw[0]);
    if (magic != UI_BUNDLE_MAGIC) return NRF_ERROR_INVALID_DATA;

    header->version = ui_read_u16(&raw[4]);
    header->width = ui_read_u16(&raw[6]);
    header->height = ui_read_u16(&raw[8]);
    header->mode_count = raw[10];
    header->component_count = raw[11];
    header->mode_offset = ui_read_u16(&raw[12]);
    header->component_offset = ui_read_u16(&raw[14]);
    header->string_offset = ui_read_u16(&raw[16]);
    header->string_length = ui_read_u16(&raw[18]);

    if (header->version != UI_BUNDLE_VERSION ||
        header->width != screen_w || header->height != screen_h ||
        header->mode_count == 0 || header->component_count == 0 ||
        header->string_length == 0 ||
        header->mode_offset < UI_BUNDLE_HEADER_SIZE ||
        header->component_offset < header->mode_offset ||
        header->string_offset < header->component_offset) {
        return NRF_ERROR_INVALID_DATA;
    }

    if (!ui_range_fits(header->mode_offset, header->mode_count, UI_BUNDLE_MODE_SIZE, entry.length) ||
        !ui_range_fits(header->component_offset, header->component_count, UI_BUNDLE_COMPONENT_SIZE, entry.length) ||
        !ui_range_fits(header->string_offset, header->string_length, 1U, entry.length)) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    mode_end = header->mode_offset + (uint32_t)header->mode_count * UI_BUNDLE_MODE_SIZE;
    component_end = header->component_offset + (uint32_t)header->component_count * UI_BUNDLE_COMPONENT_SIZE;
    if (mode_end > header->component_offset || component_end > header->string_offset) {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static ret_code_t ui_read_mode_at(const ui_bundle_header_t* header, uint8_t index, ui_mode_record_t* mode) {
    uint8_t raw[UI_BUNDLE_MODE_SIZE];
    ret_code_t ret;

    if (header == NULL || mode == NULL) return NRF_ERROR_NULL;
    if (index >= header->mode_count) return NRF_ERROR_NOT_FOUND;

    ret = ui_bundle_read(header->mode_offset + (uint32_t)index * UI_BUNDLE_MODE_SIZE, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    mode->mode_id = raw[0];
    mode->update_policy = raw[1];
    mode->component_start = raw[2];
    mode->component_count = raw[3];
    mode->partial_x = ui_read_u16(&raw[4]);
    mode->partial_y = ui_read_u16(&raw[6]);
    mode->partial_w = ui_read_u16(&raw[8]);
    mode->partial_h = ui_read_u16(&raw[10]);
    mode->flags = ui_read_u16(&raw[12]);

    if ((uint16_t)mode->component_start + mode->component_count > header->component_count) {
        return NRF_ERROR_INVALID_DATA;
    }
    if (!ui_partial_rect_valid(header, mode)) {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static ret_code_t ui_find_mode(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                               ui_bundle_header_t* header, ui_mode_record_t* mode) {
    ret_code_t ret;

    if (!m_ui_cache_valid || m_ui_cache_width != screen_w || m_ui_cache_height != screen_h) {
        ret = GUI_ReloadUICache(screen_w, screen_h);
        if (ret != NRF_SUCCESS) return ret;
    }

    if (header != NULL) {
        *header = m_ui_cached_header;
    }

    for (uint8_t i = 0; i < m_ui_cached_header.mode_count; i++) {
        if (m_ui_cached_modes[i].mode_id == mode_id) {
            if (mode != NULL) {
                *mode = m_ui_cached_modes[i];
            }
            return NRF_SUCCESS;
        }
    }

    return NRF_ERROR_NOT_FOUND;
}

ret_code_t GUI_ReloadUICache(uint16_t width, uint16_t height) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;
    ret_code_t ret = ui_read_header(&header, width, height);

    m_ui_cache_valid = false;
    m_ui_render_cache_valid = false;
    m_month_render_cache.valid = false;
    if (ret != NRF_SUCCESS) return ret;
    if (header.mode_count > UI_CACHE_MAX_MODES) {
        return NRF_ERROR_NO_MEM;
    }

    for (uint8_t i = 0; i < header.mode_count; i++) {
        ret = ui_read_mode_at(&header, i, &mode);
        if (ret != NRF_SUCCESS) return ret;
        m_ui_cached_modes[i] = mode;
        app_feed_wdt();
    }

    m_ui_cached_header = header;
    m_ui_cache_width = width;
    m_ui_cache_height = height;
    m_ui_cache_valid = true;
    return NRF_SUCCESS;
}

static ret_code_t ui_read_component(const ui_bundle_header_t* header, uint8_t index, ui_component_record_t* comp) {
    uint8_t raw[UI_BUNDLE_COMPONENT_SIZE];
    ret_code_t ret;

    if (header == NULL || comp == NULL) return NRF_ERROR_NULL;
    if (index >= header->component_count) return NRF_ERROR_NOT_FOUND;

    ret = ui_bundle_read(header->component_offset + (uint32_t)index * UI_BUNDLE_COMPONENT_SIZE, raw, sizeof(raw));
    if (ret != NRF_SUCCESS) return ret;

    comp->type = raw[0];
    comp->data_id = raw[1];
    comp->font_id = raw[2];
    comp->flags = raw[3];
    comp->x = ui_read_i16(&raw[4]);
    comp->y = ui_read_i16(&raw[6]);
    comp->w = ui_read_i16(&raw[8]);
    comp->h = ui_read_i16(&raw[10]);
    comp->fg = ui_read_u16(&raw[12]);
    comp->bg = ui_read_u16(&raw[14]);
    comp->p0 = ui_read_i16(&raw[16]);
    comp->p1 = ui_read_i16(&raw[18]);
    comp->p2 = ui_read_i16(&raw[20]);
    comp->text_offset = ui_read_u16(&raw[22]);

    if (!ui_component_valid(header, comp)) {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static ret_code_t ui_load_mode_components(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                          ui_mode_record_t* mode,
                                          ui_component_record_t* components,
                                          uint8_t max_components,
                                          uint8_t* component_count) {
    ui_bundle_header_t header;
    ret_code_t ret;

    if (mode == NULL || components == NULL || component_count == NULL) {
        return NRF_ERROR_NULL;
    }

    *component_count = 0;
    ret = ui_find_mode(mode_id, screen_w, screen_h, &header, mode);
    if (ret != NRF_SUCCESS) return ret;
    if (mode->component_count > max_components) {
        return NRF_ERROR_NO_MEM;
    }

    for (uint8_t i = 0; i < mode->component_count; i++) {
        ret = ui_read_component(&header, mode->component_start + i, &components[i]);
        if (ret != NRF_SUCCESS) return ret;
        app_feed_wdt();
    }

    *component_count = mode->component_count;
    return NRF_SUCCESS;
}

static ret_code_t ui_load_render_component_cache(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                                 ui_mode_record_t* mode,
                                                 const ui_component_record_t** components,
                                                 uint8_t* component_count) {
    ret_code_t ret;

    if (mode == NULL || components == NULL || component_count == NULL) {
        return NRF_ERROR_NULL;
    }

    if (m_ui_render_cache_valid &&
        m_ui_render_cache_mode_id == mode_id &&
        m_ui_render_cache_width == screen_w &&
        m_ui_render_cache_height == screen_h) {
        *mode = m_ui_render_cached_mode;
        *components = m_ui_render_cached_components;
        *component_count = m_ui_render_cached_count;
        return NRF_SUCCESS;
    }

    ret = ui_load_mode_components(mode_id, screen_w, screen_h,
                                  &m_ui_render_cached_mode,
                                  m_ui_render_cached_components,
                                  UI_RENDER_COMPONENT_CACHE_MAX,
                                  &m_ui_render_cached_count);
    if (ret != NRF_SUCCESS) {
        m_ui_render_cache_valid = false;
        m_ui_render_cached_count = 0;
        return ret;
    }

    m_ui_render_cache_mode_id = mode_id;
    m_ui_render_cache_width = screen_w;
    m_ui_render_cache_height = screen_h;
    m_ui_render_cache_valid = true;

    *mode = m_ui_render_cached_mode;
    *components = m_ui_render_cached_components;
    *component_count = m_ui_render_cached_count;
    return NRF_SUCCESS;
}



static void ui_render_component(Adafruit_GFX* gfx, const ui_component_record_t* comp,
                                tm_t* tm, struct Lunar_Date* lunar, gui_data_t* data,
                                bool partial_pass, int16_t origin_x, int16_t origin_y) {
    int16_t x = comp->x - origin_x;
    int16_t y = comp->y - origin_y;

    if (((comp->flags & UI_FLAG_PARTIAL_ONLY) != 0) != partial_pass) {
        return;
    }

    switch (comp->type) {
        case UI_COMP_WEEK_HEADER:
            DrawWeekHeaderArea(gfx, x, y, comp->w, comp->h, data);
            break;
        case UI_COMP_MONTH_GRID:
            DrawMonthDaysArea(gfx, x, y, comp->w, comp->h, tm, data);
            break;
        case UI_COMP_TIME_7SEG:
            DrawTime(gfx, tm, x, y,
                     ui_clamp_u16(comp->p0, UI_TIME_SCALE_MIN, UI_TIME_SCALE_MAX),
                     ui_clamp_u16(comp->p1, UI_TIME_DIGITS_MIN, UI_TIME_DIGITS_MAX));
            break;
        case UI_COMP_BATTERY:
            DrawBattery(gfx, x, y, (uint8_t)comp->w, data);
            break;
        case UI_COMP_LINE:
            GFX_fillRect(gfx, x, y, comp->w, comp->h, comp->fg);
            break;
        case UI_COMP_TEXT_FIELD:
            ui_render_text_field(gfx, comp, tm, lunar, data, x, y,
                                 comp->p0 > 0 ? (int16_t)(comp->p0 - origin_y) : (int16_t)(y + 20));
            break;
        case UI_COMP_LOW_BATTERY_TIP:
            DrawLowBatteryTip(gfx, data);
            break;
        case UI_COMP_TIME_SYNC_TIP:
            /* 触发条件：未校时(默认 2025-01)，或当前年份农历数据缺失/超出 LCDB 范围
             * (诚实降级)。后者复用同一提示框，因两种情况的修复手段都是联网同步/更新。 */
            if (((tm->tm_year + YEAR0 == 2025) && (tm->tm_mon + 1 == 1)) ||
                !lunar_db_year_supported((uint16_t)(tm->tm_year + YEAR0))) {
                DrawTimeSyncTip(gfx, data);
            }
            break;
        default:
            break;
    }
}

static void ui_render_component_list(Adafruit_GFX* gfx, const ui_component_record_t* components,
                                     uint8_t component_count, tm_t* tm,
                                     struct Lunar_Date* lunar, gui_data_t* data,
                                     bool partial_pass, int16_t origin_x, int16_t origin_y) {
    for (uint8_t i = 0; i < component_count; i++) {
        ui_render_component(gfx, &components[i], tm, lunar, data, partial_pass, origin_x, origin_y);
        app_feed_wdt();
    }
}

static bool ui_get_mode_partial_rect(uint8_t mode_id, uint16_t screen_w, uint16_t screen_h,
                                     uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;

    if (ui_find_mode(mode_id, screen_w, screen_h, &header, &mode) != NRF_SUCCESS) {
        return false;
    }
    if (mode.partial_w == 0 || mode.partial_h == 0) {
        return false;
    }

    if (x) *x = mode.partial_x;
    if (y) *y = mode.partial_y;
    if (w) *w = mode.partial_w;
    if (h) *h = mode.partial_h;
    return true;
}

bool GUI_ModeExists(uint8_t mode_id, uint16_t width, uint16_t height) {
    ui_bundle_header_t header;
    ui_mode_record_t mode;
    return ui_find_mode(mode_id, width, height, &header, &mode) == NRF_SUCCESS;
}

uint8_t GUI_GetModeUpdatePolicy(uint8_t mode_id, uint16_t width, uint16_t height) {
    if (!m_ui_cache_valid || m_ui_cache_width != width || m_ui_cache_height != height) {
        return UI_UPDATE_MANUAL;
    }

    for (uint8_t i = 0; i < m_ui_cached_header.mode_count; i++) {
        if (m_ui_cached_modes[i].mode_id == mode_id) {
            return m_ui_cached_modes[i].update_policy;
        }
    }

    return UI_UPDATE_MANUAL;
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
    bool low_battery = (data->voltage_mv <= LOW_BATTERY_THRESHOLD_MV);
    uint8_t render_mode_id = low_battery ? UI_MODE_LOW_BATTERY : (uint8_t)data->mode;
    ui_mode_record_t cached_mode;
    const ui_component_record_t* cached_components = NULL;
    uint8_t cached_component_count = 0;
    ret_code_t ui_ret;

    if (!low_battery) {
        LUNAR_SolarToLunar(&Lunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
    }
    ui_ret = ui_load_render_component_cache(render_mode_id, data->width, data->height,
                                            &cached_mode, &cached_components,
                                            &cached_component_count);
    if (ui_ret != NRF_SUCCESS) {
        cached_components = NULL;
        cached_component_count = 0;
    }


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

        if (low_battery) {
            ui_render_component_list(&gfx, cached_components, cached_component_count,
                                     &tm, &Lunar, data, false, 0, 0);
            continue;
        }

        ui_render_component_list(&gfx, cached_components, cached_component_count,
                                 &tm, &Lunar, data, false, 0, 0);
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
   * 1. 默认刷新整个HH:MM时间显示区域(首次/时间大跳变)
   * 2. 正常分钟更新时,固定刷新完整MM两位数字
   * 3. 这样避免分钟十位旧段漏刷,同时仍比刷新完整HH:MM面积小
   *
   * 刷新区域动态调整(Dynamic refresh area adjustment)：
   * - 完整时间区域: 约272x128像素
   * - 正常分钟区域: 约148x128像素
   * - 小时变化/时间跳变: 刷新完整HH:MM
   */
  void GetTimeRefreshArea(uint32_t timestamp, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
      // Base time area (centered HH:MM inside TIME_REFRESH_*)
      // 基础时间区域(TIME_REFRESH_*内居中的HH:MM)
      uint16_t base_x = TIME_REFRESH_X;
      uint16_t base_y = TIME_REFRESH_Y;
      uint16_t base_w = TIME_REFRESH_WIDTH;
      uint16_t base_h = TIME_REFRESH_HEIGHT;

      (void)ui_get_mode_partial_rect(MODE_CLOCK_CALENDAR, 800, 480, &base_x, &base_y, &base_w, &base_h);

      // 7-segment layout parameters (must match DrawTime/Draw7Number)
      // 7段数码管布局参数(必须与DrawTime/Draw7Number匹配)
      const uint16_t cS = 6;      // scale factor | 缩放因子
      const uint16_t nD = 2;      // digits per number (HH / MM) | 每个数字的位数
      const uint16_t digit_advance = 11 * cS + 2;  // Draw7Number digit stride | 数字步进
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
    const int16_t margin_y = 8;
    // Expand upwards if there is room | 如果有空间则向上扩展
    if (window_y_local > margin_y) {
        window_y_local -= margin_y;
        window_h += margin_y;
    }
    // Expand downwards if there is room | 如果有空间则向下扩展
    if (window_y_local + window_h + margin_y <= (int16_t)base_h) {
        window_h += margin_y;
    }

      // Normal minute updates always refresh the full "MM" block. Refreshing
      // only the changed digit can leave stale segments in the minute-tens
      // digit, and later minute-ones updates will never touch that area.
      // 正常分钟更新固定刷新完整"MM"两位。只刷变化数字会漏掉分钟十位残留段。
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

          uint16_t minute_start = window_x_local + digit_block_width + 6 * cS;
          const uint16_t digit_margin_x = cS;
          bool hour_unchanged = (prev_ht == curr_ht) && (prev_hu == curr_hu);
          bool minute_changed = (prev_mt != curr_mt) || (prev_mu != curr_mu);

          if (hour_unchanged && minute_changed) {
              uint16_t left = (minute_start > digit_margin_x) ?
                              (uint16_t)(minute_start - digit_margin_x) : minute_start;
              uint16_t right = (uint16_t)(minute_start + (2 * digit_advance) + digit_margin_x);
              if (right > base_w) {
                  right = base_w;
              }

              window_x_local = left;
              window_w = (uint16_t)(right - left);
          }
      }

      // Persist the chosen window for DrawGUI_ClockOnly (GFX_setWindow)
      // 保存选定的窗口供DrawGUI_ClockOnly使用(GFX_setWindow)
      s_last_time_refresh_ts = timestamp;

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
    uint16_t base_x = TIME_REFRESH_X;
    uint16_t base_y = TIME_REFRESH_Y;
    uint16_t base_w = TIME_REFRESH_WIDTH;
    uint16_t base_h = TIME_REFRESH_HEIGHT;
    (void)ui_get_mode_partial_rect(MODE_CLOCK_CALENDAR, 800, 480, &base_x, &base_y, &base_w, &base_h);
    if (x) *x = base_x;
    if (y) *y = base_y;
}

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
#define FALLBACK_FONT_FIRST  0x20
#define FALLBACK_FONT_LAST   0x7E
#define FALLBACK_GLYPH_W     5
#define FALLBACK_GLYPH_H     7
#define FALLBACK_GLYPH_ADV   6  /* 5 columns + 1 column spacing */

/* Public-domain 5x7 ASCII font (Adafruit GFX classic), columns LSB-on-top,
 * ASCII 0x20..0x7E = 95 glyphs x 5 bytes = 475 bytes ROM. */
static const uint8_t fallback_font[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x01, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x32},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x04, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x02, 0x01, 0x02, 0x04, 0x02},
};

static void fallback_draw_char(Adafruit_GFX* gfx, int16_t x, int16_t y,
                               char ch, uint16_t color, uint8_t scale) {
    uint8_t code = (uint8_t)ch;
    const uint8_t* glyph;
    if (code < FALLBACK_FONT_FIRST || code > FALLBACK_FONT_LAST) code = '?';
    glyph = fallback_font[code - FALLBACK_FONT_FIRST];
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

static void fallback_paint(Adafruit_GFX* gfx, gui_data_t* data) {
    int16_t W = (int16_t)data->width;
    int16_t H = (int16_t)data->height;
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
    } else if (data->color == 3) {
        GFX_begin_4c(&gfx, data->width, data->height, ph);
    } else {
        GFX_begin(&gfx, data->width, data->height, ph);
    }

    GFX_firstPage(&gfx);
    do {
        fallback_paint(&gfx, data);
    } while (GFX_nextPage(&gfx, callback, cb_data));

    GFX_end(&gfx);
}



