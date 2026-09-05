#ifndef __FONTS_H
#define __FONTS_H

#include "u8g2_font.h"

/**
 * 文字列表:
 * Character List:
 *
所有 ASCII 字符 (32-128)
All ASCII characters (32-128)

正月二月三月四月五月六月七月八月九月十月冬月腊月闰
Lunar months

初一初二初三初四初五初六初七初八初九初十
Lunar days 1-10

十一十二十三十四十五十六十七十八十九二十
Lunar days 11-20

廿一廿二廿三廿四廿五廿六廿七廿八廿九三十
Lunar days 21-30

星期一二三四五六日周
Weekdays

猴鸡狗猪鼠牛虎兔龙蛇马羊
Chinese zodiac animals

庚辛壬癸甲乙丙丁戊己
Heavenly stems

申酉戌亥子丑寅卯辰巳午未
Earthly branches

小寒大寒立春雨水惊蛰春分清明谷雨立夏小满芒种夏至小暑大暑立秋处暑白露秋分寒露霜降立冬小雪大雪冬至
24 solar terms

年月日
Year, month, day

离还有天
Days until/remaining

℃
Celsius symbol

元旦节情人节妇女节植树节愚人节清明节劳动节青年节儿童节建党节建军节教师节国庆节
Common holidays

母亲节父亲节万圣节感恩节平安夜圣诞节
Western holidays

春节元宵节龙抬头端午节七夕节中元节中秋节重阳节寒衣节腊八节除夕
Traditional Chinese festivals

休班
Work/rest schedule
 */

// 文泉驿9号点阵字体（农历专用）
// WenQuanYi 9pt bitmap font (for lunar calendar)
extern const uint8_t u8g2_font_wqy9_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy9_t_lunar");

// 文泉驿12号点阵字体（农历专用）
// WenQuanYi 12pt bitmap font (for lunar calendar)
extern const uint8_t u8g2_font_wqy12_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy12_t_lunar");

// 以下字库来自 u8g2，用于显示数字
// Following fonts are from u8g2 library, used for displaying numbers

// Helvetica 14号粗体数字字体（仅数字）
// Helvetica Bold 14pt (numbers only)
extern const uint8_t u8g2_font_helvB14_tn[] U8G2_FONT_SECTION("u8g2_font_helvB14_tn");

// Helvetica 18号粗体数字字体（仅数字）
// Helvetica Bold 18pt (numbers only)
extern const uint8_t u8g2_font_helvB18_tn[] U8G2_FONT_SECTION("u8g2_font_helvB18_tn");
#endif
