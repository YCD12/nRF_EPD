#include "GUI.h"

#include <stddef.h>

#include "Lunar.h"
#include "calendar_grid.h"
#include "calendar_annotations.h"
#include "calendar_resources.h"
#include "clock_layout.h"
#include "clock_text.h"
#include "fonts.h"
#include "resource_store.h"
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
static bool s_render_valid;
static uint16_t ui_color(uint8_t color);
static char m_todo_ellipsis[UI_TEXT_BYTES];

bool GUI_RenderValid(void) { return s_render_valid; }
bool GUI_TodoRenderValid(void) { return GUI_RenderValid(); }

/* Page culling is independent of the external layout's decorative lines. */
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

/* 拼「闰」+月份名，最多 9 字节。 */
static void format_lunar_month(char* out, const char* prefix, const char* month) {
    size_t pos = 0;

    while (*prefix != '\0' && pos < 9U) out[pos++] = *prefix++;
    while (*month != '\0' && pos < 9U) out[pos++] = *month++;
    out[pos] = '\0';
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
                           const gui_data_t* data,const int16_t *s) {
    int16_t x = layout->x;
    int16_t y = layout->y;
    int16_t span_width = layout->w;
    calendar_week_header_metrics_t hdr = {
        .col_w = (uint8_t)(span_width / 7),
        .remainder = (uint8_t)(span_width % 7),
        .row_h = layout->h,
    };

    gui_set_font(gfx, (gui_font_id_t)layout->font_primary);
    int16_t fh = (hdr.row_h - GFX_getFontHeight(gfx)) / 2 + GFX_getFontAscent(gfx) + s[UI_WEEK_BASELINE_SHIFT];
    int16_t cw = GFX_getUTF8Width(gfx, LUNAR_WeekdayName(0));
    for (int i = 0; i < 7; i++) {
        uint8_t day = (data->week_start + i) % 7;
        uint16_t bg = ui_color((day == 0 || day == 6) ? s[UI_WEEK_WEEKEND_BG] : s[UI_WEEK_WEEKDAY_BG]);
        GFX_fillRect(gfx, x + i * hdr.col_w, y, i == 6 ? (hdr.col_w + hdr.remainder) : hdr.col_w, hdr.row_h, bg);
        GFX_setTextColor(gfx, ui_color(layout->foreground), bg);
        GFX_setCursor(gfx, x + (hdr.col_w - cw) / 2 + i * hdr.col_w, y + fh);
        GFX_puts(gfx, LUNAR_WeekdayName(day));
    }
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
                          const ui_component_t *layout,const int16_t *s) {
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

    if (s[UI_GRID_GRID_DOT] > 0) {
        for (uint8_t i = 1; i < m.month_day_rows; i++)
            GFX_drawDottedLine(gfx, rect.x, rect.y + i * m.cell_h, rect.x + 7 * m.cell_w - 1,
                               rect.y + i * m.cell_h, ui_color(s[UI_GRID_GRID_COLOR]), s[UI_GRID_GRID_DOT], s[UI_GRID_GRID_GAP]);
        for (uint8_t i = 1; i < 7; i++)
            GFX_drawDottedLine(gfx, rect.x + i * m.cell_w, rect.y, rect.x + i * m.cell_w,
                               rect.y + m.month_day_rows * m.cell_h - 1, ui_color(s[UI_GRID_GRID_COLOR]), s[UI_GRID_GRID_DOT], s[UI_GRID_GRID_GAP]);
    }

    for (uint8_t i = 0; i < m.month_max_days; i++) {
        uint16_t year = tm->tm_year + YEAR0;
        uint8_t month = tm->tm_mon + 1;
        uint8_t day = i + 1;
        calendar_cell_pos_t cell = calendar_grid_cell(&rect, &m, i);
        cell.cr=s[UI_GRID_DATE_HALF];
        cell.bx=rect.x+(m.cell_w-2*cell.cr)/2+cell.display_week*m.cell_w;
        cell.by=rect.y+(m.cell_h-2*cell.cr)/2+((i+m.adjusted_first_day)/7)*m.cell_h+s[UI_GRID_CELL_Y];

        /* Include overhanging date circles, labels and holiday corner marks.
         * Drawing order within every intersecting page remains unchanged. */
        int16_t cell_y;
        calendar_grid_cell_origin(&rect, &m, i, NULL, &cell_y);
        if (!gui_page_hits(gfx, cell_y - 192, cell_y + m.cell_h + 192)) {
            continue;
        }

        LUNAR_SolarToLunar(&dayLunar, year, month, day);

        if (day == tm->tm_mday) {
            GFX_fillCircle(gfx, cell.bx + cell.cr, cell.by + cell.cr + s[UI_GRID_TODAY_Y], s[UI_GRID_TODAY_RADIUS], ui_color(layout->accent));
            GFX_setTextColor(gfx, ui_color(s[UI_GRID_TODAY_TEXT]), ui_color(layout->accent));
        } else {
            GFX_setTextColor(gfx, ui_color(cell.weekend ? s[UI_GRID_WEEKEND_COLOR] : layout->foreground), ui_color(layout->background));
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
            if(!s[UI_GRID_ASCII_SMALL]) anno_pixel=anno.font_size ? anno.font_size :
                (m.large ? CALENDAR_ANNOTATION_FONT_LARGE : CALENDAR_ANNOTATION_FONT_SMALL);
        }

        GFX_setFontMode(gfx, 1);
        if (GetFestival(year, month, day, (uint8_t)cell.actual_week, &dayLunar, festival_buf)) {
            festival = festival_buf;
            if (day != tm->tm_mday) GFX_setTextColor(gfx, ui_color(s[UI_GRID_FESTIVAL_COLOR]), ui_color(layout->background));
        } else if (dayLunar.Date == 1) {
            format_lunar_month(festival_buf, LUNAR_LeapName(dayLunar.IsLeap),
                               LUNAR_MonthName(dayLunar.Month));
            festival = festival_buf;
        } else {
            festival = LUNAR_DateName(dayLunar.Date);
        }
        if (replace_below) {
            festival = anno.label;
            gui_set_font(gfx, (gui_font_id_t)s[anno_pixel==CALENDAR_ANNOTATION_FONT_SMALL?UI_GRID_ANNOTATION_SMALL_FONT:UI_GRID_ANNOTATION_LARGE_FONT]);
            GFX_setTextColor(gfx, anno_fg, ui_color((day == tm->tm_mday) ? layout->accent : layout->background));
        } else {
            gui_set_font(gfx, (gui_font_id_t)layout->font_secondary);
        }
        GFX_setFontMode(gfx,1);
        GFX_setCursor(gfx, cell.bx + (2 * cell.cr - GFX_getUTF8Width(gfx, festival)) / 2,
                      gfx->ty + GFX_getFontHeight(gfx) + s[UI_GRID_LABEL_GAP]);
        GFX_puts(gfx, festival);

        if (has_anno && calendar_annotation_draws_corner(&anno)) {
            int16_t mark_x;
            int16_t mark_y;

            gui_set_font(gfx, (gui_font_id_t)s[anno_pixel==CALENDAR_ANNOTATION_FONT_SMALL?UI_GRID_ANNOTATION_SMALL_FONT:UI_GRID_ANNOTATION_LARGE_FONT]);
            GFX_setFontMode(gfx,1);
            GFX_setTextColor(gfx, anno_fg, ui_color(layout->background));
            int16_t cell_x,cell_y;
            calendar_grid_cell_origin(&rect,&m,i,&cell_x,&cell_y);
            mark_x=cell_x+m.cell_w-s[UI_GRID_CORNER_PAD]-GFX_getUTF8Width(gfx,anno.label);
            if(mark_x<cell_x+s[UI_GRID_CORNER_PAD]) mark_x=cell_x+s[UI_GRID_CORNER_PAD];
            mark_y=cell_y+s[UI_GRID_CORNER_PAD]+GFX_getFontAscent(gfx);
            GFX_setCursor(gfx, mark_x, mark_y);
            GFX_puts(gfx, anno.label);
        }

        if (has_holiday) {
            calendar_holiday_mark_t holiday;
            holiday.circle_r=s[UI_GRID_HOLIDAY_RADIUS];
            if(has_anno && calendar_annotation_draws_corner(&anno)) {
                int16_t cx,cy;calendar_grid_cell_origin(&rect,&m,i,&cx,&cy);
                holiday.text_x=cx+s[UI_GRID_HOLIDAY_PAD];
                holiday.text_y=cy+m.cell_h-s[UI_GRID_HOLIDAY_PAD];
                holiday.circle_x=holiday.text_x+holiday.circle_r;
                holiday.circle_y=holiday.text_y+s[UI_GRID_HOLIDAY_BASELINE];
            } else {
                holiday.text_x=cell.bx+s[UI_GRID_HOLIDAY_TEXT_X];
                holiday.text_y=cell.by+s[UI_GRID_HOLIDAY_TEXT_Y];
                holiday.circle_x=cell.bx+s[UI_GRID_HOLIDAY_CIRCLE_X];
                holiday.circle_y=cell.by+s[UI_GRID_HOLIDAY_CIRCLE_Y];
            }

            if (day == tm->tm_mday) {
                GFX_fillCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r,
                               ui_color(layout->background));
                GFX_drawCircle(gfx, holiday.circle_x, holiday.circle_y, holiday.circle_r, ui_color(layout->accent));
            }
            gui_set_font(gfx, (gui_font_id_t)layout->font_tertiary);
            GFX_setFontMode(gfx,1);
            GFX_setTextColor(gfx, ui_color(work ? s[UI_GRID_WORK_COLOR] : s[UI_GRID_REST_COLOR]), ui_color(layout->background));
            GFX_setCursor(gfx, holiday.text_x, holiday.text_y);
            char label[UI_TEXT_BYTES];
            if (ui_resource_text(layout, work ? 0 : 1, label, sizeof(label)))
                GFX_puts(gfx, label);
            else
                s_render_valid = false;
        }

        if (has_anno && calendar_annotation_draws_dot(&anno)) {
            int16_t mark_x;
            int16_t mark_y;
            uint8_t mark_r;

            int16_t cx,cy;calendar_grid_cell_origin(&rect,&m,i,&cx,&cy);
            mark_x=cx+(has_holiday?s[UI_GRID_DOT_PAD]:m.cell_w-s[UI_GRID_DOT_PAD]);
            mark_y=cy+s[UI_GRID_DOT_Y];mark_r=s[UI_GRID_DOT_RADIUS];
            /* 当日红底在分屏小格中会延伸到角落，白边让红点保持可辨认。 */
            if (day == tm->tm_mday) {
                GFX_fillCircle(gfx, mark_x, mark_y, mark_r + s[UI_GRID_DOT_BORDER], ui_color(layout->background));
            }
            GFX_fillCircle(gfx, mark_x, mark_y, mark_r, ui_color(layout->accent));
        }
    }
}

static void Draw7Segment(Adafruit_GFX* gfx, uint16_t x, uint16_t y,
                         uint16_t cS, uint16_t horizontal_width,
                         uint16_t vertical_width, bool horizontal,
                         uint16_t color)
{
    /* Both orientations use the same widening/narrowing scanline profile. */
    for (uint16_t i = 0U; i < 2U * cS; ++i) {
        uint16_t inset = i < cS ? cS - i : i - cS;
        uint16_t length = (horizontal ? horizontal_width : vertical_width) + 2U * (cS - inset);
        if (horizontal)
            GFX_drawFastHLine(gfx, (int16_t)(x + inset), (int16_t)(y + i), (int16_t)length, color);
        else
            GFX_drawFastVLine(gfx, (int16_t)(x + i), (int16_t)(y + inset), (int16_t)length, color);
    }
}

/* 仅用于时钟 HH/MM，数字右对齐，前面补 0。宽度公式与 clock_layout.h 保持一致。 */
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

/* Measure the furthest painted pixel, including placeholder glyphs and the
 * actual advances between letters. Separately adding the widths of a title
 * and its ellipsis loses the final title glyph's trailing advance. */
static int16_t todo_text_extent(Adafruit_GFX *gfx, const char *text, int16_t *left,int16_t *advance_out)
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
    if(advance_out) *advance_out=cursor;
    return right;
}

static int16_t todo_text_width(Adafruit_GFX *gfx, const char *text)
{
    return todo_text_extent(gfx, text, NULL,NULL);
}

/* Draw a bounded UTF-8 line, wrapping at glyph boundaries. The final line
 * reserves space for an ellipsis when the title is wider than the pane. */
static void DrawTodoText(Adafruit_GFX *gfx, const char **cursor, int16_t width,
                         bool last_line, bool strike,const int16_t *s)
{
    char line[TODO_ITEMS_TEXT_MAX + UI_TEXT_BYTES];
    size_t length = 0U;
    const char *text = *cursor;
    const char *draw = text;
    if (width <= 0) return;
    if (todo_text_width(gfx, text) <= width) {
        *cursor = text + strlen(text);
        goto draw_line;
    }
    const char *dots = last_line ? m_todo_ellipsis : "";
    while (*dots != '\0' && todo_text_width(gfx, dots) > width)
        dots+=(uint8_t)*dots<0x80U?1U:(uint8_t)*dots<0xe0U?2U:3U;
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
        int16_t end = origin_x + todo_text_extent(gfx, draw, &left,NULL);
        int16_t start = origin_x + left;
        int16_t y = origin_y - gfx->u8g2.font_info.ascent_A / s[UI_TODO_STRIKE_DIVISOR];
        if (start < gfx->clip_left) start = gfx->clip_left;
        if (end > gfx->clip_right) end = gfx->clip_right;
        if (y >= gfx->clip_top && y < gfx->clip_bottom && end > start)
            GFX_fillRect(gfx, start, y, end - start, s[UI_TODO_STRIKE_WIDTH], ui_color(s[UI_TODO_TEXT_COLOR]));
    }
}

/* TODO owns its pane from title to final row. Clock date/status components
 * deliberately do not constrain this layout or reduce the selected count. */
#include "ui_render.h"

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
    s_render_valid = true;
    resource_store_render_begin();
    // 验证一周起始日参数
    if (data->week_start > 6) data->week_start = 0;

    tm_t tm = {0};
    struct Lunar_Date Lunar = {0};

    bool layout_ready = ui_resources_retry_init();
    if (layout_ready) (void)calendar_resources_retry_init();

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
    gfx->u8g2.font_stop_after_io = true;

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
            DrawResourceScene(gfx,&tm,&Lunar,data);
        }
        /* Do not write a known invalid page or render the remaining
         * pages. GFX_begin clears this frame's I/O latch on the next attempt. */
        if (!resource_store_render_valid() || gfx->u8g2.font_frame_io_error)
            s_render_valid = false;
        if (!s_render_valid) break;
    } while (GFX_nextPage(gfx, callback, callback_data));  // 分页绘制

    /* Later glyphs and font switches may recover their local read state,
     * while an earlier page already contains placeholders from failed I/O.
     * Do not allow any mode to commit that frame or advance its baseline. */
    if (gfx->u8g2.font_frame_io_error) s_render_valid = false;
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
    s_render_valid = false;
    resource_store_render_begin();
    if (!ui_resources_retry_init()) return;
    tm_t tm = {0};
    clock_refresh_geom_t geom = gui_clock_geom(data);
    if (geom.canvas_w == 0U || geom.canvas_h == 0U) return;
    s_render_valid = true;
    if (s_time_window_w == 0U || s_time_window_h == 0U) {
        s_time_window_x = s_time_window_y = 0U;
        s_time_window_w = geom.canvas_w;
        s_time_window_h = geom.canvas_h;
    }

    transformTime(data->timestamp, &tm);

    Adafruit_GFX *gfx = &m_gui_gfx;
    /* 页高 0：GFX_begin 使用静态页缓冲 */
    GFX_begin(gfx, geom.canvas_w, geom.canvas_h, 0);
    gfx->u8g2.font_stop_after_io = true;
    GFX_setWindow(gfx, s_time_window_x, s_time_window_y, s_time_window_w, s_time_window_h);

    GFX_firstPage(gfx);
    do {
        GFX_fillScreen(gfx, GFX_WHITE);
        const ui_component_t *clock = ui_resource_component(
            data->mode == MODE_CLOCK ? UI_COMP_CLOCK_TIME : UI_COMP_SPLIT_TIME);
        int16_t style[UI_MAX_PARAMS];
        if (!ui_resource_params(clock, style)) {
            s_render_valid = false;
            break;
        }
        DrawClockFace(gfx, &tm, clock, style, true);
        if (!resource_store_render_valid() || gfx->u8g2.font_frame_io_error) {
            s_render_valid = false;
            break;
        }
    } while (GFX_nextPage(gfx, callback, callback_data));

    GFX_end(gfx);
}

/* 画布局部窗 → 屏幕矩形，并记下 ClockOnly 窗口。 */
void GetTimeRefreshAreaFromLocal(const gui_data_t *data,
                                 uint16_t lx, uint16_t ly, uint16_t lw, uint16_t lh,
                                 uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h) {
    s_time_window_x = lx;
    s_time_window_y = ly;
    s_time_window_w = lw;
    s_time_window_h = lh;

    /* Strip rendering only sets the window; it does not need a second
     * external geometry read before DrawGUI_ClockOnly validates its own. */
    if (!x && !y && !w && !h) return;
    clock_refresh_geom_t geom = gui_clock_geom(data);
    bool valid = geom.canvas_w != 0U && geom.canvas_h != 0U;
    if (x) *x = (uint16_t)(geom.origin_x + lx);
    if (y) *y = (uint16_t)(geom.origin_y + ly);
    if (w) *w = valid ? lw : 0U;
    if (h) *h = valid ? lh : 0U;
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
