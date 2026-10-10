#include "GUI.h"

#include <stddef.h>

#include "Lunar.h"
#include "calendar_grid.h"
#include "calendar_annotations.h"
#include "calendar_resources.h"
#include "clock_layout.h"
#include "clock_text.h"
#include "fonts.h"
#include "ui_resources.h"
#include "todo_items.h"

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

/* Rendering already shares GFX's static page buffer and runs serially in main.
 * Share its context too: the font caches must not consume both the full-frame
 * worker's stack frame and a nested clock-only refresh frame. GFX_begin resets
 * this context before every render, including external-font revision state. */
static Adafruit_GFX m_gui_gfx;
static bool s_todo_render_valid;

bool GUI_TodoRenderValid(void) { return s_todo_render_valid; }

/* Page culling is independent of the external layout's decorative lines. */
static bool gui_page_hits(const Adafruit_GFX *gfx, int32_t top, int32_t bottom)
{
    int32_t y = gfx->py + (int32_t)gfx->current_page * gfx->page_height;
    return gfx->rotation != GFX_ROTATE_0 || (top < y + gfx->page_height && bottom > y);
}

/* Font slots have validated geometry. Include the complete glyph bounding box
 * and the missing-glyph placeholder, not only the Latin capital-A ascent.
 * The two baselines describe a union, in either vertical order. */
static bool gui_text_page_hits(const Adafruit_GFX *gfx, uint8_t font,
                               int16_t first, int16_t last)
{
    const gui_font_metrics_t *m = GUI_GetFontMetrics((gui_font_id_t)font);
    if (m == NULL) return true;
    int16_t ascent = m->max_char_height + m->y_offset;
    int16_t descent = -m->y_offset;
    if (ascent < m->ascent) ascent = m->ascent;
    if (descent < m->max_char_height - m->ascent)
        descent = m->max_char_height - m->ascent;
    int16_t top = first < last ? first : last;
    int16_t bottom = first > last ? first : last;
    return gui_page_hits(gfx, top - ascent, bottom + descent);
}

static u8g2_font_placeholder_t gui_font_placeholder(gui_font_id_t font_id)
{
    const gui_font_metrics_t* metrics = GUI_GetFontMetrics(font_id);
    return metrics != NULL ? *metrics : (u8g2_font_placeholder_t){0};
}

/* Select a fixed font slot or the optional TODO font. External fonts keep a fixed
 * placeholder geometry so a deleted or incomplete glyph never collapses the
 * layout. */
static void gui_set_font(Adafruit_GFX* gfx, gui_font_id_t font_id)
{
#if !GUI_USE_EXTERNAL_FONTS
    if (font_id == GUI_FONT_TODO)
        font_id = todo_items_font_size() == 9U ? GUI_FONT_WQY9 : GUI_FONT_WQY12;
#endif
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
                                uint16_t color, const char *unit,
                                gui_font_id_t number_font, gui_font_id_t unit_font)
{
    gui_set_text_style(gfx, color, GFX_WHITE, number_font);
    gui_print_int(gfx, value, min_digits);
    gui_set_text_style(gfx, GFX_BLACK, GFX_WHITE, unit_font);
    GFX_puts(gfx, unit);
}

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
    clock_format_lunar_md(text, sizeof(text), LUNAR_LeapName(lunar->IsLeap),
                          LUNAR_MonthName(lunar->Month), LUNAR_DateName(lunar->Date));
    GFX_puts(gfx, text);
}

static void gui_print_lunar_year(Adafruit_GFX *gfx, const struct Lunar_Date *lunar)
{
    GFX_puts(gfx, LUNAR_StemName(LUNAR_GetStem(lunar)));
    GFX_puts(gfx, LUNAR_BranchName(LUNAR_GetBranch(lunar)));
}

// 公历节日数据表

// 农历节日数据表

// 放假和调休数据，每年更新
// 数据格式：0xWMMDD，其中W为工作日标志(1表示需要上班，0表示放假)，MM为月份，DD为日期

/**
 * @brief 查询指定日期是否为法定节假日或调休工作日
 *
 * @param[in] mon  月份(1-12)
 * @param[in] day  日期(1-31)
 * @param work 输出参数：true表示调休需上班，false表示放假
 * @return true 该日期在节假日表中
 * @return false 该日期为普通工作日
 */

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
    if (calendar_resource_festival(CAL_RES_LUNAR_FESTIVALS,
                                   Lunar->Month, Lunar->Date, festival, 10U)) return true;
    // 除夕：春节前一天（12/29 或 12/30），12/30 已在上面判断
    // 需要判断次日是否为正月初一，因为农历大小月不固定
    if (Lunar->Month == 12 && Lunar->Date == 29) {
        struct Lunar_Date nextLunar;
        uint16_t next_year = year;
        uint8_t next_month = mon;
        uint8_t next_day = day + 1U;
        if (next_day > get_last_day(year, mon - 1U)) {
            next_day = 1U;
            if (++next_month > 12U) {
                next_month = 1U;
                ++next_year;
            }
        }
        LUNAR_SolarToLunar(&nextLunar, next_year, next_month, next_day);
        if (nextLunar.Month == 1 && nextLunar.Date == 1) {
            return calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                          CAL_SPECIAL_NEW_YEARS_EVE,
                                          festival, 10U);
        }
    }
    // 母亲节: 五月第二个星期日
    if (mon == 5 && week == 0 && day >= 8 && day <= 14) {
        return calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                      CAL_SPECIAL_MOTHERS_DAY, festival, 10U);
    }
    // 父亲节: 六月第三个星期日
    if (mon == 6 && week == 0 && day >= 15 && day <= 21) {
        return calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                      CAL_SPECIAL_FATHERS_DAY, festival, 10U);
    }
    // 感恩节：十一月第四个星期四
    if (mon == 11 && week == 4 && day >= 22 && day <= 28) {
        return calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                      CAL_SPECIAL_THANKSGIVING, festival, 10U);
    }

    // 公历节日
    if (calendar_resource_festival(CAL_RES_SOLAR_FESTIVALS, mon, day, festival, 10U)) return true;
    // 二十四节气
    // 节气是中国传统历法的重要组成部分，用于指导农业生产
    uint8_t JQdate;
    if (GetJieQi(year, mon, day, &JQdate) && JQdate == day) {
        // 计算节气索引：每月2个节气，day>=15则为第二个节气
        uint8_t JQ = (mon - 1) * 2;
        if (day >= 15) JQ++;
        if (JQ == 6)  // 清明
            return calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                          CAL_SPECIAL_QINGMING, festival, 10U);
        strcpy(festival, LUNAR_JieQiName(JQ));
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

    const ui_component_t *layout = ui_resource_component(
        data->mode == MODE_CLOCK ? UI_COMP_CLOCK_HEADER : UI_COMP_CAL_HEADER);
    if (!layout) return;
    gui_set_font(gfx, (gui_font_id_t)layout->font_tertiary);

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
static void DrawVoltage(Adafruit_GFX* gfx, int16_t right_x, int16_t y, uint16_t voltage_mv,
                        gui_font_id_t font) {
    char voltage[8];

    clock_format_voltage(voltage, sizeof(voltage), voltage_mv);
    gui_set_font(gfx, font);
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
static void DrawDateHeader(Adafruit_GFX* gfx, const ui_component_t *layout, const tm_t* tm,
                           const struct Lunar_Date* Lunar, const gui_data_t* data) {
    int16_t x = layout->x;
    int16_t y = layout->y;
    if (!gui_text_page_hits(gfx, layout->font_primary, y - 2, y - 2) &&
        !gui_text_page_hits(gfx, layout->font_secondary, y - 2, y - 2) &&
        !gui_text_page_hits(gfx, layout->font_tertiary, y - 14, y) &&
        !gui_text_page_hits(gfx, layout->font_tertiary, layout->value0 + 9,
                            layout->value0 + 9)) return;
    // 绘制公历年月(大字体)
    GFX_setCursor(gfx, x, y - 2);
    gui_print_date_part(gfx, tm->tm_year + YEAR0, 1U, GFX_RED, "年",
                        (gui_font_id_t)layout->font_primary,
                        (gui_font_id_t)layout->font_secondary);
    gui_print_date_part(gfx, tm->tm_mon + 1, 1U, GFX_RED, "月",
                        (gui_font_id_t)layout->font_primary,
                        (gui_font_id_t)layout->font_secondary);

    int16_t tx = gfx->tx;  // 记录当前文字位置
    int16_t ty = y;

    // 绘制农历日期和周数
    gui_set_font(gfx, (gui_font_id_t)layout->font_tertiary);
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
    GFX_puts(gfx, LUNAR_ZodiacName(LUNAR_GetZodiac(Lunar)));
    GFX_puts(gfx, "]");

    // 绘制右侧信息：电压和WiFi名称
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    DrawVoltage(gfx, GUI_WIDTH(data) - layout->value1, layout->value0, data->voltage_mv,
                (gui_font_id_t)layout->font_tertiary);
    GFX_setCursor(gfx, GUI_WIDTH(data) - GFX_getUTF8Width(gfx, data->ssid) -
                  (layout->value1 - 2), y);
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
static void DrawWeekHeader(Adafruit_GFX* gfx, const ui_component_t *layout,
                           const gui_data_t* data) {
    int16_t x = layout->x;
    int16_t y = layout->y;
    int16_t span_width = layout->w;
    calendar_week_header_metrics_t hdr = {
        .col_w = (uint8_t)(span_width / 7),
        .remainder = (uint8_t)(span_width % 7),
        .row_h = (uint8_t)layout->h,
    };

    gui_set_font(gfx, (gui_font_id_t)layout->font_primary);
    uint8_t fh = (hdr.row_h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + 1;
    if (!gui_page_hits(gfx, y, y + hdr.row_h) &&
        !gui_text_page_hits(gfx, layout->font_primary, y + fh, y + fh)) return;
    int16_t cw = GFX_getUTF8Width(gfx, LUNAR_WeekdayName(0));
    for (int i = 0; i < 7; i++) {
        uint8_t day = (data->week_start + i) % 7;
        uint16_t bg = (day == 0 || day == 6) ? GFX_RED : GFX_BLACK;
        GFX_fillRect(gfx, x + i * hdr.col_w, y, i == 6 ? (hdr.col_w + hdr.remainder) : hdr.col_w, hdr.row_h, bg);
        GFX_setTextColor(gfx, GFX_WHITE, bg);
        GFX_setCursor(gfx, x + (hdr.col_w - cw) / 2 + i * hdr.col_w, y + fh);
        GFX_puts(gfx, LUNAR_WeekdayName(day));
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
                          const ui_component_t *layout) {
    calendar_grid_rect_t rect = {
        .x = layout->x,
        .y = layout->y,
        .width = layout->w,
        .bottom = (int16_t)(layout->y + layout->h),
    };
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

        gui_set_font(gfx, (gui_font_id_t)layout->font_primary);
        char day_text[3];
        size_t day_length = gui_format_int(day_text, sizeof(day_text), day, 1U);
        GFX_setCursor(gfx, cell.bx + (2 * cell.cr - GFX_getUTF8Width(gfx, day_text)) / 2,
                      cell.by - (cell.cr - GFX_getFontHeight(gfx)));
        GFX_write(gfx, day_text, day_length);

        char festival_buf[10] = {0};
        const char* festival;
        calendar_annotation_t anno;
        bool has_anno = calendar_annotations_get(year, month, day, &anno);
        bool work = false;
        bool has_holiday = calendar_resource_holiday(year, month, day, &work);

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
            format_lunar_month(festival_buf, LUNAR_LeapName(dayLunar.IsLeap),
                               LUNAR_MonthName(dayLunar.Month));
            festival = festival_buf;
        } else {
            festival = LUNAR_DateName(dayLunar.Date);
        }
        if (replace_below) {
            festival = anno.label;
            gui_set_font(gfx, annotation_wqy_font(anno_pixel));
            GFX_setTextColor(gfx, anno_fg, (day == tm->tm_mday) ? GFX_RED : GFX_WHITE);
        } else {
            gui_set_font(gfx, (gui_font_id_t)layout->font_secondary);
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
        }

        if (has_holiday) {
            calendar_holiday_mark_t holiday =
                calendar_holiday_mark(&rect, &m, &cell, i,
                                      has_anno && calendar_annotation_draws_corner(&anno));

            if (day == tm->tm_mday) {
                GFX_fillCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r,
                               GFX_WHITE);
                GFX_drawCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r,
                               GFX_RED);
            }
            gui_set_font(gfx, (gui_font_id_t)layout->font_tertiary);
            GFX_setTextColor(gfx, work ? GFX_BLACK : GFX_RED, GFX_WHITE);
            GFX_setCursor(gfx, holiday.text_x, holiday.text_y);
            GFX_puts(gfx, work ? "班" : "休");
        }

        if (has_anno && calendar_annotation_draws_dot(&anno)) {
            int16_t mark_x;
            int16_t mark_y;
            uint8_t mark_r;

            calendar_annotation_dot_center(&rect, &m, i, has_holiday,
                                           &mark_x, &mark_y, &mark_r);
            /* 当日红底在分屏小格中会延伸到角落，白边让红点保持可辨认。 */
            if (day == tm->tm_mday) {
                GFX_fillCircle(gfx, mark_x, mark_y, mark_r + 1U, GFX_WHITE);
            }
            GFX_fillCircle(gfx, mark_x, mark_y, mark_r, GFX_RED);
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
    const ui_component_t *header = ui_resource_component(UI_COMP_CAL_HEADER);
    const ui_component_t *week = ui_resource_component(UI_COMP_CAL_WEEK);
    const ui_component_t *grid = ui_resource_component(UI_COMP_CAL_GRID);
    if (!header || !week || !grid) return;
    DrawDateHeader(gfx, header, tm, Lunar, data);
    DrawWeekHeader(gfx, week, data);
    DrawMonthGrid(gfx, tm, data, grid);
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
static uint16_t s_time_window_w;
static uint16_t s_time_window_h;

/* 按当前模式取时钟画布几何。 */
static clock_refresh_geom_t gui_clock_geom(const gui_data_t *data)
{
    clock_refresh_geom_t geometry = {0};
    if (data != NULL) {
        (void)ui_resources_clock_geom(data->mode == MODE_CLOCK,
                                      (uint16_t)GUI_WIDTH(data),
                                      (uint16_t)GUI_HEIGHT(data), &geometry);
    }
    return geometry;
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
    const ui_component_t *separator = ui_resource_component(UI_COMP_SPLIT_SEPARATOR);
    const ui_component_t *header = ui_resource_component(UI_COMP_SPLIT_RIGHT_HEADER);
    const ui_component_t *week = ui_resource_component(UI_COMP_SPLIT_WEEK);
    const ui_component_t *grid = ui_resource_component(UI_COMP_SPLIT_GRID);
    if (!separator || !header || !week || !grid) return;

    GFX_drawFastVLine(gfx, separator->x, separator->y, separator->h, GFX_BLACK);

    if (gui_text_page_hits(gfx, header->font_primary, header->y, header->value0 + 9)) {
        GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
        DrawVoltage(gfx, header->value1, header->value0, data->voltage_mv,
                    (gui_font_id_t)header->font_primary);
        GFX_setCursor(gfx, header->x, header->y);
        GFX_puts(gfx, data->ssid);
    }

    DrawWeekHeader(gfx, week, data);
    DrawMonthGrid(gfx, tm, data, grid);
}

/* 绘制「YYYY年M月D日」；pad2 时月日两位。 */
static void DrawYmd(Adafruit_GFX* gfx, const tm_t* tm, uint16_t num_color, bool pad2,
                    const ui_component_t *layout)
{
    gui_font_id_t number = (gui_font_id_t)layout->font_primary;
    gui_font_id_t unit = (gui_font_id_t)layout->font_secondary;
    gui_print_date_part(gfx, tm->tm_year + YEAR0, 1U, num_color, "年", number, unit);
    gui_print_date_part(gfx, tm->tm_mon + 1, pad2 ? 2U : 1U, num_color, "月", number, unit);
    gui_print_date_part(gfx, tm->tm_mday, pad2 ? 2U : 1U, num_color,
                        pad2 ? "日 " : "日", number, unit);
}

/* 右对齐节气名或倒计时两行。 */
static void DrawJieqiRight(Adafruit_GFX* gfx, const tm_t* tm,
                           int16_t right_x, int16_t y1, int16_t y2,
                           uint16_t line1_color, uint16_t line2_color,
                           gui_font_id_t font)
{
    char line1[16];
    char line2[16];
    uint8_t days = 0;
    uint8_t jq = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &days);

    gui_set_font(gfx, font);
    clock_format_jieqi_countdown(line1, sizeof(line1), line2, sizeof(line2),
                                 LUNAR_JieQiName(jq % 24), days);
    GFX_setTextColor(gfx, line1_color, GFX_WHITE);
    GFX_setCursor(gfx, right_x - GFX_getUTF8Width(gfx, line1), y1);
    GFX_puts(gfx, line1);
    if (line2[0] != '\0') {
        GFX_setTextColor(gfx, line2_color, GFX_WHITE);
        GFX_setCursor(gfx, right_x - GFX_getUTF8Width(gfx, line2), y2);
        GFX_puts(gfx, line2);
    }
}

/* TODO text has a smaller writable area than the complete page buffer. Clip
 * font scanlines too: generated glyph offsets and narrow custom UI panes must
 * never overwrite another row, the split calendar, or the footer. Rendering
 * is serial, like m_gui_gfx, so this costs no nested stack context. */
static struct {
    void (*forward)(u8g2_font_t *, int16_t, int16_t, int16_t, uint8_t, uint16_t);
    int16_t left, right, top, bottom;
} m_todo_clip;

static void todo_clip_line(u8g2_font_t *font, int16_t x, int16_t y,
                           int16_t length, uint8_t direction, uint16_t color)
{
#if !GFX_FIXED_FONT_DIRECTION_0
    if (direction == 2U) { x -= length - 1; direction = 0U; }
    if (direction == 3U) { y -= length - 1; direction = 1U; }
#endif
    if (direction == 0U) {
        if (y < m_todo_clip.top || y >= m_todo_clip.bottom) return;
        if (x < m_todo_clip.left) { length -= m_todo_clip.left - x; x = m_todo_clip.left; }
        if ((int32_t)x + length > m_todo_clip.right) length = m_todo_clip.right - x;
    } else {
        if (x < m_todo_clip.left || x >= m_todo_clip.right) return;
        if (y < m_todo_clip.top) { length -= m_todo_clip.top - y; y = m_todo_clip.top; }
        if ((int32_t)y + length > m_todo_clip.bottom) length = m_todo_clip.bottom - y;
    }
    if (length > 0) m_todo_clip.forward(font, x, y, length, direction, color);
}

/* Measure the furthest painted pixel, including placeholder glyphs and the
 * actual advances between letters. Separately adding the widths of a title
 * and its ellipsis loses the final title glyph's trailing advance. */
static int16_t todo_text_extent(Adafruit_GFX *gfx, const char *text, int16_t *left)
{
    int16_t cursor = 0, right = 0;
    if (left != NULL) *left = 0;
    while (*text != '\0') {
        const uint8_t *bytes = (const uint8_t *)text;
        uint16_t code = bytes[0];
        unsigned length = code < 0x80U ? 1U : code < 0xE0U ? 2U : 3U;
        if (length == 2U) code = ((code & 0x1FU) << 6) | (bytes[1] & 0x3FU);
        if (length == 3U) code = ((code & 0x0FU) << 12) | ((bytes[1] & 0x3FU) << 6) | (bytes[2] & 0x3FU);
        int8_t advance = 0;
        int16_t edge = cursor;
        int16_t start = cursor;
        if (u8g2_GetGlyphWidthIfPresent(&gfx->u8g2, code, &advance)) {
            if (gfx->u8g2.font_decode.glyph_width != 0U) {
                start += gfx->u8g2.glyph_x_offset;
                edge += gfx->u8g2.glyph_x_offset + gfx->u8g2.font_decode.glyph_width;
            }
        } else if (code != ' ' && gfx->u8g2.placeholder_enabled) {
            start += gfx->u8g2.placeholder.x_offset;
            edge += gfx->u8g2.placeholder.x_offset + gfx->u8g2.placeholder.max_char_width;
            advance = gfx->u8g2.placeholder.advance;
        }
        if (edge > right) right = edge;
        if (left != NULL && start < *left) *left = start;
        cursor += advance;
        text += length;
    }
    return right;
}

static int16_t todo_text_width(Adafruit_GFX *gfx, const char *text)
{
    return todo_text_extent(gfx, text, NULL);
}

/* Draw a bounded UTF-8 line, wrapping at glyph boundaries. The final line
 * reserves space for an ellipsis when the title is wider than the pane. */
static void DrawTodoText(Adafruit_GFX *gfx, const char **cursor, int16_t width,
                         bool last_line, bool strike)
{
    char line[TODO_ITEMS_TEXT_MAX + 4U];
    size_t length = 0U;
    const char *text = *cursor;
    const char *draw = text;
    if (width <= 0) return;
    if (todo_text_width(gfx, text) <= width) {
        *cursor = text + strlen(text);
        goto draw_line;
    }
    const char *dots = last_line ? "..." : "";
    while (*dots != '\0' && todo_text_width(gfx, dots) > width) dots++;
    while (*text != '\0') {
        unsigned char first = (unsigned char)*text;
        size_t bytes = first < 0x80U ? 1U : (first < 0xE0U ? 2U : 3U);
        if (length + bytes > TODO_ITEMS_TEXT_MAX) break;
        for (size_t i = 0U; i < bytes; ++i) line[length + i] = text[i];
        clock_text_append(line, sizeof(line), length + bytes, dots);
        if (todo_text_width(gfx, line) > width) break;
        length += bytes;
        text += bytes;
    }
    clock_text_append(line, sizeof(line), length, dots);
    *cursor = text;
    draw = line;
draw_line:
    /* Selecting an external font resets its transparency. Preserve the
     * overlapping bearings of adjacent italic glyphs on the white canvas. */
    GFX_setFontMode(gfx, 1U);
    int16_t origin_x = gfx->tx, origin_y = gfx->ty;
    GFX_puts(gfx, draw);
    if (strike) {
        int16_t left;
        int16_t end = origin_x + todo_text_extent(gfx, draw, &left);
        int16_t start = origin_x + left;
        int16_t y = origin_y - gfx->u8g2.font_info.ascent_A / 2;
        if (start < m_todo_clip.left) start = m_todo_clip.left;
        if (end > m_todo_clip.right) end = m_todo_clip.right;
        if (y >= m_todo_clip.top && y < m_todo_clip.bottom && end > start)
            GFX_drawFastHLine(gfx, start, y, end - start, GFX_BLACK);
    }
}

/* TODO owns its pane from title to final row. Clock date/status components
 * deliberately do not constrain this layout or reduce the selected count. */
static void DrawTodo(Adafruit_GFX *gfx, const gui_data_t *data)
{
    todo_selection_t selection;
    const int16_t x = 23, first_y = 62, bottom = GUI_HEIGHT(data) - 30;
    int16_t right = data->mode == MODE_TODO ? GUI_WIDTH(data) - 25 : 367;
    const ui_component_t *calendar = ui_resource_component(UI_COMP_SPLIT_GRID);
    if (data->mode == MODE_TODO_CALENDAR && calendar != NULL && right > calendar->x - 10)
        right = calendar->x - 10;
    const int16_t width = right - x - 14;
    if (width < 1 || bottom - first_y < 24) {
        todo_items_suspend_display();
        s_todo_render_valid = false;
        return;
    }
    m_todo_clip.forward = gfx->u8g2.draw_hv_line;
    m_todo_clip.left = x; m_todo_clip.right = right;
    m_todo_clip.top = 12; m_todo_clip.bottom = bottom;
    gfx->u8g2.draw_hv_line = todo_clip_line;
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    /* Share the calendar frame geometry; split mode draws one continuous line. */
    if (data->mode == MODE_TODO) {
        const ui_component_t *top = ui_resource_component(UI_COMP_SPLIT_FRAME_TOP);
        if (top) GFX_fillRect(gfx, top->x, top->y, top->w, top->h, GFX_BLACK);
    }
    char temperature[9];
    gui_format_int(temperature, sizeof(temperature), data->temperature, 1U);
    const ui_component_t *calendar_header = ui_resource_component(UI_COMP_CAL_HEADER);
    gui_font_id_t temp_number_font = calendar_header ? (gui_font_id_t)calendar_header->font_primary : GUI_FONT_HELVB18;
    gui_font_id_t temp_unit_font = calendar_header ? (gui_font_id_t)calendar_header->font_secondary : GUI_FONT_WQY12;
    int16_t heading_width = right - x;
    gui_set_font(gfx, temp_number_font);
    int16_t number_advance = 0;
    for (const char *digit = temperature; *digit != '\0'; ++digit) {
        int8_t advance = 0;
        if (!u8g2_GetGlyphWidthIfPresent(&gfx->u8g2, (uint8_t)*digit, &advance))
            advance = gfx->u8g2.placeholder.advance;
        number_advance += advance;
    }
    gui_set_font(gfx, temp_unit_font);
    int16_t temp_width = number_advance + todo_text_width(gfx, "℃");
    if (temp_width + 8 < heading_width) {
        GFX_setCursor(gfx, right - temp_width, 36);
        gui_set_font(gfx, temp_number_font);
        GFX_puts(gfx, temperature);
        gui_set_font(gfx, temp_unit_font);
        GFX_puts(gfx, "℃");
        heading_width -= temp_width + 8;
    }
    gui_set_font(gfx, GUI_FONT_WQY9);
    GFX_setCursor(gfx, x, 36);
    const char *cursor;
    const gui_font_id_t font = (gui_font_id_t)todo_items_font_slot();
    if (font == GUI_FONT_TODO &&
        !GUI_TodoFontMatches(todo_items_font_size(), todo_items_font_key()))
        todo_items_suspend_display();
    if (!todo_items_select(data->timestamp, &selection)) {
        s_todo_render_valid = false;
        cursor = "TODO DATA ERROR";
        DrawTodoText(gfx, &cursor, heading_width, true, false);
        goto finish;
    }
    char heading[16] = "TODO ";
    size_t heading_pos = clock_text_append_u8(heading, sizeof(heading), 5U, selection.count);
    heading_pos = clock_text_append(heading, sizeof(heading), heading_pos, "/");
    clock_text_append_u8(heading, sizeof(heading), heading_pos, selection.daily_count);
    cursor = heading;
    DrawTodoText(gfx, &cursor, heading_width, true, false);
    if (selection.count == 0U) {
        GFX_setCursor(gfx, x, first_y + 20);
        cursor = "NO ACTIVE TASKS";
        DrawTodoText(gfx, &cursor, right - x, true, false);
        goto finish;
    }
    const int16_t row_height = (bottom - first_y - 4) / todo_items_display_limit();
    const gui_font_metrics_t *metrics = GUI_GetFontMetrics(font);
    int16_t descent = metrics != NULL ? metrics->max_char_height - metrics->ascent : 5;
    if (metrics != NULL && descent < -metrics->y_offset) descent = -metrics->y_offset;
    const bool extended = todo_items_extended_font();
    int16_t text_x = x + 14;
    int16_t text_width = width;
    int16_t first_baseline = 15, line_step = 18;
    bool two_lines;
    if (extended) {
        if (metrics == NULL || row_height < metrics->max_char_height + 6) {
            todo_items_suspend_display();
            s_todo_render_valid = false;
            goto finish;
        }
        /* Reserve the complete italic bearing on the first glyph as well as
         * its ascent/descent. Larger glyphs must not overlap the next row. */
        if (metrics->x_offset < 0) text_x -= metrics->x_offset;
        text_width = right - text_x;
        first_baseline = metrics->ascent + 3;
        line_step = metrics->max_char_height + 4;
        two_lines = row_height >= 2 * metrics->max_char_height + 10;
    } else {
        two_lines = row_height >= 52 + descent;
    }
    for (uint8_t row = 0U; row < selection.count; ++row) {
        int16_t y = first_y + row * row_height;
        if (!gui_page_hits(gfx, y, y + row_height)) continue;
        m_todo_clip.top = y; m_todo_clip.bottom = y + row_height;
        todo_item_t item;
        if (!todo_items_read(selection.index[row], &item)) {
            s_todo_render_valid = false;
            continue;
        }
        int16_t marker_y = y + (extended ? 3 + (metrics->max_char_height - 7) / 2 : 4);
        uint16_t marker_color = data->color > 1 &&
            (selection.phase[row] == TODO_PHASE_ACTIVE || selection.phase[row] == TODO_PHASE_EXPIRED) ? GFX_RED : GFX_BLACK;
        if (item.flags & TODO_ITEM_RESIDENT)
            GFX_fillRect(gfx, x, marker_y, 7, 7, marker_color);
        else
            GFX_drawRect(gfx, x, marker_y, 7, 7, marker_color);
        gui_set_font(gfx, font);
        GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
        cursor = item.text;
        GFX_setCursor(gfx, text_x, y + first_baseline);
        bool strike = selection.phase[row] == TODO_PHASE_EXPIRED;
        DrawTodoText(gfx, &cursor, text_width, !two_lines, strike);
        if (two_lines && *cursor != '\0') {
            GFX_setCursor(gfx, text_x, y + first_baseline + line_step);
            DrawTodoText(gfx, &cursor, text_width, true, strike);
        }
    }
finish:
    gfx->u8g2.draw_hv_line = m_todo_clip.forward;
}

/* Full and split clocks share the face and footer; their headers use different
 * arrangements from the external UI package. Both render in black for partial refresh. */

static void DrawClock(Adafruit_GFX* gfx, const tm_t* tm, const struct Lunar_Date* Lunar,
                      const gui_data_t* data) {
    bool fullscreen = data->mode == MODE_CLOCK;
    const ui_component_t *header = ui_resource_component(
        fullscreen ? UI_COMP_CLOCK_HEADER : UI_COMP_SPLIT_LEFT_HEADER);
    const ui_component_t *top_line = ui_resource_component(
        fullscreen ? UI_COMP_CLOCK_TOP_LINE : UI_COMP_SPLIT_LEFT_TOP_LINE);
    const ui_component_t *bottom_line = ui_resource_component(
        fullscreen ? UI_COMP_CLOCK_BOTTOM_LINE : UI_COMP_SPLIT_LEFT_BOTTOM_LINE);
    const ui_component_t *bottom = ui_resource_component(
        fullscreen ? UI_COMP_CLOCK_BOTTOM_INFO : UI_COMP_SPLIT_BOTTOM_INFO);
    if (!header || !top_line || !bottom_line || !bottom) return;

    if (gui_text_page_hits(gfx, header->font_primary, header->y,
                           fullscreen ? header->y : header->value0) ||
        gui_text_page_hits(gfx, header->font_secondary, header->y,
                           fullscreen ? header->y : header->value0) ||
        (fullscreen && gui_text_page_hits(gfx, header->font_tertiary,
                                          header->y - 2, header->value0))) {
        GFX_setCursor(gfx, header->x, header->y);
        DrawYmd(gfx, tm, GFX_BLACK, fullscreen, header);
        if (fullscreen) {
            GFX_setCursor(gfx, header->x, header->value0);
            gui_set_font(gfx, (gui_font_id_t)header->font_tertiary);
            GFX_puts(gfx, "星期");
            GFX_puts(gfx, LUNAR_WeekdayName(tm->tm_wday));
            GFX_setCursor(gfx, header->value1, header->value0);
            gui_print_lunar_md(gfx, Lunar);

            DrawVoltage(gfx, header->x + header->w, header->y - 11, data->voltage_mv,
                        (gui_font_id_t)header->font_tertiary);
            char ssid[5] = {0};
            int16_t sw = GFX_getUTF8Width(gfx, "25℃[1234]");
            clock_ssid_tail4(ssid, sizeof(ssid), data->ssid);
            GFX_setCursor(gfx, header->x + header->w - sw - 2, header->value0);
            gui_set_font(gfx, (gui_font_id_t)header->font_tertiary);
            gui_print_int(gfx, data->temperature, 1U);
            GFX_puts(gfx, "℃[");
            GFX_puts(gfx, ssid);
            GFX_puts(gfx, "]");
        } else {
            gui_set_font(gfx, (gui_font_id_t)header->font_secondary);
            GFX_setCursor(gfx, header->x, header->value0);
            gui_print_lunar_md(gfx, Lunar);

            /* Keep the existing numeric/unit font widths and alignment. */
            gui_set_font(gfx, (gui_font_id_t)header->font_primary);
            int8_t temp = data->temperature;
            int16_t digit_width = GFX_getUTF8Width(gfx, "0");
            int16_t temp_num_width = digit_width;
            if (temp < 0) {
                temp_num_width += GFX_getUTF8Width(gfx, "-");
                temp = -temp;
            }
            if (temp >= 10) temp_num_width += digit_width;
            if (temp >= 100) temp_num_width += digit_width;
            gui_set_font(gfx, (gui_font_id_t)header->font_secondary);
            int16_t total_temp_width = temp_num_width + GFX_getUTF8Width(gfx, "℃");
            gui_set_font(gfx, (gui_font_id_t)header->font_primary);
            GFX_setCursor(gfx, header->value1 - total_temp_width, header->value0);
            gui_print_int(gfx, data->temperature, 1U);
            gui_set_font(gfx, (gui_font_id_t)header->font_secondary);
            GFX_puts(gfx, "℃");
        }
    }

    GFX_drawFastHLine(gfx, top_line->x, top_line->y, top_line->w, GFX_BLACK);
    clock_refresh_geom_t geom = gui_clock_geom(data);
    DrawTimeFromGeom(gfx, tm, &geom);
    GFX_drawFastHLine(gfx, bottom_line->x, bottom_line->y, bottom_line->w, GFX_BLACK);

    if (!gui_text_page_hits(gfx, bottom->font_primary, bottom->y, bottom->value0)) return;
    gui_set_font(gfx, (gui_font_id_t)bottom->font_primary);
    GFX_setTextColor(gfx, GFX_BLACK, GFX_WHITE);
    GFX_setCursor(gfx, bottom->x, bottom->y);
    gui_print_lunar_year(gfx, Lunar);
    if (fullscreen) GFX_puts(gfx, LUNAR_ZodiacName(LUNAR_GetZodiac(Lunar)));
    GFX_puts(gfx, "年");

    GFX_setCursor(gfx, bottom->x, bottom->value0);
    if (fullscreen) GFX_puts(gfx, " ");
    gui_print_int(gfx, GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday), 1U);
    GFX_puts(gfx, "周");

    DrawJieqiRight(gfx, tm, fullscreen ? bottom->value1 : bottom->x + bottom->w,
                   bottom->y, bottom->value0, GFX_BLACK, GFX_BLACK,
                   (gui_font_id_t)bottom->font_primary);
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
    const ui_component_t *top = ui_resource_component(UI_COMP_SPLIT_FRAME_TOP);
    const ui_component_t *bottom = ui_resource_component(UI_COMP_SPLIT_FRAME_BOTTOM);
    if (!top || !bottom) return;
    const ui_component_t *separator = ui_resource_component(UI_COMP_SPLIT_SEPARATOR);
    GFX_fillRect(gfx, top->x, top->y, top->w, top->h, GFX_BLACK);
    int16_t frame_x = data->mode == MODE_TODO_CALENDAR && separator ? separator->x : bottom->x;
    GFX_fillRect(gfx, frame_x, bottom->y, bottom->x + bottom->w - frame_x, bottom->h, GFX_BLACK);

    // 在右侧绘制日历
    DrawCalendar_Right(gfx, tm, Lunar, data);

    // 在左侧绘制时钟
    if (data->mode == MODE_TODO_CALENDAR) DrawTodo(gfx, data);
    else DrawClock(gfx, tm, Lunar, data);
}

/* An application-only update may leave the external UI package absent.
 * Use a tiny built-in message so the panel does not refresh to blank white.
 * Column bits are 5x7 glyphs for N, O, D, A and T. */
static void DrawMissingResources(Adafruit_GFX* gfx, const gui_data_t* data) {
    static const uint8_t glyphs[5][5] = {
        {0x7f, 0x06, 0x18, 0x60, 0x7f}, /* N */
        {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* O */
        {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
        {0x7e, 0x09, 0x09, 0x09, 0x7e}, /* A */
        {0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
    };
    static const uint8_t message[] = {0, 1, 0xff, 2, 3, 4, 3}; /* NO DATA */
    const int16_t scale = 6;
    const int16_t text_w = (int16_t)(sizeof(message) * 6U - 1U) * scale;
    const int16_t x = (GUI_WIDTH(data) - text_w) / 2;
    const int16_t y = (GUI_HEIGHT(data) - 7 * scale) / 2;
    GFX_drawRect(gfx, x - 24, y - 24, text_w + 48, 7 * scale + 48, GFX_BLACK);
    for (unsigned i = 0; i < sizeof(message); ++i) {
        if (message[i] == 0xffU) continue;
        for (unsigned col = 0; col < 5U; ++col) {
            uint8_t bits = glyphs[message[i]][col];
            for (unsigned row = 0; row < 7U; ++row)
                if (bits & (1U << row))
                    GFX_fillRect(gfx, x + (int16_t)(i * 6U + col) * scale,
                                 y + (int16_t)row * scale, scale, scale, GFX_BLACK);
        }
    }
}

/**
 * @brief 按显示模式绘制完整界面
 *
 * @param data           GUI 数据
 * @param callback       分页缓冲回调
 * @param callback_data  回调用户数据
 *
 * 支持黑白/三色/四色；使用静态缓冲分页，入口仅可串行调用。
 */
void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data) {
    s_todo_render_valid = true;
    const bool todo_mode = data->mode == MODE_TODO || data->mode == MODE_TODO_CALENDAR;
    // 验证一周起始日参数
    if (data->week_start > 6) data->week_start = 0;

    tm_t tm = {0};
    struct Lunar_Date Lunar = {0};

    bool layout_ready = ui_resources_init();

    // 将UNIX时间戳转换为tm结构
    transformTime(data->timestamp, &tm);

    Adafruit_GFX *gfx = &m_gui_gfx;
    /* Static page buffer; page sizing must not depend on the heap reserve. */
    int16_t ph = 0;

    // 根据颜色模式初始化GFX
    if (data->color == 2)
        GFX_begin_3c(gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 三色屏幕
#if !GFX_FIXED_BWR_PANEL
    else if (data->color == 3)
        GFX_begin_4c(gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 四色屏幕
#endif
    else
        GFX_begin(gfx, GUI_WIDTH(data), GUI_HEIGHT(data), ph);  // 黑白屏幕
    gfx->u8g2.font_stop_after_io = todo_mode;

    if (layout_ready)
        LUNAR_SolarToLunar(&Lunar, tm.tm_year + YEAR0, tm.tm_mon + 1, tm.tm_mday);
    /* Components draw their own backgrounds; text state must not depend on
     * whether an earlier, nonintersecting page happened to visit a month cell. */
    GFX_setFontMode(gfx, 1);
    GFX_firstPage(gfx);
    do {
        // 填充白色背景
        GFX_fillScreen(gfx, GFX_WHITE);

        // 根据显示模式绘制不同界面
        if (!layout_ready) {
            DrawMissingResources(gfx, data);
        } else {
            switch (data->mode) {
                case MODE_CALENDAR:
                    DrawCalendar(gfx, &tm, &Lunar, data);  // 全屏日历
                    break;
                case MODE_CLOCK:
                    DrawClock(gfx, &tm, &Lunar, data);  // 全屏时钟
                    break;
                case MODE_TODO:
                    DrawTodo(gfx, data);
                    break;
                case MODE_CLOCK_CALENDAR:
                case MODE_TODO_CALENDAR:
                    DrawClockCalendar_Full(gfx, &tm, &Lunar, data);  // 分屏模式
                    break;
                default:
                    break;
            }
        }
        // 如果是2025年1月且未同步时间，显示同步提示
        if (layout_ready && (data->mode == MODE_CALENDAR || data->mode == MODE_CLOCK) &&
            (tm.tm_year + YEAR0 == 2025 && tm.tm_mon + 1 == 1)) {
            DrawTimeSyncTip(gfx, data);
        }
        /* Do not write a known invalid TODO page or render the remaining
         * pages. GFX_begin clears this frame's I/O latch on the next attempt. */
        if (todo_mode && (!s_todo_render_valid || gfx->u8g2.font_frame_io_error)) break;
    } while (GFX_nextPage(gfx, callback, callback_data));  // 分页绘制

    /* Later glyphs and font switches may recover their local read state,
     * while an earlier page already contains placeholders from failed I/O.
     * Keep that frame out of the TODO scheduling snapshot so main retries. */
    if (gfx->u8g2.font_frame_io_error) s_todo_render_valid = false;
    GFX_end(gfx);
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
    if (geom.canvas_w == 0U || geom.canvas_h == 0U) return;
    if (s_time_window_w == 0U || s_time_window_h == 0U) {
        s_time_window_x = s_time_window_y = 0U;
        s_time_window_w = geom.canvas_w;
        s_time_window_h = geom.canvas_h;
    }

    transformTime(data->timestamp, &tm);

    Adafruit_GFX *gfx = &m_gui_gfx;
    /* 页高 0：GFX_begin 使用静态页缓冲 */
    GFX_begin(gfx, geom.canvas_w, geom.canvas_h, 0);
    GFX_setWindow(gfx, s_time_window_x, s_time_window_y, s_time_window_w, s_time_window_h);

    GFX_firstPage(gfx);
    do {
        GFX_fillScreen(gfx, GFX_WHITE);
        DrawTime(gfx, &tm,
                 (int16_t)clock_hhmm_local_x_gap(geom.canvas_w, geom.cS, geom.nD,
                                                 geom.span_gap),
                 (int16_t)clock_hhmm_local_y(geom.canvas_h, geom.cS),
                 geom.cS, geom.nD);
    } while (GFX_nextPage(gfx, callback, callback_data));

    GFX_end(gfx);
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
