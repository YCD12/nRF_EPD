#ifndef __FONTS_H
#define __FONTS_H

#include "u8g2_font.h"

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

// 文泉驿 9 号点阵（农历专用）
extern const uint8_t u8g2_font_wqy9_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy9_t_lunar");

// 文泉驿 12 号点阵（农历专用）
extern const uint8_t u8g2_font_wqy12_t_lunar[] U8G2_FONT_SECTION("u8g2_font_wqy12_t_lunar");

// u8g2 数字字体。Helvetica 18 号粗体（仅数字）。本板 800x480 不链接 14 号。
extern const uint8_t u8g2_font_helvB18_tn[] U8G2_FONT_SECTION("u8g2_font_helvB18_tn");
#endif
