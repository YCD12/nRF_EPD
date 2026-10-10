#ifndef __FONTS_H
#define __FONTS_H

#include <stdint.h>
#include <stdbool.h>

#include "u8g2_font.h"

#ifndef GUI_USE_EXTERNAL_FONTS
#define GUI_USE_EXTERNAL_FONTS 1
#endif

#ifndef GUI_INCLUDE_INTERNAL_FONTS
#define GUI_INCLUDE_INTERNAL_FONTS 0
#endif

typedef enum {
    GUI_FONT_WQY9 = 0,
    GUI_FONT_WQY12,
    GUI_FONT_HELVB18,
    GUI_FONT_TODO,
} gui_font_id_t;

typedef u8g2_font_placeholder_t gui_font_metrics_t;

/**
 * 字库收录：
 *
 * ASCII (32-128)
 * 正月二月三月四月五月六月七月八月九月十月冬月腊月闰
 * 初一~三十
 * 星期一二三四五六日周
 * 猴鸡狗猪鼠牛虎兔龙蛇马羊
 * 庚辛壬癸甲乙丙丁戊己
 * 申酉戌亥子丑寅卯辰巳午未
 * 二十四节气
 * 年月日 / 离还有天 / ℃
 * 元旦节情人节妇女节植树节愚人节清明节劳动节青年节儿童节建党节建军节教师节国庆节
 * 母亲节父亲节万圣节感恩节平安夜圣诞节
 * 春节元宵节龙抬头端午节七夕节中元节中秋节重阳节寒衣节腊八节除夕
 * 休班
 */

#if GUI_INCLUDE_INTERNAL_FONTS
// 文泉驿 9 号点阵（农历专用）
extern const uint8_t u8g2_font_wqy9_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy9_t_lunar");

// 文泉驿 12 号点阵（农历专用）
extern const uint8_t u8g2_font_wqy12_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy12_t_lunar");

// u8g2 数字字体。Helvetica 18 号粗体（仅数字）。本板 800x480 不链接 14 号。
extern const uint8_t u8g2_font_helvB18_tn[] U8G2_FONT_SECTION("u8g2_font_helvB18_tn");
#endif

/* Initializes and probes FM25Q04B when external-font mode is enabled. */
uint8_t GUI_ExternalFontsInit(void);

/* Returns a font source only after the external Flash probe succeeded. */
const u8g2_font_source_t* GUI_GetExternalFont(gui_font_id_t font_id);

const gui_font_metrics_t* GUI_GetFontMetrics(gui_font_id_t font_id);
bool GUI_TodoFontMatches(uint8_t size, uint32_t key);

#endif
