/*
    U8g2_for_Adafruit_GFX.cpp

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

#include "u8g2_font.h"

#include <stddef.h>

// 从字体数据中读取单个字节
static uint8_t u8g2_font_get_byte(const uint8_t* font, uint8_t offset) {
    font += offset;
    return u8x8_pgm_read(font);
}

// 从字体数据中读取双字节（大端序）
static uint16_t u8g2_font_get_word(const uint8_t* font, uint8_t offset) U8X8_NOINLINE;
static uint16_t u8g2_font_get_word(const uint8_t* font, uint8_t offset) {
    uint16_t pos;
    font += offset;
    pos = u8x8_pgm_read(font);
    font++;
    pos <<= 8;
    pos += u8x8_pgm_read(font);
    return pos;
}

/*========================================================================*/
/* new font format */
/* 新字体格式 */
// 读取字体信息结构（从字体数据的头部读取元数据）
void u8g2_read_font_info(u8g2_font_info_t* font_info, const uint8_t* font) {
    /* offset 0 */
    font_info->glyph_cnt = u8g2_font_get_byte(font, 0);
    font_info->bbx_mode = u8g2_font_get_byte(font, 1);
    font_info->bits_per_0 = u8g2_font_get_byte(font, 2);
    font_info->bits_per_1 = u8g2_font_get_byte(font, 3);

    /* offset 4 */
    font_info->bits_per_char_width = u8g2_font_get_byte(font, 4);
    font_info->bits_per_char_height = u8g2_font_get_byte(font, 5);
    font_info->bits_per_char_x = u8g2_font_get_byte(font, 6);
    font_info->bits_per_char_y = u8g2_font_get_byte(font, 7);
    font_info->bits_per_delta_x = u8g2_font_get_byte(font, 8);

    /* offset 9 */
    font_info->max_char_width = u8g2_font_get_byte(font, 9);
    font_info->max_char_height = u8g2_font_get_byte(font, 10);
    font_info->x_offset = u8g2_font_get_byte(font, 11);
    font_info->y_offset = u8g2_font_get_byte(font, 12);

    /* offset 13 */
    font_info->ascent_A = u8g2_font_get_byte(font, 13);
    font_info->descent_g = u8g2_font_get_byte(font, 14);
    font_info->ascent_para = u8g2_font_get_byte(font, 15);
    font_info->descent_para = u8g2_font_get_byte(font, 16);

    /* offset 17 */
    font_info->start_pos_upper_A = u8g2_font_get_word(font, 17);
    font_info->start_pos_lower_a = u8g2_font_get_word(font, 19);

    /* offset 21 */
    font_info->start_pos_unicode = u8g2_font_get_word(font, 21);
}

// 获取字体边界框宽度
uint8_t u8g2_GetFontBBXWidth(u8g2_font_t* u8g2) { return u8g2->font_info.max_char_width; /* new font info structure */ }

// 获取字体边界框高度
uint8_t u8g2_GetFontBBXHeight(u8g2_font_t* u8g2) {
    return u8g2->font_info.max_char_height; /* new font info structure */
}

// 获取字体边界框X偏移
int8_t u8g2_GetFontBBXOffX(u8g2_font_t* u8g2) { return u8g2->font_info.x_offset; /* new font info structure */ }

// 获取字体边界框Y偏移
int8_t u8g2_GetFontBBXOffY(u8g2_font_t* u8g2) { return u8g2->font_info.y_offset; /* new font info structure */ }

// 获取大写字母A的高度
uint8_t u8g2_GetFontCapitalAHeight(u8g2_font_t* u8g2) { return u8g2->font_info.ascent_A; /* new font info structure */ }

// 从压缩字体数据中读取无符号位
static uint8_t u8g2_font_decode_get_unsigned_bits(u8g2_font_decode_t* f, uint8_t cnt) U8X8_NOINLINE;
static uint8_t u8g2_font_decode_get_unsigned_bits(u8g2_font_decode_t* f, uint8_t cnt) {
    uint8_t val;
    uint8_t bit_pos = f->decode_bit_pos;
    uint8_t bit_pos_plus_cnt;

    // val = *(f->decode_ptr);
    val = u8x8_pgm_read(f->decode_ptr);

    val >>= bit_pos;
    bit_pos_plus_cnt = bit_pos;
    bit_pos_plus_cnt += cnt;
    if (bit_pos_plus_cnt >= 8) {
        uint8_t s = 8;
        s -= bit_pos;
        f->decode_ptr++;
        // val |= *(f->decode_ptr) << (8-bit_pos);
        val |= u8x8_pgm_read(f->decode_ptr) << (s);
        // bit_pos -= 8;
        bit_pos_plus_cnt -= 8;
    }
    val &= (1U << cnt) - 1;
    // bit_pos += cnt;

    f->decode_bit_pos = bit_pos_plus_cnt;
    return val;
}

/*
        2 bit --> cnt = 2
            -2,-1,0. 1

        3 bit --> cnt = 3
            -2,-1,0. 1
            -4,-3,-2,-1,0,1,2,3

            if ( x < 0 )
    r = bits(x-1)+1;
        else
    r = bits(x)+1;

*/
/* optimized */
// 从压缩字体数据中读取有符号位
static int8_t u8g2_font_decode_get_signed_bits(u8g2_font_decode_t* f, uint8_t cnt) U8X8_NOINLINE;
static int8_t u8g2_font_decode_get_signed_bits(u8g2_font_decode_t* f, uint8_t cnt) {
    int8_t v, d;
    v = (int8_t)u8g2_font_decode_get_unsigned_bits(f, cnt);
    d = 1;
    cnt--;
    d <<= cnt;
    v -= d;
    return v;
    // return (int8_t)u8g2_font_decode_get_unsigned_bits(f, cnt) - ((1<<cnt)>>1);
}

// 根据方向添加Y向量
static int16_t u8g2_add_vector_y(int16_t dy, int8_t x, int8_t y, uint8_t dir) U8X8_NOINLINE;
static int16_t u8g2_add_vector_y(int16_t dy, int8_t x, int8_t y, uint8_t dir) {
    switch (dir) {
        case 0:
            dy += y;
            break;
        case 1:
            dy += x;
            break;
        case 2:
            dy -= y;
            break;
        default:
            dy -= x;
            break;
    }
    return dy;
}

// 根据方向添加X向量
static int16_t u8g2_add_vector_x(int16_t dx, int8_t x, int8_t y, uint8_t dir) U8X8_NOINLINE;
static int16_t u8g2_add_vector_x(int16_t dx, int8_t x, int8_t y, uint8_t dir) {
    switch (dir) {
        case 0:
            dx += x;
            break;
        case 1:
            dx -= y;
            break;
        case 2:
            dx -= x;
            break;
        default:
            dx += y;
            break;
    }
    return dx;
}

/*
    Description:
        Draw a run-length area of the glyph. "len" can have any size and the line
        length has to be wrapped at the glyph border.
    描述：
        绘制字形的游程长度区域。"len"可以是任意大小，线长度需要在字形边界处换行。
    Args:
        len:          Length of the line
                      线的长度
        is_foreground     foreground/background?
                          前景/背景？
        u8g2->font_decode.target_x    X position
                                      X位置
        u8g2->font_decode.target_y    Y position
                                      Y位置
        u8g2->font_decode.is_transparent  Transparent mode
                                          透明模式
    Return:
        -
    Calls:
        u8g2_Draw90Line()
    Called by:
        u8g2_font_decode_glyph()
*/
/* optimized */
// 解码并绘制字形的游程长度数据
static void u8g2_font_decode_len(u8g2_font_t* u8g2, uint8_t len, uint8_t is_foreground) {
    uint8_t cnt;     /* total number of remaining pixels, which have to be drawn */
    uint8_t rem;     /* remaining pixel to the right edge of the glyph */
    uint8_t current; /* number of pixels, which need to be drawn for the draw procedure */
    /* current is either equal to cnt or equal to rem */

    /* local coordinates of the glyph */
    uint8_t lx, ly;

    /* target position on the screen */
    int16_t x, y;

    u8g2_font_decode_t* decode = &(u8g2->font_decode);

    cnt = len;

    /* get the local position */
    lx = decode->x;
    ly = decode->y;

    for (;;) {
        /* calculate the number of pixel to the right edge of the glyph */
        rem = decode->glyph_width;
        rem -= lx;

        /* calculate how many pixel to draw. This is either to the right edge */
        /* or lesser, if not enough pixel are left */
        current = rem;
        if (cnt < rem) current = cnt;

        /* now draw the line, but apply the rotation around the glyph target position */
        // u8g2_font_decode_draw_pixel(u8g2, lx,ly,current, is_foreground);

        /* get target position */
        x = decode->target_x;
        y = decode->target_y;

        /* apply rotation */
        x = u8g2_add_vector_x(x, lx, ly, decode->dir);
        y = u8g2_add_vector_y(y, lx, ly, decode->dir);

        /* draw foreground and background (if required) */
        if (current > 0) /* avoid drawing zero length lines, issue #4 */
        {
            if (is_foreground) {
                u8g2->draw_hv_line(u8g2, x, y, current, decode->dir, decode->fg_color);
            } else if (decode->is_transparent == 0) {
                u8g2->draw_hv_line(u8g2, x, y, current, decode->dir, decode->bg_color);
            }
        }

        /* check, whether the end of the run length code has been reached */
        if (cnt < rem) break;
        cnt -= rem;
        lx = 0;
        ly++;
    }
    lx += cnt;

    decode->x = lx;
    decode->y = ly;
}

// 设置字形解码器（初始化解码状态）
static void u8g2_font_setup_decode(u8g2_font_t* u8g2, const uint8_t* glyph_data) {
    u8g2_font_decode_t* decode = &(u8g2->font_decode);
    decode->decode_ptr = glyph_data;
    decode->decode_bit_pos = 0;

    /* 8 Nov 2015, this is already done in the glyph data search procedure */
    /*
    decode->decode_ptr += 1;
    decode->decode_ptr += 1;
    */

    decode->glyph_width = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_char_width);
    decode->glyph_height = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_char_height);
}

/*
    Description:
        Decode and draw a glyph.
    描述：
        解码并绘制一个字形
    Args:
        glyph_data:           Pointer to the compressed glyph data of the font
                              指向字体压缩字形数据的指针
        u8g2->font_decode.target_x    X position
                                      X位置
        u8g2->font_decode.target_y    Y position
                                      Y位置
        u8g2->font_decode.is_transparent  Transparent mode
                                          透明模式
    Return:
        Width (delta x advance) of the glyph.
        字形的宽度（X增量）
    Calls:
        u8g2_font_decode_len()
*/
/* optimized */
static int8_t u8g2_font_decode_glyph(u8g2_font_t* u8g2, const uint8_t* glyph_data) {
    uint8_t a, b;
    int8_t x, y;
    int8_t d;
    int8_t h;
    u8g2_font_decode_t* decode = &(u8g2->font_decode);

    u8g2_font_setup_decode(u8g2, glyph_data);
    h = u8g2->font_decode.glyph_height;

    x = u8g2_font_decode_get_signed_bits(decode, u8g2->font_info.bits_per_char_x);
    y = u8g2_font_decode_get_signed_bits(decode, u8g2->font_info.bits_per_char_y);
    d = u8g2_font_decode_get_signed_bits(decode, u8g2->font_info.bits_per_delta_x);

    if (decode->glyph_width > 0) {
        decode->target_x = u8g2_add_vector_x(decode->target_x, x, -(h + y), decode->dir);
        decode->target_y = u8g2_add_vector_y(decode->target_y, x, -(h + y), decode->dir);
        // u8g2_add_vector(&(decode->target_x), &(decode->target_y), x, -(h+y), decode->dir);

        /* reset local x/y position */
        decode->x = 0;
        decode->y = 0;

        /* decode glyph */
        for (;;) {
            a = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_0);
            b = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_1);
            do {
                u8g2_font_decode_len(u8g2, a, 0);
                u8g2_font_decode_len(u8g2, b, 1);
            } while (u8g2_font_decode_get_unsigned_bits(decode, 1) != 0);

            if (decode->y >= h) break;
        }
    }
    return d;
}

/*
    Description:
        Find the starting point of the glyph data.
    描述：
        查找字形数据的起始位置
    Args:
        encoding: Encoding (ASCII or Unicode) of the glyph
                  字形的编码（ASCII或Unicode）
    Return:
        Address of the glyph data or NULL, if the encoding is not avialable in the font.
        字形数据的地址，如果字体中不存在该编码则返回NULL
*/
const uint8_t* u8g2_font_get_glyph_data(u8g2_font_t* u8g2, uint16_t encoding) {
    const uint8_t* font = u8g2->font;
    font += 23;

    if (encoding <= 255) {
        // ASCII 字符处理
        if (encoding >= 'a') {
            font += u8g2->font_info.start_pos_lower_a;
        } else if (encoding >= 'A') {
            font += u8g2->font_info.start_pos_upper_A;
        }

        for (;;) {
            if (u8x8_pgm_read(font + 1) == 0) break;
            if (u8x8_pgm_read(font) == encoding) {
                return font + 2; /* skip encoding and glyph size */
            }
            font += u8x8_pgm_read(font + 1);
        }
    } else {
        // Unicode 字符处理
        uint16_t e;
        const uint8_t* unicode_lookup_table;
        /* support for the new unicode lookup table */
        /* 支持新的 Unicode 查找表 */

        font += u8g2->font_info.start_pos_unicode;
        unicode_lookup_table = font;

        /* u8g2 issue 596: search for the glyph start in the unicode lookup table */
        /* u8g2 issue 596: 在 Unicode 查找表中搜索字形起始位置 */
        do {
            font += u8g2_font_get_word(unicode_lookup_table, 0);
            e = u8g2_font_get_word(unicode_lookup_table, 2);
            unicode_lookup_table += 4;
        } while (e < encoding);

        /* variable "font" is now updated according to the lookup table */
        /* 变量 "font" 现在已根据查找表更新 */

        for (;;) {
            e = u8x8_pgm_read(font);
            e <<= 8;
            e |= u8x8_pgm_read(font + 1);
            if (e == 0) break;
            if (e == encoding) {
                return font + 3; /* skip encoding and glyph size */
            }
            font += u8x8_pgm_read(font + 2);
        }
    }
    return NULL;
}

// 绘制单个字形的内部实现
static int16_t u8g2_font_draw_glyph(u8g2_font_t* u8g2, int16_t x, int16_t y, uint16_t encoding) {
    int16_t dx = 0;
    u8g2->font_decode.target_x = x;
    u8g2->font_decode.target_y = y;
    // u8g2->font_decode.is_transparent = is_transparent; this is already set
    // u8g2->font_decode.dir = dir;
    const uint8_t* glyph_data = u8g2_font_get_glyph_data(u8g2, encoding);
    if (glyph_data != NULL) {
        dx = u8g2_font_decode_glyph(u8g2, glyph_data);
    }
    return dx;
}

//========================================================

// 检查字形是否存在
uint8_t u8g2_IsGlyph(u8g2_font_t* u8g2, uint16_t requested_encoding) {
    /* updated to new code */
    if (u8g2_font_get_glyph_data(u8g2, requested_encoding) != NULL) return 1;
    return 0;
}

/* side effect: updates u8g2->font_decode and u8g2->glyph_x_offset */
/* actually u8g2_GetGlyphWidth returns the glyph delta x and glyph width itself is set as side effect */
/* 副作用：更新 u8g2->font_decode 和 u8g2->glyph_x_offset */
/* 实际上 u8g2_GetGlyphWidth 返回字形的X增量，字形宽度本身作为副作用被设置 */
// 获取字形宽度
int8_t u8g2_GetGlyphWidth(u8g2_font_t* u8g2, uint16_t requested_encoding) {
    const uint8_t* glyph_data = u8g2_font_get_glyph_data(u8g2, requested_encoding);
    if (glyph_data == NULL) return 0;

    u8g2_font_setup_decode(u8g2, glyph_data);
    u8g2->glyph_x_offset = u8g2_font_decode_get_signed_bits(&(u8g2->font_decode), u8g2->font_info.bits_per_char_x);
    u8g2_font_decode_get_signed_bits(&(u8g2->font_decode), u8g2->font_info.bits_per_char_y);

    /* glyph width is here: u8g2->font_decode.glyph_width */
    return u8g2_font_decode_get_signed_bits(&(u8g2->font_decode), u8g2->font_info.bits_per_delta_x);
}

// 设置字体模式（透明/不透明）
void u8g2_SetFontMode(u8g2_font_t* u8g2, uint8_t is_transparent) {
    u8g2->font_decode.is_transparent = is_transparent;  // new font procedures
}

// 设置字体方向（0/1/2/3 对应 0°/90°/180°/270°）
void u8g2_SetFontDirection(u8g2_font_t* u8g2, uint8_t dir) { u8g2->font_decode.dir = dir; }

// 绘制单个字形（公共接口）
int16_t u8g2_DrawGlyph(u8g2_font_t* u8g2, int16_t x, int16_t y, uint16_t encoding) {
    return u8g2_font_draw_glyph(u8g2, x, y, encoding);
}

// 绘制字符串
int16_t u8g2_DrawStr(u8g2_font_t* u8g2, int16_t x, int16_t y, const char* s) {
    int16_t sum, delta;
    sum = 0;

    while (*s != '\0') {
        delta = u8g2_DrawGlyph(u8g2, x, y, *s);
        switch (u8g2->font_decode.dir) {
            case 0:
                x += delta;
                break;
            case 1:
                y += delta;
                break;
            case 2:
                x -= delta;
                break;
            case 3:
                y -= delta;
                break;
        }
        sum += delta;
        s++;
    }
    return sum;
}

// 设置字体
void u8g2_SetFont(u8g2_font_t* u8g2, const uint8_t* font) {
    if (u8g2->font != font) {
        u8g2->font = font;
        u8g2->font_decode.is_transparent = 0;

        u8g2_read_font_info(&(u8g2->font_info), font);
    }
}

// 设置前景色
void u8g2_SetForegroundColor(u8g2_font_t* u8g2, uint16_t fg) { u8g2->font_decode.fg_color = fg; }

// 设置背景色
void u8g2_SetBackgroundColor(u8g2_font_t* u8g2, uint16_t bg) { u8g2->font_decode.bg_color = bg; }
