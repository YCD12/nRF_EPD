/*
  U8g2_for_Adafruit_GFX.h

  Add unicode support and U8g2 fonts to Adafruit GFX libraries.
  为 Adafruit GFX 库添加 Unicode 支持和 U8g2 字体

  U8g2 for Adafruit GFX Lib (https://github.com/olikraus/U8g2_for_Adafruit_GFX)

  Copyright (c) 2018, olikraus@gmail.com
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list
    of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice, this
    list of conditions and the following disclaimer in the documentation and/or other
    materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
  CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/
#ifndef __U8G2_H
#define __U8G2_H

#include <stdint.h>

#ifdef __GNUC__
#define U8X8_NOINLINE __attribute__((noinline))
#define U8X8_SECTION(name) __attribute__((section(name)))
#define U8X8_UNUSED __attribute__((unused))
#else
#define U8X8_SECTION(name)
#define U8X8_NOINLINE
#define U8X8_UNUSED
#endif

#if defined(__GNUC__) && defined(__AVR__)
#define U8X8_FONT_SECTION(name) U8X8_SECTION(".progmem." name)
#define u8x8_pgm_read(adr) pgm_read_byte_near(adr)
#define U8X8_PROGMEM PROGMEM
#endif

#ifndef U8X8_FONT_SECTION
#define U8X8_FONT_SECTION(name)
#endif

#ifndef u8x8_pgm_read
#define u8x8_pgm_read(adr) (*(const uint8_t*)(adr))
#endif

#ifndef U8X8_PROGMEM
#define U8X8_PROGMEM
#endif

#define U8G2_FONT_SECTION(name) U8X8_FONT_SECTION(name)

/* the macro U8G2_USE_LARGE_FONTS enables large fonts (>32K) */
/* it can be enabled for those uC supporting larger arrays */
/* U8G2_USE_LARGE_FONTS 宏用于启用大字体（>32K） */
/* 可在支持大数组的微控制器上启用 */
#if defined(unix) || defined(__arm__) || defined(__arc__) || defined(ESP8266) || defined(ESP_PLATFORM)
#ifndef U8G2_USE_LARGE_FONTS
#define U8G2_USE_LARGE_FONTS
#endif
#endif

// 字体信息结构体，包含字体的所有元数据
typedef struct _u8g2_font_info_t {
    /* offset 0 */
    uint8_t glyph_cnt;         // 字形数量
    uint8_t bbx_mode;          // 边界框模式
    uint8_t bits_per_0;        // 背景位数
    uint8_t bits_per_1;        // 前景位数

    /* offset 4 */
    uint8_t bits_per_char_width;   // 字符宽度编码位数
    uint8_t bits_per_char_height;  // 字符高度编码位数
    uint8_t bits_per_char_x;       // X偏移编码位数
    uint8_t bits_per_char_y;       // Y偏移编码位数
    uint8_t bits_per_delta_x;      // X增量编码位数

    /* offset 9 */
    int8_t max_char_width;     // 最大字符宽度
    int8_t max_char_height; /* overall height, NOT ascent. Instead ascent = max_char_height + y_offset */
                            /* 总体高度，不是上升高度。上升高度 = max_char_height + y_offset */
    int8_t x_offset;           // X偏移
    int8_t y_offset;           // Y偏移

    /* offset 13 */
    int8_t ascent_A;           // 大写A的上升高度
    int8_t descent_g; /* usually a negative value */
                      /* 小写g的下降高度（通常为负值） */
    int8_t ascent_para;        // 段落上升高度
    int8_t descent_para;       // 段落下降高度

    /* offset 17 */
    uint16_t start_pos_upper_A;  // 大写字母A的起始位置
    uint16_t start_pos_lower_a;  // 小写字母a的起始位置

    /* offset 21 */
    uint16_t start_pos_unicode;  // Unicode字符的起始位置
} u8g2_font_info_t;

// 字体解码结构体，用于解码和绘制字形
typedef struct _u8g2_font_decode_t {
    const uint8_t* decode_ptr; /* pointer to the compressed data */
                               /* 指向压缩数据的指针 */

    int16_t target_x;          // 目标X坐标
    int16_t target_y;          // 目标Y坐标
    uint16_t fg_color;         // 前景色
    uint16_t bg_color;         // 背景色

    int8_t x; /* local coordinates, (0,0) is upper left */
              /* 局部X坐标，(0,0)为左上角 */
    int8_t y; // 局部Y坐标
    int8_t glyph_width;        // 字形宽度
    int8_t glyph_height;       // 字形高度

    uint8_t decode_bit_pos; /* bitpos inside a byte of the compressed data */
                            /* 压缩数据字节内的位位置 */
    uint8_t is_transparent;    // 透明模式标志
    uint8_t dir; /* direction */
                 /* 绘制方向 */
} u8g2_font_decode_t;

// 字体主结构体
typedef struct _u8g2_font_t {
    const uint8_t* font; /* current font for all text procedures */
                         /* 当前字体数据指针，用于所有文本操作 */

    u8g2_font_decode_t font_decode; /* new font decode structure */
                                    /* 字体解码结构 */
    u8g2_font_info_t font_info;     /* new font info structure */
                                    /* 字体信息结构 */

    int8_t glyph_x_offset; /* set by u8g2_GetGlyphWidth as a side effect */
                           /* 字形X偏移，由 u8g2_GetGlyphWidth 设置 */

    void (*draw_hv_line)(struct _u8g2_font_t* u8g2, int16_t x, int16_t y, int16_t len, uint8_t dir, uint16_t color);
    // 绘制水平/垂直线的回调函数
} u8g2_font_t;

// 检查指定编码的字形是否存在
uint8_t u8g2_IsGlyph(u8g2_font_t* u8g2, uint16_t requested_encoding);

// 获取字形宽度（返回X增量）
int8_t u8g2_GetGlyphWidth(u8g2_font_t* u8g2, uint16_t requested_encoding);

// 设置字体模式（透明/不透明）
void u8g2_SetFontMode(u8g2_font_t* u8g2, uint8_t is_transparent);

// 设置字体绘制方向（0/1/2/3 对应 0°/90°/180°/270°）
void u8g2_SetFontDirection(u8g2_font_t* u8g2, uint8_t dir);

// 绘制单个字形（支持 ASCII 和 Unicode）
int16_t u8g2_DrawGlyph(u8g2_font_t* u8g2, int16_t x, int16_t y, uint16_t encoding);

// 绘制字符串
int16_t u8g2_DrawStr(u8g2_font_t* u8g2, int16_t x, int16_t y, const char* s);

// 设置字体
void u8g2_SetFont(u8g2_font_t* u8g2, const uint8_t* font);

// 设置前景色
void u8g2_SetForegroundColor(u8g2_font_t* u8g2, uint16_t fg);

// 设置背景色
void u8g2_SetBackgroundColor(u8g2_font_t* u8g2, uint16_t bg);

#endif
