#include "GUI.h"

#include <stddef.h>

#include "Lunar.h"
#include "calendar_grid.h"
#include "calendar_annotations.h"
#include "clock_layout.h"
#include "clock_text.h"
#include "fonts.h"

// 计算数组元素个数的宏定义
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#if defined(GUI_FIXED_PANEL_WIDTH) && GUI_FIXED_PANEL_WIDTH > 0 && \
    defined(GUI_FIXED_PANEL_HEIGHT) && GUI_FIXED_PANEL_HEIGHT > 0
#define GUI_WIDTH(data)  ((void)(data), (int16_t)GUI_FIXED_PANEL_WIDTH)
#define GUI_HEIGHT(data) ((void)(data), (int16_t)GUI_FIXED_PANEL_HEIGHT)
#else
#define GUI_WIDTH(data)  ((data)->width)
#define GUI_HEIGHT(data) ((data)->height)
#endif

#if !GUI_USE_EXTERNAL_FONTS && !GUI_INCLUDE_INTERNAL_FONTS
#error "At least one GUI font source must be enabled"
#endif

/* Conservative vertical clipping for the unrotated built-in layouts. */
static bool gui_page_hits(const Adafruit_GFX *gfx, int32_t top, int32_t bottom)
{
    int32_t y = gfx->py + (int32_t)gfx->current_page * gfx->page_height;
    return gfx->rotation != GFX_ROTATE_0 || (top < y + gfx->page_height && bottom > y);
}

static u8g2_font_placeholder_t gui_font_placeholder(gui_font_id_t font_id)
{
    const gui_font_metrics_t* metrics = GUI_GetFontMetrics(font_id);
    return metrics != NULL ? *metrics : (u8g2_font_placeholder_t){0};
}

/* Select one of the three fixed font slots.  External fonts keep a fixed
 * placeholder geometry so a deleted or incomplete glyph never collapses the
 * layout. */
static void gui_set_font(Adafruit_GFX* gfx, gui_font_id_t font_id)
{
    u8g2_font_placeholder_t placeholder = gui_font_placeholder(font_id);
#if GUI_USE_EXTERNAL_FONTS
    const u8g2_font_source_t* source = GUI_GetExternalFont(font_id);
    if (source != NULL) {
        GFX_setExternalFont(gfx, source);
        GFX_setFontPlaceholder(gfx, &placeholder);
        return;
    }
#endif

#if !GUI_USE_EXTERNAL_FONTS && GUI_INCLUDE_INTERNAL_FONTS
    switch (font_id) {
        case GUI_FONT_WQY9:
            GFX_setFont(gfx, u8g2_font_wqy9_t_lunar);
            break;
        case GUI_FONT_WQY12:
            GFX_setFont(gfx, u8g2_font_wqy12_t_lunar);
            break;
        case GUI_FONT_HELVB18:
            GFX_setFont(gfx, u8g2_font_helvB18_tn);
            break;
        default:
            break;
    }
#else
    GFX_setMissingFont(gfx, &placeholder);
#endif
}

static size_t gui_format_int(char *out, size_t out_size, int value, uint8_t min_digits)
{
    char reversed[10];
    size_t digits = 0U;
    size_t pos = 0U;
    bool negative = value < 0;
    unsigned magnitude = negative ? (unsigned)(-(value + 1)) + 1U : (unsigned)value;

    do {
        reversed[digits++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U && digits < sizeof(reversed));
    while (digits < min_digits && digits < sizeof(reversed)) reversed[digits++] = '0';
    if (negative && pos + 1U < out_size) out[pos++] = '-';
    while (digits > 0U && pos + 1U < out_size) out[pos++] = reversed[--digits];
    if (out_size > 0U) out[pos] = '\0';
    return pos;
}

static void gui_print_int(Adafruit_GFX *gfx, int value, uint8_t min_digits)
{
    char text[12];
    size_t length = gui_format_int(text, sizeof(text), value, min_digits);
    GFX_write(gfx, text, length);
}

static void gui_set_text_style(Adafruit_GFX *gfx, uint16_t fg, uint16_t bg,
                               gui_font_id_t font_id)
{
    GFX_setTextColor(gfx, fg, bg);
    gui_set_font(gfx, font_id);
}

/* Calendar and clock dates share the same numeric/unit font pair. */
static void gui_print_date_part(Adafruit_GFX *gfx, int value, uint8_t min_digits,
                                uint16_t color, const char *unit)
{
    gui_set_text_style(gfx, color, GFX_WHITE, GUI_FONT_HELVB18);
    gui_print_int(gfx, value, min_digits);
    gui_set_text_style(gfx, GFX_BLACK, GFX_WHITE, GUI_FONT_WQY12);
    GFX_puts(gfx, unit);
}

// 节日数据结构体
typedef struct {
    uint8_t month;  // 月份
    uint8_t day;  // 日期
    char name[10];  // 节日名称(3x3+1) 最多3个汉字
} Festival;

/* 拼「闰」+月份名，最多 9 字节。 */
static void format_lunar_month(char* out, const char* prefix, const char* month) {
    size_t pos = 0;

    while (*prefix != '\0' && pos < 9U) out[pos++] = *prefix++;
    while (*month != '\0' && pos < 9U) out[pos++] = *month++;
    out[pos] = '\0';
}

static void gui_print_lunar_md(Adafruit_GFX *gfx, const struct Lunar_Date *lunar)
{
    char text[16];
    clock_format_lunar_md(text, sizeof(text), Lunar_MonthLeapString[lunar->IsLeap],
                          Lunar_MonthString[lunar->Month], Lunar_DateString[lunar->Date]);
    GFX_puts(gfx, text);
}

static void gui_print_lunar_year(Adafruit_GFX *gfx, const struct Lunar_Date *lunar)
{
    GFX_puts(gfx, Lunar_StemStrig[LUNAR_GetStem(lunar)]);
    GFX_puts(gfx, Lunar_BranchStrig[LUNAR_GetBranch(lunar)]);
}

// 公历节日数据表
static const Festival festivals[] = {
    {1, 1, "元旦节"},  {2, 14, "情人节"}, {3, 8, "妇女节"},  {3, 12, "植树节"},  {4, 1, "愚人节"},
    {5, 1, "劳动节"},  {5, 4, "青年节"},  {6, 1, "儿童节"},  {7, 1, "建党节"},   {8, 1, "建军节"},
    {9, 10, "教师节"}, {10, 1, "国庆节"}, {11, 1, "万圣节"}, {12, 24, "平安夜"}, {12, 25, "圣诞节"},
};

// 农历节日数据表
static const Festival festivals_lunar[] = {
    {1, 1, "春节"},    {1, 15, "元宵节"}, {2, 2, "龙抬头"},  {5, 5, "端午节"},  {7, 7, "七夕节"}, {7, 15, "中元节"},
    {8, 15, "中秋节"}, {9, 9, "重阳节"},  {10, 1, "寒衣节"}, {12, 8, "腊八节"}, {12, 30, "除夕"},
};

// 放假和调休数据，每年更新
// 数据格式：0xWMMDD，其中W为工作日标志(1表示需要上班，0表示放假)，MM为月份，DD为日期
#define HOLIDAY_YEAR 2026
static const uint16_t holidays[] = {
    0x0101, 0x0102, 0x0103, 0x1104, 0x120E, 0x020F, 0x0210, 0x0211, 0x0212, 0x0213, 0x0214, 0x0215, 0x0216,
    0x0217, 0x121C, 0x0404, 0x0405, 0x0406, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x1509, 0x0613, 0x0614,
    0x0615, 0x0919, 0x091A, 0x091B, 0x1914, 0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x1A0A,
};

/**
 * @brief 查询指定日期是否为法定节假日或调休工作日
 *
 * @param[in] mon  月份(1-12)
 * @param[in] day  日期(1-31)
 * @param work 输出参数：true表示调休需上班，false表示放假
 * @return true 该日期在节假日表中
 * @return false 该日期为普通工作日
 */
static bool GetHoliday(uint8_t mon, uint8_t day, bool* work) {
    for (uint8_t i = 0; i < ARRAY_SIZE(holidays); i++) {
        // 提取月份：从高8位取低4位
        // 提取日期：取低8位
        if (((holidays[i] >> 8) & 0xF) == mon && (holidays[i] & 0xFF) == day) {
            // 提取工作日标志：取最高4位
            *work = ((holidays[i] >> 12) & 0xF) > 0;
            return true;
        }
    }
    return false;
}

/**
 * @brief 获取指定日期的节日名称
 *
 * @param[in] year  公历年份
 * @param[in] mon  公历月份(1-12)
 * @param[in] day  公历日期(1-31)
 * @param[in] week  星期几(0=周日, 6=周六)
 * @param[in] Lunar  农历日期结构体指针
 * @param festival 输出缓冲区，用于存储节日名称
 * @return true 找到节日
 * @return false 无节日
 *
 * 查询优先级：农历节日 → 除夕/母亲节/父亲节/感恩节 → 公历节日 → 二十四节气
 */
static bool GetFestival(uint16_t year, uint8_t mon, uint8_t day, uint8_t week, const struct Lunar_Date* Lunar,
                        char* festival) {
    // 农历节日
    for (uint8_t i = 0; i < ARRAY_SIZE(festivals_lunar); i++) {
        if (Lunar->Month == festivals_lunar[i].month && Lunar->Date == festivals_lunar[i].day) {
            strcpy(festival, festivals_lunar[i].name);
            return true;
        }
    }

    // 除夕：春节前一天（12/29 或 12/30），12/30 已在上面判断
    // 需要判断次日是否为正月初一，因为农历大小月不固定
    if (Lunar->Month == 12 && Lunar->Date == 29) {
        struct Lunar_Date nextLunar;
        struct devtm tm = {year, mon, day, 0, 0, 0, week};
        // 计算次日的农历日期
        transformTime(transformTimeStruct(&tm) + 86400, &tm);  // +86400秒(1天)
        LUNAR_SolarToLunar(&nextLunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
        if (nextLunar.Month == 1 && nextLunar.Date == 1) {
            strcpy(festival, "除夕");
            return true;
        }
    }
    // 母亲节: 五月第二个星期日
    if (mon == 5 && week == 0 && day >= 8 && day <= 14) {
        strcpy(festival, "母亲节");
        return true;
    }
    // 父亲节: 六月第三个星期日
    if (mon == 6 && week == 0 && day >= 15 && day <= 21) {
        strcpy(festival, "父亲节");
        return true;
    }
    // 感恩节：十一月第四个星期四
    if (mon == 11 && week == 4 && day >= 22 && day <= 28) {
        strcpy(festival, "感恩节");
        return true;
    }

    // 公历节日
    for (uint8_t i = 0; i < ARRAY_SIZE(festivals); i++) {
        if (mon == festivals[i].month && day == festivals[i].day) {
            strcpy(festival, festivals[i].name);
            return true;
        }
    }

    // 二十四节气
    // 节气是中国传统历法的重要组成部分，用于指导农业生产
    uint8_t JQdate;
    if (GetJieQi(year, mon, day, &JQdate) && JQdate == day) {
        // 计算节气索引：每月2个节气，day>=15则为第二个节气
        uint8_t JQ = (mon - 1) * 2;
        if (day >= 15) JQ++;
        strcpy(festival, JieQiStr[JQ]);
        if (JQ == 6)  // 清明
            // 清明既是节气又是节日，需要加"节"字
            strcat(festival, "节");

        return true;
    }

    return false;
}

/**
 * @brief 绘制时间同步提示框
 *
 * @param gfx  图形上下文
 * @param data GUI数据
 *
 * 系统时间未同步时(如 2025 年 1 月)在屏幕中央提示用户访问网页校时。
 */
static void DrawTimeSyncTip(Adafruit_GFX* gfx, const gui_data_t* data) {
    const char* title = "SYNC TIME!";
    const char* url = "https://ycd12.github.io/EPD-nRF5_DYC/";

    gui_set_font(gfx, GUI_FONT_WQY9);

    // 计算提示框尺寸和位置
    int16_t fh = GFX_getFontHeight(gfx);
    int16_t box_w = GFX_getUTF8Width(gfx, url) + 20;  // URL宽度+边距
    int16_t box_h = fh * 2 + 20;  // 两行文字+边距
    int16_t box_x = (GUI_WIDTH(data) - box_w) / 2;  // 水平居中
    int16_t box_y = GUI_HEIGHT(data) / 2 - box_h / 2;  // 垂直居中

    // 绘制白色背景框
    GFX_fillRect(gfx, box_x, box_y, box_w, box_h, GFX_WHITE);
    // 绘制圆角边框
    GFX_drawRoundRect(gfx, box_x, box_y, box_w, box_h, 5, GFX_BLACK);
    // 绘制红色标题
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_setCursor(gfx, box_x + (box_w - GFX_getUTF8Width(gfx, title)) / 2, box_y + 5 + fh);
    GFX_puts(gfx, title);
    // 绘制黑色URL
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, box_x + 10, box_y + box_h - GFX_getFontAscent(gfx));
    GFX_puts(gfx, url);
}

/* 右对齐绘制「x.xV」。 */
static void DrawVoltage(Adafruit_GFX* gfx, int16_t right_x, int16_t y, uint16_t voltage_mv) {
    char voltage[8];

    clock_format_voltage(voltage, sizeof(voltage), voltage_mv);
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_setCursor(gfx, right_x - GFX_getUTF8Width(gfx, voltage), y + 9);
    GFX_puts(gfx, voltage);
}

/**
 * @brief 用纯整数计算 ISO 8601 周数
 *
 * @param year  自 1900 年起的年份（同 tm_year）
 * @param mon   月份（0-11）
 * @param mday  日（1-31）
 * @param wday  星期（0=周日）
 * @return 周数（1-53）
 *
 * 第 1 周含当年第一个星期四；周一为一周之始。
 * 12 月底可能落入下年第 1 周，1 月初可能落入上年 52/53 周。
 */
static uint8_t GetWeekOfYear(uint8_t year, uint8_t mon, uint8_t mday, uint8_t wday) {
    // 将参数转换为实际年份和1基月份
    uint16_t actual_year = (uint16_t)year + YEAR0;
    uint8_t month = mon + 1;  // 将0-11转换为1-12

    // 每月天数(非闰年)
    static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // 检查是否为闰年：能被4整除且不能被100整除，或能被400整除
    uint8_t is_leap = ((actual_year % 4 == 0) && (actual_year % 100 != 0)) || (actual_year % 400 == 0);

    // 计算当前是一年中的第几天(1-366)
    uint16_t day_of_year = mday;
    for (uint8_t i = 0; i < month - 1; i++) {
        day_of_year += days_in_month[i];
        if (i == 1 && is_leap) {  // 闰年2月
            day_of_year += 1;
        }
    }

    // 将"周日=0"系统转换为"周一=0"系统用于ISO周计算
    uint8_t iso_wday = (wday + 6) % 7;

    // 使用 ISO 8601：以本周四在年中的位置定周数
    // 找到当前周的星期四(星期四决定周数)
    // 距周一的天数: iso_wday，距星期四的天数: 3 - iso_wday
    int16_t thursday_day_of_year = (int16_t)day_of_year + (3 - (int16_t)iso_wday);

    // 处理跨年边界情况
    if (thursday_day_of_year < 1) {
        // 当前周属于上一年，需要计算上一年的最后一周
        uint16_t prev_year = actual_year - 1;
        uint8_t prev_is_leap = ((prev_year % 4 == 0) && (prev_year % 100 != 0)) || (prev_year % 400 == 0);
        uint16_t prev_year_days = prev_is_leap ? 366 : 365;
        thursday_day_of_year += prev_year_days;
        // 返回上一年最后一周的周数(52或53)
        return (thursday_day_of_year - 1) / 7 + 1;
    }

    uint16_t year_days = is_leap ? 366 : 365;
    if (thursday_day_of_year > year_days) {
        // 当前周属于下一年，返回第1周
        return 1;
    }

    // 正常情况：计算周数
    return (thursday_day_of_year - 1) / 7 + 1;
}

/**
 * @brief 绘制日期头部（MODE_CALENDAR）
 *
 * @param gfx    图形上下文
 * @param x      起始 X
 * @param y      起始 Y
 * @param tm     公历时间
 * @param Lunar  农历日期
 * @param data   GUI 数据
 *
 * 第1行：年月 + 农历日期 + ISO 周数；第2行：干支年 + 生肖；右上角：电压与名称。
 */
static void DrawDateHeader(Adafruit_GFX* gfx, int16_t x, int16_t y, const tm_t* tm,
                           const struct Lunar_Date* Lunar, const gui_data_t* data) {
    // 绘制公历年月(大字体)
    GFX_setCursor(gfx, x, y - 2);
    gui_print_date_part(gfx, tm->tm_year + YEAR0, 1U, GFX_RED, "年");
    gui_print_date_part(gfx, tm->tm_mon + 1, 1U, GFX_RED, "月");

    int16_t tx = gfx->tx;  // 记录当前文字位置
    int16_t ty = y;

    // 绘制农历日期和周数
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_setCursor(gfx, tx, ty);
    if (Lunar->IsLeap) GFX_puts(gfx, " ");  // 闰月前加空格对齐
    gui_print_lunar_md(gfx, Lunar);
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_puts(gfx, " [");
    gui_print_int(gfx, GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday), 1U);
    GFX_puts(gfx, "周]");

    // 绘制农历干支年和生肖
    GFX_setCursor(gfx, tx, ty - 14);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_puts(gfx, " ");
    gui_print_lunar_year(gfx, Lunar);
    GFX_puts(gfx, "年");
    GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
    GFX_puts(gfx, " [");
    GFX_puts(gfx, Lunar_ZodiacString[LUNAR_GetZodiac(Lunar)]);
    GFX_puts(gfx, "]");

    // 绘制右侧信息：电压和WiFi名称
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    DrawVoltage(gfx, GUI_WIDTH(data) - 10 - 2, GUI_HEIGHT(data) > GUI_LARGE_SCREEN_H ? 16 : 6, data->voltage_mv);
    GFX_setCursor(gfx, GUI_WIDTH(data) - GFX_getUTF8Width(gfx, data->ssid) - 10, y);
    GFX_puts(gfx, data->ssid);
}

/**
 * @brief 绘制星期栏
 *
 * @param gfx         图形上下文
 * @param x           起始 X
 * @param y           起始 Y
 * @param span_width  总宽度（全屏 width-20，分屏右侧 width-10）
 * @param data        含一周起始日
 *
 * 周末（六/日）红底。
 */
static void DrawWeekHeader(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t span_width,
                           const gui_data_t* data) {
    bool large = GUI_HEIGHT(data) > GUI_LARGE_SCREEN_H;
    calendar_week_header_metrics_t hdr = calendar_week_header_metrics(span_width, large);

    gui_set_font(gfx, large ? GUI_FONT_WQY12 : GUI_FONT_WQY9);
    uint8_t fh = (hdr.row_h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;
    int16_t cw = GFX_getUTF8Width(gfx, Lunar_DayString[0]);
    for (int i = 0; i < 7; i++) {
        uint8_t day = (data->week_start + i) % 7;
        uint16_t bg = (day == 0 || day == 6) ? GFX_RED : GFX_BLACK;
        GFX_fillRect(gfx, x + i * hdr.col_w, y, i == 6 ? (hdr.col_w + hdr.remainder) : hdr.col_w, hdr.row_h, bg);
        GFX_setTextColor(gfx, GFX_WHITE, bg);
        GFX_setCursor(gfx, x + (hdr.col_w - cw) / 2 + i * hdr.col_w, y + fh);
        GFX_puts(gfx, Lunar_DayString[day]);
    }
}

static gui_font_id_t annotation_wqy_font(uint8_t pixel)
{
    return pixel == CALENDAR_ANNOTATION_FONT_LARGE ? GUI_FONT_WQY12 : GUI_FONT_WQY9;
}

static void annotation_prepare_style(const calendar_annotation_t* anno, bool large,
                                     uint8_t* pixel, uint16_t* fg)
{
    *pixel = large ? CALENDAR_ANNOTATION_FONT_LARGE : CALENDAR_ANNOTATION_FONT_SMALL;
    *fg = GFX_BLACK;
    if (anno->font_size == CALENDAR_ANNOTATION_FONT_SMALL ||
        anno->font_size == CALENDAR_ANNOTATION_FONT_LARGE) {
        *pixel = anno->font_size;
    }
    if (calendar_annotation_label_has_ascii_letter(anno->label)) {
        *pixel = CALENDAR_ANNOTATION_FONT_SMALL;
    }
    if (anno->color == CALENDAR_ANNOTATION_COLOR_WHITE) {
        *fg = GFX_WHITE;
    } else if (anno->color == CALENDAR_ANNOTATION_COLOR_RED) {
        *fg = GFX_RED;
    }
}

/**
 * @brief 在给定矩形内绘制月历格（全屏与分屏共用）
 */
static void DrawMonthGrid(Adafruit_GFX* gfx, const tm_t* tm, const gui_data_t* data,
                          calendar_grid_rect_t rect) {
    calendar_grid_input_t in = {
        .first_day_week = get_first_day_week(tm->tm_year + YEAR0, tm->tm_mon + 1),
        .week_start = data->week_start,
        .month_max_days = thisMonthMaxDays(tm->tm_year + YEAR0, tm->tm_mon + 1),
        .screen_height = GUI_HEIGHT(data),
    };
    calendar_grid_metrics_t m = calendar_grid_compute(&rect, &in);
    struct Lunar_Date dayLunar;

    if (m.large) {
        for (uint8_t i = 1; i < m.month_day_rows; i++)
            GFX_drawDottedLine(gfx, rect.x, rect.y + i * m.cell_h, rect.x + 7 * m.cell_w - 1,
                               rect.y + i * m.cell_h, GFX_BLACK, 1, 5);
        for (uint8_t i = 1; i < 7; i++)
            GFX_drawDottedLine(gfx, rect.x + i * m.cell_w, rect.y, rect.x + i * m.cell_w,
                               rect.y + m.month_day_rows * m.cell_h - 1, GFX_BLACK, 1, 5);
    }

    for (uint8_t i = 0; i < m.month_max_days; i++) {
        uint16_t year = tm->tm_year + YEAR0;
        uint8_t month = tm->tm_mon + 1;
        uint8_t day = i + 1;
        calendar_cell_pos_t cell = calendar_grid_cell(&rect, &m, i);

        /* Include overhanging date circles, labels and holiday corner marks.
         * Drawing order within every intersecting page remains unchanged. */
        int16_t cell_y;
        calendar_grid_cell_origin(&rect, &m, i, NULL, &cell_y);
        if (!gui_page_hits(gfx, cell_y - 32, cell_y + m.cell_h + 32)) {
            continue;
        }

        LUNAR_SolarToLunar(&dayLunar, year, month, day);

        if (day == tm->tm_mday) {
            GFX_fillCircle(gfx, cell.bx + cell.cr, cell.by + cell.cr - 3, 2 * cell.cr, GFX_RED);
            GFX_setTextColor(gfx, GFX_WHITE, GFX_RED);
        } else {
            GFX_setTextColor(gfx, cell.weekend ? GFX_RED : GFX_BLACK, GFX_WHITE);
        }

        gui_set_font(gfx, GUI_FONT_HELVB18);
        char day_text[3];
        size_t day_length = gui_format_int(day_text, sizeof(day_text), day, 1U);
        GFX_setCursor(gfx, cell.bx + (2 * cell.cr - GFX_getUTF8Width(gfx, day_text)) / 2,
                      cell.by - (cell.cr - GFX_getFontHeight(gfx)));
        GFX_write(gfx, day_text, day_length);

        char festival_buf[10] = {0};
        const char* festival;
        calendar_annotation_t anno;
        bool has_anno = calendar_annotations_get(year, month, day, &anno);

        bool replace_below = has_anno && calendar_annotation_replaces_label(&anno);
        uint16_t anno_fg = GFX_BLACK;
        uint8_t anno_pixel = m.large ? CALENDAR_ANNOTATION_FONT_LARGE : CALENDAR_ANNOTATION_FONT_SMALL;

        if (has_anno && calendar_annotation_has_text(&anno)) {
            annotation_prepare_style(&anno, m.large, &anno_pixel, &anno_fg);
        }

        GFX_setFontMode(gfx, 1);
        if (GetFestival(year, month, day, (uint8_t)cell.actual_week, &dayLunar, festival_buf)) {
            festival = festival_buf;
            if (day != tm->tm_mday) GFX_setTextColor(gfx, GFX_RED, GFX_WHITE);
        } else if (dayLunar.Date == 1) {
            format_lunar_month(festival_buf, Lunar_MonthLeapString[dayLunar.IsLeap],
                               Lunar_MonthString[dayLunar.Month]);
            festival = festival_buf;
        } else {
            festival = Lunar_DateString[dayLunar.Date];
        }
        if (replace_below) {
            festival = anno.label;
            gui_set_font(gfx, annotation_wqy_font(anno_pixel));
            GFX_setTextColor(gfx, anno_fg, (day == tm->tm_mday) ? GFX_RED : GFX_WHITE);
        } else {
            gui_set_font(gfx, annotation_wqy_font(m.large ? CALENDAR_ANNOTATION_FONT_LARGE
                                                           : CALENDAR_ANNOTATION_FONT_SMALL));
        }
        GFX_setCursor(gfx, cell.bx + (2 * cell.cr - GFX_getUTF8Width(gfx, festival)) / 2,
                      gfx->ty + GFX_getFontHeight(gfx) + 3);
        GFX_puts(gfx, festival);

        if (has_anno && calendar_annotation_draws_corner(&anno)) {
            int16_t mark_x;
            int16_t mark_y;

            gui_set_font(gfx, annotation_wqy_font(anno_pixel));
            GFX_setTextColor(gfx, anno_fg, GFX_WHITE);
            calendar_annotation_corner_cursor(&rect, &m, i, GFX_getUTF8Width(gfx, anno.label),
                                              GFX_getFontAscent(gfx), &mark_x, &mark_y);
            GFX_setCursor(gfx, mark_x, mark_y);
            GFX_puts(gfx, anno.label);
        } else if (has_anno && calendar_annotation_draws_dot(&anno)) {
            int16_t mark_x;
            int16_t mark_y;
            uint8_t mark_r;

            calendar_annotation_dot_center(&rect, &m, i, &mark_x, &mark_y, &mark_r);
            GFX_fillCircle(gfx, mark_x, mark_y, mark_r, GFX_RED);
        }

        bool work = false;
        if (year == HOLIDAY_YEAR && GetHoliday(month, day, &work)) {
            calendar_holiday_mark_t holiday =
                calendar_holiday_mark(&rect, &m, &cell, i,
                                      has_anno && calendar_annotation_draws_corner(&anno));

            if (day == tm->tm_mday) {
                GFX_fillCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r,
                               GFX_WHITE);
                GFX_drawCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r,
                               GFX_RED);
            }
            gui_set_font(gfx, GUI_FONT_WQY9);
            GFX_setTextColor(gfx, work ? GFX_BLACK : GFX_RED, GFX_WHITE);
            GFX_setCursor(gfx, holiday.text_x, holiday.text_y);
            GFX_puts(gfx, work ? "班" : "休");
        }
    }
}

/**
 * @brief 绘制完整日历（MODE_CALENDAR）
 *
 * @param gfx    图形上下文
 * @param tm     公历时间
 * @param Lunar  农历日期
 * @param data   GUI 数据
 */
static void DrawCalendar(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                         const gui_data_t* data) {
    bool large = GUI_HEIGHT(data) > GUI_LARGE_SCREEN_H;
    calendar_grid_rect_t days = {
        .x = 10,
        .y = large ? 84 : 64,
        .width = (int16_t)(GUI_WIDTH(data) - 20),
        .bottom = (int16_t)(GUI_HEIGHT(data) - 10),
    };

    DrawDateHeader(gfx, 10, large ? 38 : 28, tm, Lunar, data);
    DrawWeekHeader(gfx, 10, large ? 44 : 32, days.width, data);
    DrawMonthGrid(gfx, tm, data, days);
}

/* 七段字模和段位几何放在 Flash，避免每次绘制时在栈上创建数组。
 * 当前时钟只显示非负的 HH/MM 数值，不保留负号和空白字模。 */
typedef struct {
    uint8_t x_scale;
    uint8_t x_bias;
    uint8_t y_scale;
    uint8_t y_bias;
    bool horizontal;
} seven_segment_geometry_t;

static const uint8_t s_seven_segment_digits[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66,
    0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static const seven_segment_geometry_t s_seven_segment_geometry[7] = {
    {1U, 1U, 0U, 0U, true},  /* top */
    {7U, 1U, 1U, 1U, false}, /* top right */
    {7U, 1U, 10U, 2U, false},/* bottom right */
    {1U, 1U, 18U, 2U, true}, /* bottom */
    {0U, 0U, 10U, 2U, false},/* bottom left */
    {0U, 0U, 1U, 1U, false}, /* top left */
    {1U, 1U, 9U, 1U, true},  /* middle */
};

static void Draw7Segment(Adafruit_GFX* gfx, uint16_t x, uint16_t y,
                         uint16_t cS, uint16_t horizontal_width,
                         uint16_t vertical_width, bool horizontal,
                         uint16_t color)
{
    uint16_t i;

    if (horizontal) {
        for (i = 0U; i < cS; ++i) {
            GFX_drawFastHLine(gfx, (int16_t)(x + cS - i), (int16_t)(y + i),
                              (int16_t)(horizontal_width + 2U * i), color);
        }
        for (i = 0U; i < cS; ++i) {
            GFX_drawFastHLine(gfx, (int16_t)(x + i), (int16_t)(y + cS + i),
                              (int16_t)(horizontal_width + 2U * (cS - i)), color);
        }
    } else {
        for (i = 0U; i < cS; ++i) {
            GFX_drawFastVLine(gfx, (int16_t)(x + i), (int16_t)(y + cS - i),
                              (int16_t)(vertical_width + 2U * i), color);
        }
        for (i = 0U; i < cS; ++i) {
            GFX_drawFastVLine(gfx, (int16_t)(x + cS + i), (int16_t)(y + i),
                              (int16_t)(vertical_width + 2U * (cS - i)), color);
        }
    }
}

/* 仅用于时钟 HH/MM，数字右对齐，前面补 0。宽度公式与 clock_layout.h 保持一致。 */
static void Draw7Number(Adafruit_GFX* gfx, unsigned int n, unsigned int xLoc,
                        unsigned int yLoc, uint16_t cS, uint16_t fC,
                        uint16_t bC, uint16_t nD)
{
    uint16_t count = nD;
    uint16_t digit_step;
    uint16_t horizontal_width;
    uint16_t vertical_width;
    uint8_t segment;
    unsigned int num = n;

    if (count == 0U) {
        count = 1U;
    } else if (count > 10U) {
        count = 10U;
    }

    horizontal_width = (uint16_t)(5U * cS);
    vertical_width = (uint16_t)(7U * cS);
    digit_step = (uint16_t)(11U * cS + 2U);
    xLoc += (unsigned int)count * digit_step;

    while (count-- > 0U) {
        uint8_t digit = (uint8_t)(num % 10U);

        xLoc -= digit_step;
        for (segment = 0U; segment < 7U; ++segment) {
            const seven_segment_geometry_t* geometry = &s_seven_segment_geometry[segment];
            uint16_t x = (uint16_t)(xLoc + geometry->x_scale * cS + geometry->x_bias);
            uint16_t y = (uint16_t)(yLoc + geometry->y_scale * cS + geometry->y_bias);
            uint16_t color = (s_seven_segment_digits[digit] & (1U << segment)) ? fC : bC;

            Draw7Segment(gfx, x, y, cS, horizontal_width, vertical_width,
                         geometry->horizontal, color);
        }
        num /= 10U;
    }
}

/**
 * @brief 绘制时钟时间显示(HH:MM格式，7段数码管样式)
 *
 * @param gfx 图形上下文
 * @param tm  时间结构体
 * @param x   起始X坐标（数字左上角）
 * @param y   起始Y坐标
 * @param cS  缩放因子
 * @param nD  每个数字的位数(通常为2)
 */
static void DrawTime(Adafruit_GFX* gfx, const tm_t* tm, int16_t x, int16_t y, uint16_t cS, uint16_t nD) {
    // 绘制小时
    Draw7Number(gfx, tm->tm_hour, x, y, cS, GFX_BLACK, GFX_WHITE, nD);
    x += clock_digit_block_width(cS, nD) + 2 * cS;
    // 绘制冒号(两个方块)
    GFX_fillRect(gfx, x, clock_colon_top_y((uint16_t)y, cS), 2 * cS, 2 * cS, GFX_BLACK);
    GFX_fillRect(gfx, x, clock_colon_bot_y((uint16_t)y, cS), 2 * cS, 2 * cS, GFX_BLACK);
    x += 4 * cS;
    // 绘制分钟
    Draw7Number(gfx, tm->tm_min, x, y, cS, GFX_BLACK, GFX_WHITE, nD);
}

// 动态时间刷新窗口的状态(用于局部刷新)
// 这些变量追踪TIME_REFRESH区域内最后选择的子窗口
static uint16_t s_time_window_x = 0;  // TIME_REFRESH区域内的本地X坐标
static uint16_t s_time_window_y = 0;  // TIME_REFRESH区域内的本地Y坐标
static uint16_t s_time_window_w = TIME_REFRESH_WIDTH;  // 当前刷新窗口的宽度
static uint16_t s_time_window_h = TIME_REFRESH_HEIGHT;  // 当前刷新窗口的高度

/* 按当前模式取时钟画布几何。 */
static clock_refresh_geom_t gui_clock_geom(const gui_data_t *data)
{
    return clock_refresh_geom_sized(data != NULL && data->mode == MODE_CLOCK,
                                   data != NULL ? GUI_WIDTH(data) : 0U,
                                   data != NULL ? GUI_HEIGHT(data) : 0U);
}

/* 在几何约定的屏幕位置画 HH:MM。 */
static void DrawTimeFromGeom(Adafruit_GFX *gfx, const tm_t *tm, const clock_refresh_geom_t *geom)
{
    uint16_t x = 0;
    uint16_t y = 0;

    clock_hhmm_screen_xy(geom, &x, &y);
    DrawTime(gfx, tm, (int16_t)x, (int16_t)y, geom->cS, geom->nD);
}

// 在分屏的右侧绘制日历(从x=SPLIT_RIGHT_START开始)
/**
 * @brief 在分屏右侧绘制日历
 *
 * @param gfx    图形上下文
 * @param tm     公历时间
 * @param Lunar  农历日期
 * @param data   GUI 数据
 *
 * 从 x=392 起宽 408px；有分隔线；顶部为名称与电压；标准星期头 + 日期网格。
 */
static void DrawCalendar_Right(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                               const gui_data_t* data) {
    (void)Lunar;
    int16_t offset_x = SPLIT_RIGHT_START;  // 右侧起始X坐标
    int16_t right_width = GUI_WIDTH(data) - SPLIT_RIGHT_START;  // 右侧宽度
    bool large = GUI_HEIGHT(data) > GUI_LARGE_SCREEN_H;

    // 绘制垂直分隔线(缩短以匹配保留的底部区域)
    // 线从y=70到y=420(顶部和底部各60px边距)
    int16_t separator_top = 70;
    int16_t separator_bottom = large ? 420 : GUI_HEIGHT(data) - 10;
    GFX_drawFastVLine(gfx, offset_x - 2, separator_top, separator_bottom - separator_top, GFX_BLACK);

    // 顶部头：左侧蓝牙名称，右侧电压(同一行)
    int16_t header_y = large ? 86 : 78;

    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    DrawVoltage(gfx, GUI_WIDTH(data) - 10 - 2, header_y - 9, data->voltage_mv);

    // 日历区域左侧的蓝牙名称(使用wqy9以匹配电压字体)
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, offset_x + 8, header_y);
    GFX_puts(gfx, data->ssid);

    int16_t week_x = offset_x + 5;
    int16_t week_y = large ? 96 : 86;
    calendar_grid_rect_t days = {
        .x = offset_x + 5,
        .y = large ? 136 : 116,
        .width = (int16_t)(right_width - 10),
        .bottom = large ? 420 : (int16_t)(GUI_HEIGHT(data) - 10),
    };

    DrawWeekHeader(gfx, week_x, week_y, days.width, data);
    DrawMonthGrid(gfx, tm, data, days);
}

/* 绘制「YYYY年M月D日」；pad2 时月日两位。 */
static void DrawYmd(Adafruit_GFX* gfx, const tm_t* tm, uint16_t num_color, bool pad2)
{
    gui_print_date_part(gfx, tm->tm_year + YEAR0, 1U, num_color, "年");
    gui_print_date_part(gfx, tm->tm_mon + 1, pad2 ? 2U : 1U, num_color, "月");
    gui_print_date_part(gfx, tm->tm_mday, pad2 ? 2U : 1U, num_color, pad2 ? "日 " : "日");
}

/* 右对齐节气名或倒计时两行。 */
static void DrawJieqiRight(Adafruit_GFX* gfx, const tm_t* tm,
                           int16_t right_x, int16_t y1, int16_t y2,
                           uint16_t line1_color, uint16_t line2_color)
{
    char line1[16];
    char line2[16];
    uint8_t days = 0;
    uint8_t jq = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &days);

    gui_set_font(gfx, GUI_FONT_WQY12);
    clock_format_jieqi_countdown(line1, sizeof(line1), line2, sizeof(line2),
                                 JieQiStr[jq % 24], days);
    GFX_setTextColor(gfx, line1_color, GFX_WHITE);
    GFX_setCursor(gfx, right_x - GFX_getUTF8Width(gfx, line1), y1);
    GFX_puts(gfx, line1);
    if (line2[0] != '\0') {
        GFX_setTextColor(gfx, line2_color, GFX_WHITE);
        GFX_setCursor(gfx, right_x - GFX_getUTF8Width(gfx, line2), y2);
        GFX_puts(gfx, line2);
    }
}

/**
 * @brief 在分屏左侧绘制时钟
 *
 * @param gfx         图形上下文
 * @param tm          公历时间
 * @param Lunar       农历日期
 * @param data        GUI 数据
 * @param width_limit 左侧区域宽度
 *
 * 上下各 60px 边距；顶栏年月日/农历/温度；中部 7 段时间；底部干支/周数/节气。
 * 仅用黑色（局刷不支持彩色）。
 */
static void DrawClock_Left(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                           const gui_data_t* data, uint16_t width_limit) {
    // 局部刷新使用黑色(不支持红色)
    uint16_t text_color = GFX_BLACK;

    int16_t padding = 15;

    // 紧凑设计的布局常量，上下各60px边距
    // 内容区域：y=60到y=420 (高度360px)
    int16_t top_line_y = 128;  // 与日历星期头底部对齐
    int16_t bottom_line_y = 360;  // 上移以在底部留空间
    int16_t bottom_info_y1 = 380;  // 底部信息第一行
    int16_t bottom_info_y2 = 404;  // 底部信息第二行

    // === 第1行：年/月/日，使用大字体 ===
    // 数字使用helvB18，汉字使用wqy12
    if (gui_page_hits(gfx, 60, 128)) {
    GFX_setCursor(gfx, padding, 92);
    DrawYmd(gfx, tm, text_color, false);

    // === 第2行：左侧农历日期，右侧温度 ===
    gui_set_font(gfx, GUI_FONT_WQY12);
    GFX_setCursor(gfx, padding, 115);  // 顶部边距+60
    gui_print_lunar_md(gfx, Lunar);

    // 右侧温度，数字使用大字体
    gui_set_font(gfx, GUI_FONT_HELVB18);
    // 计算温度数字宽度，不使用snprintf(优化性能)
    int8_t temp = data->temperature;
    int16_t digit_width = GFX_getUTF8Width(gfx, "0");
    int16_t temp_num_width = digit_width;  // 至少一位数字
    if (temp < 0) {
        temp_num_width += GFX_getUTF8Width(gfx, "-");  // 负号宽度
        temp = -temp;
    }
    if (temp >= 10) temp_num_width += digit_width;  // 十位
    if (temp >= 100) temp_num_width += digit_width;  // 百位(不太可能)
    gui_set_font(gfx, GUI_FONT_WQY12);
    int16_t degree_width = GFX_getUTF8Width(gfx, "℃");
    int16_t total_temp_width = temp_num_width + degree_width;

    gui_set_font(gfx, GUI_FONT_HELVB18);
    GFX_setCursor(gfx, width_limit - padding - total_temp_width, 115);  // 顶部边距+60
    gui_print_int(gfx, data->temperature, 1U);
    gui_set_font(gfx, GUI_FONT_WQY12);
    GFX_puts(gfx, "℃");

    }
    // 顶部水平线(与日历星期头底部对齐)
    GFX_drawFastHLine(gfx, padding, top_line_y, width_limit - 2 * padding, GFX_BLACK);

    clock_refresh_geom_t geom = gui_clock_geom(data);
    DrawTimeFromGeom(gfx, tm, &geom);

    // 底部水平线(上移)
    GFX_drawFastHLine(gfx, padding, bottom_line_y, width_limit - 2 * padding, GFX_BLACK);

    if (!gui_page_hits(gfx, bottom_line_y, 420)) return;
    // === 底部区域：左侧 - 农历干支年 + 周数 ===
    // 底部文字使用wqy12字体
    gui_set_font(gfx, GUI_FONT_WQY12);
    GFX_setTextColor(gfx, text_color, GFX_WHITE);

    // 左列：第一行农历干支年，第二行周数
    GFX_setCursor(gfx, padding, bottom_info_y1);
    gui_print_lunar_year(gfx, Lunar);
    GFX_puts(gfx, "年");

    GFX_setCursor(gfx, padding, bottom_info_y2);
    gui_print_int(gfx, GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday), 1U);
    GFX_puts(gfx, "周");

    DrawJieqiRight(gfx, tm, width_limit - padding, bottom_info_y1, bottom_info_y2,
                   text_color, text_color);
}

// 绘制完整的分屏界面，包含时钟和日历(用于初始的全屏刷新)
/**
 * @brief 绘制分屏时钟+日历（MODE_CLOCK_CALENDAR，首次全刷）
 *
 * @param gfx    图形上下文
 * @param tm     公历时间
 * @param Lunar  农历日期
 * @param data   GUI 数据
 *
 * 左 0–392：时钟（之后可局刷）；右 392–800：月历；中间分隔线。
 */
static void DrawClockCalendar_Full(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                                   const gui_data_t* data) {
    // 在屏幕顶部和底部绘制水平线(距边缘30像素)
    GFX_fillRect(gfx, 15, 45, GUI_WIDTH(data) - 30, 2, GFX_BLACK);  // 顶部线(2px粗)
    GFX_fillRect(gfx, 15, GUI_HEIGHT(data) - 45, GUI_WIDTH(data) - 30, 2, GFX_BLACK);  // 底部线(2px粗)

    // 在右侧绘制日历
    DrawCalendar_Right(gfx, tm, Lunar, data);

    // 在左侧绘制时钟(初次绘制使用全彩色)
    DrawClock_Left(gfx, tm, Lunar, data, SPLIT_LEFT_WIDTH);
}

/**
 * @brief 绘制全屏时钟（MODE_CLOCK）
 *
 * @param gfx    图形上下文
 * @param tm     公历时间
 * @param Lunar  农历日期
 * @param data   GUI 数据
 *
 * 支持局刷，因此只用黑色。
 */
static void DrawClock(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                      const gui_data_t* data) {
    // 为了支持局部刷新(时钟模式)，使用黑色代替红色
    // 因为局部刷新只支持黑白两色
    uint16_t text_color = GFX_BLACK;  // 从GFX_RED改为GFX_BLACK

    uint8_t padding = GUI_HEIGHT(data) > GUI_LARGE_SCREEN_H ? 100 : 40;
    if (gui_page_hits(gfx, 0, 70)) {
    GFX_setCursor(gfx, padding, 36);
    DrawYmd(gfx, tm, text_color, true);

    GFX_setCursor(gfx, padding, 58);
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_puts(gfx, "星期");
    GFX_puts(gfx, Lunar_DayString[tm->tm_wday]);
    GFX_setCursor(gfx, 138, 58);
    gui_print_lunar_md(gfx, Lunar);

    DrawVoltage(gfx, GUI_WIDTH(data) - padding, 25, data->voltage_mv);

    char ssid[5] = {0};
    int16_t sw = GFX_getUTF8Width(gfx, "25℃[1234]");
    clock_ssid_tail4(ssid, sizeof(ssid), data->ssid);
    GFX_setCursor(gfx, GUI_WIDTH(data) - padding - sw - 2, 58);
    gui_set_font(gfx, GUI_FONT_WQY9);
    gui_print_int(gfx, data->temperature, 1U);
    GFX_puts(gfx, "℃[");
    GFX_puts(gfx, ssid);
    GFX_puts(gfx, "]");

    }
    GFX_drawFastHLine(gfx, padding - 10, 68, GUI_WIDTH(data) - 2 * (padding - 10), GFX_BLACK);

    clock_refresh_geom_t geom = gui_clock_geom(data);
    DrawTimeFromGeom(gfx, tm, &geom);

    GFX_drawFastHLine(gfx, padding - 10, GUI_HEIGHT(data) - 68, GUI_WIDTH(data) - 2 * (padding - 10), GFX_BLACK);

    if (!gui_page_hits(gfx, GUI_HEIGHT(data) - 68, GUI_HEIGHT(data))) return;
    GFX_setCursor(gfx, padding, GUI_HEIGHT(data) - 68 + 30);
    gui_set_font(gfx, GUI_FONT_WQY12);
    gui_print_lunar_year(gfx, Lunar);
    GFX_setTextColor(gfx, text_color, GFX_WHITE);
    GFX_puts(gfx, Lunar_ZodiacString[LUNAR_GetZodiac(Lunar)]);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_puts(gfx, "年");

    GFX_setCursor(gfx, padding, GUI_HEIGHT(data) - 68 + 30 + 20);
    GFX_puts(gfx, " ");
    gui_print_int(gfx, GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday), 1U);
    GFX_puts(gfx, "周");

    DrawJieqiRight(gfx, tm, GUI_WIDTH(data) - padding, GUI_HEIGHT(data) - 68 + 30,
                   GUI_HEIGHT(data) - 68 + 30 + 20, text_color, GFX_BLACK);
}

/**
 * @brief 按显示模式绘制完整界面
 *
 * @param data           GUI 数据
 * @param callback       分页缓冲回调
 * @param callback_data  回调用户数据
 *
 * 支持黑白/三色/四色；按堆大小分页。
 */
void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data) {
    // 验证一周起始日参数
    if (data->week_start > 6) data->week_start = 0;

    tm_t tm = {0};
    struct Lunar_Date Lunar;

    // 将UNIX时间戳转换为tm结构
    transformTime(data->timestamp, &tm);

    Adafruit_GFX gfx;
    /* Static page buffer; page sizing must not depend on the heap reserve. */
    int16_t ph = 0;

    // 根据颜色模式初始化GFX
    if (data->color == 2)
        GFX_begin_3c(&gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 三色屏幕
#if !GFX_FIXED_BWR_PANEL
    else if (data->color == 3)
        GFX_begin_4c(&gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 四色屏幕
#endif
    else
        GFX_begin(&gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 黑白屏幕

    LUNAR_SolarToLunar(&Lunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
    GFX_firstPage(&gfx);
    do {
        // 填充白色背景
        GFX_fillScreen(&gfx, GFX_WHITE);

        // 根据显示模式绘制不同界面
        switch (data->mode) {
            case MODE_CALENDAR:
                DrawCalendar(&gfx, &tm, &Lunar, data);  // 全屏日历
                break;
            case MODE_CLOCK:
                DrawClock(&gfx, &tm, &Lunar, data);  // 全屏时钟
                break;
            case MODE_CLOCK_CALENDAR:
                DrawClockCalendar_Full(&gfx, &tm, &Lunar, data);  // 分屏模式
                break;
            default:
                break;
        }
        // 如果是2025年1月且未同步时间，显示同步提示
        if ((data->mode == MODE_CALENDAR || data->mode == MODE_CLOCK) &&
            (tm.tm_year + YEAR0 == 2025 && tm.tm_mon + 1 == 1)) {
            DrawTimeSyncTip(&gfx, data);
        }
    } while (GFX_nextPage(&gfx, callback, callback_data));  // 分页绘制

    GFX_end(&gfx);
}

/**
 * @brief 仅画 HH:MM，供时钟局刷（分屏与全屏时钟共用）
 *
 * @param data           GUI 数据
 * @param callback       分页缓冲回调
 * @param callback_data  回调用户数据
 *
 * 只画时间区；配合 GFX_setWindow 可缩到单个数字；黑白局刷，减少刷新面积。
 */
void DrawGUI_ClockOnly(const gui_data_t* data, buffer_callback callback, void* callback_data) {
    tm_t tm = {0};
    clock_refresh_geom_t geom = gui_clock_geom(data);

    transformTime(data->timestamp, &tm);

    Adafruit_GFX gfx;
    /* 页高 0：GFX_begin 使用静态页缓冲 */
    GFX_begin(&gfx, geom.canvas_w, geom.canvas_h, 0);
    GFX_setWindow(&gfx, s_time_window_x, s_time_window_y, s_time_window_w, s_time_window_h);

    GFX_firstPage(&gfx);
    do {
        GFX_fillScreen(&gfx, GFX_WHITE);
        DrawTime(&gfx, &tm,
                 (int16_t)clock_hhmm_local_x(geom.canvas_w, geom.cS, geom.nD),
                 (int16_t)clock_hhmm_local_y(geom.canvas_h, geom.cS),
                 geom.cS, geom.nD);
    } while (GFX_nextPage(&gfx, callback, callback_data));

    GFX_end(&gfx);
}

/* 画布局部窗 → 屏幕矩形，并记下 ClockOnly 窗口。 */
void GetTimeRefreshAreaFromLocal(const gui_data_t *data,
                                 uint16_t lx, uint16_t ly, uint16_t lw, uint16_t lh,
                                 uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h) {
    clock_refresh_geom_t geom = gui_clock_geom(data);

    s_time_window_x = lx;
    s_time_window_y = ly;
    s_time_window_w = lw;
    s_time_window_h = lh;

    if (x) *x = (uint16_t)(geom.origin_x + lx);
    if (y) *y = (uint16_t)(geom.origin_y + ly);
    if (w) *w = lw;
    if (h) *h = lh;
}

/* 屏幕坐标 HH:MM（或个位）窗，并记下 DrawGUI_ClockOnly 用的局部窗口。 */
void GetTimeRefreshArea(const gui_data_t *data, uint32_t prev_ts,
                        uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
    clock_refresh_geom_t geom = gui_clock_geom(data);
    uint32_t timestamp = (data != NULL) ? data->timestamp : 0U;
    clock_refresh_window_t win =
        clock_refresh_local_window(&geom, prev_ts, timestamp);

    GetTimeRefreshAreaFromLocal(data, win.x, win.y, win.w, win.h, x, y, w, h);
}
