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
#include <stdint.h>
#include <string.h>

#define U8G2_FONT_INVALID_OFFSET UINT32_MAX
#define U8G2_FONT_READ_RETRIES 3U

typedef char font_info_wire_layout[(offsetof(u8g2_font_info_t, descent_para) == 16U) ? 1 : -1];

static void u8g2_font_cache_reset(u8g2_font_t* u8g2) {
    uint8_t i;

    u8g2->font_cache_valid = 0;
    u8g2->font_cache_base = 0;
    u8g2->font_cache_length = 0;
    u8g2->font_read_error = 0;
    u8g2->glyph_cache_next = 0U;
    u8g2->glyph_source = u8g2->glyph_source_next = 0U;
    memset(u8g2->glyph_sources, 0, sizeof(u8g2->glyph_sources));
    for (i = 0U; i < U8G2_GLYPH_CACHE_SIZE; i++) {
        u8g2->glyph_cache_valid[i] = 0U;
    }
}

/* Keep lookup results across the three common font switches in calendar cells.
 * Only offsets survive a switch; the byte cache always belongs to the active
 * source. Revision/length/reader changes evict the previous source's offsets. */
static void u8g2_select_glyph_source(u8g2_font_t *u8g2, const u8g2_font_source_t *source)
{
    for (uint8_t i = 0; i < U8G2_GLYPH_SOURCE_CACHE_SIZE; ++i) {
        const u8g2_font_source_t *cached = &u8g2->glyph_sources[i];
        if (cached->read == source->read && cached->context == source->context &&
            cached->data == source->data && cached->length == source->length &&
            cached->revision == source->revision) {
            u8g2->glyph_source = i;
            return;
        }
    }
    uint8_t slot = u8g2->glyph_source_next;
    u8g2->glyph_source_next = (slot + 1U) % U8G2_GLYPH_SOURCE_CACHE_SIZE;
    u8g2->glyph_source = slot;
    u8g2->glyph_sources[slot] = *source;
    for (uint8_t i = 0; i < U8G2_GLYPH_CACHE_SIZE; ++i)
        if (u8g2->glyph_cache_source[i] == slot) u8g2->glyph_cache_valid[i] = 0;
}

static void u8g2_apply_placeholder_metrics(u8g2_font_t* u8g2,
                                            const u8g2_font_placeholder_t* placeholder) {
    u8g2->font_info.max_char_width = (uint8_t)placeholder->max_char_width;
    u8g2->font_info.max_char_height = (uint8_t)placeholder->max_char_height;
    u8g2->font_info.x_offset = placeholder->x_offset;
    u8g2->font_info.y_offset = placeholder->y_offset;
    u8g2->font_info.ascent_A = placeholder->ascent;
    u8g2->font_info.descent_g = placeholder->descent;
    u8g2->font_info.ascent_para = placeholder->ascent_para;
    u8g2->font_info.descent_para = placeholder->descent_para;
}

static void u8g2_draw_placeholder(u8g2_font_t* u8g2, int16_t x, int16_t y) {
    int16_t left = x + u8g2->placeholder.x_offset;
    int16_t top = y - u8g2->placeholder.ascent;
    int16_t width = u8g2->placeholder.max_char_width;
    int16_t height = u8g2->placeholder.max_char_height;

    if (width <= 0 || height <= 0 || u8g2->draw_hv_line == NULL) {
        return;
    }
    u8g2->draw_hv_line(u8g2, left, top, width, 0U, u8g2->font_decode.fg_color);
    if (height > 1) {
        u8g2->draw_hv_line(u8g2, left, top + height - 1, width, 0U, u8g2->font_decode.fg_color);
        u8g2->draw_hv_line(u8g2, left, top, height, 1U, u8g2->font_decode.fg_color);
        u8g2->draw_hv_line(u8g2, left + width - 1, top, height, 1U, u8g2->font_decode.fg_color);
        /* A diagonal cross makes the missing-glyph state visible without
         * introducing a second font resource. */
        {
            int16_t i;
            int16_t diagonal = (width < height) ? width : height;
            for (i = 1; i < diagonal - 1; i++) {
                u8g2->draw_hv_line(u8g2, left + i, top + i, 1, 0U, u8g2->font_decode.fg_color);
                u8g2->draw_hv_line(u8g2, left + width - 1 - i, top + i, 1, 0U,
                                   u8g2->font_decode.fg_color);
            }
        }
    }
}

static uint8_t u8g2_font_read_byte(u8g2_font_t* u8g2, uint32_t offset) {
    u8g2_font_source_t* source = &u8g2->font_source;

    if (source->read == NULL) {
        if (source->data == NULL) {
            u8g2->font_read_error = 1;
            return 0;
        }
        return u8x8_pgm_read(source->data + offset);
    }

    if (offset >= source->length) {
        u8g2->font_read_error = 1;
        return 0;
    }

    if (!u8g2->font_cache_valid ||
        offset < u8g2->font_cache_base ||
        offset >= u8g2->font_cache_base + u8g2->font_cache_length) {
        /* The cache size follows the SPI payload limit and is not required
         * to be a power of two.  Use arithmetic block alignment instead of
         * a bit mask so a cache miss maps to one FM25Q04B read. */
        uint32_t base = (offset / U8G2_FONT_CACHE_SIZE) * U8G2_FONT_CACHE_SIZE;
        size_t length = U8G2_FONT_CACHE_SIZE;
        int ret;

        if (length > (size_t)(source->length - base)) {
            length = (size_t)(source->length - base);
        }

        ret = source->read(source->context, base, u8g2->font_cache, length);
        if (ret != 0) {
            u8g2_font_cache_reset(u8g2);
            u8g2->font_read_error = 1;
            return 0;
        }

        u8g2->font_cache_base = base;
        u8g2->font_cache_length = (uint16_t)length;
        u8g2->font_cache_valid = 1;
    }

    return u8g2->font_cache[offset - u8g2->font_cache_base];
}

static uint8_t u8g2_font_read_header_byte(u8g2_font_t* u8g2,
                                          uint32_t offset,
                                          uint8_t* read_error) {
    uint8_t value = u8g2_font_read_byte(u8g2, offset);

    if (u8g2->font_read_error) {
        *read_error = 1U;
    }
    return value;
}

static uint16_t u8g2_font_read_header_word(u8g2_font_t* u8g2,
                                           uint32_t offset,
                                           uint8_t* read_error) {
    uint16_t value = (uint16_t)u8g2_font_read_header_byte(u8g2, offset, read_error) << 8;
    value |= u8g2_font_read_header_byte(u8g2, offset + 1U, read_error);
    return value;
}

// 从字体数据中读取双字节（大端序）
static uint16_t u8g2_font_get_word(u8g2_font_t* u8g2, uint32_t offset) U8X8_NOINLINE;
static uint16_t u8g2_font_get_word(u8g2_font_t* u8g2, uint32_t offset) {
    uint16_t pos = u8g2_font_read_byte(u8g2, offset);
    pos <<= 8;
    pos += u8g2_font_read_byte(u8g2, offset + 1U);
    return pos;
}

/*========================================================================*/
/* new font format */
/* 新字体格式 */
// 读取字体信息结构（从字体数据的头部读取元数据）
static void u8g2_read_font_info(u8g2_font_t* u8g2) {
    u8g2_font_info_t* font_info = &u8g2->font_info;
    uint8_t read_error = 0U;

    u8g2->font_info_valid = 0U;
    u8g2->font_read_error = 0U;

    /* The initial 17 fields are all bytes in wire order; the aligned
     * uint16_t offsets below must still be decoded explicitly as big endian. */
    uint8_t* fields = (uint8_t*)font_info;
    for (uint8_t i = 0U; i < 17U; i++) {
        fields[i] = u8g2_font_read_header_byte(u8g2, i, &read_error);
    }

    /* offset 17 */
    font_info->start_pos_upper_A = u8g2_font_read_header_word(u8g2, 17, &read_error);
    font_info->start_pos_lower_a = u8g2_font_read_header_word(u8g2, 19, &read_error);

    /* offset 21 */
    font_info->start_pos_unicode = u8g2_font_read_header_word(u8g2, 21, &read_error);

    u8g2->font_read_error = read_error;
    u8g2->font_info_valid = (read_error == 0U) ? 1U : 0U;
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
static uint8_t u8g2_font_decode_get_unsigned_bits(u8g2_font_t* u8g2, uint8_t cnt) U8X8_NOINLINE;
static uint8_t u8g2_font_decode_get_unsigned_bits(u8g2_font_t* u8g2, uint8_t cnt) {
    u8g2_font_decode_t* f = &u8g2->font_decode;
    uint8_t val;
    uint8_t bit_pos = f->decode_bit_pos;
    uint8_t bit_pos_plus_cnt;

    if (cnt == 0U || cnt > 8U) {
        u8g2->font_read_error = 1U;
        return 0U;
    }
    if (u8g2->glyph_end_offset != 0U &&
        f->decode_offset >= u8g2->glyph_end_offset) {
        u8g2->font_read_error = 1U;
        return 0U;
    }
    val = u8g2_font_read_byte(u8g2, f->decode_offset);

    val >>= bit_pos;
    bit_pos_plus_cnt = bit_pos;
    bit_pos_plus_cnt += cnt;
    if (bit_pos_plus_cnt >= 8) {
        uint8_t s = 8;
        s -= bit_pos;
        f->decode_offset++;
        /* At exactly eight bits only advance the cursor. The current byte
         * already satisfies the request, including at the end of a glyph. */
        if (bit_pos_plus_cnt > 8U) {
            if (u8g2->glyph_end_offset != 0U &&
                f->decode_offset >= u8g2->glyph_end_offset) {
                u8g2->font_read_error = 1U;
                return 0U;
            }
            val |= u8g2_font_read_byte(u8g2, f->decode_offset) << s;
        }
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
static int8_t u8g2_font_decode_get_signed_bits(u8g2_font_t* u8g2, uint8_t cnt) U8X8_NOINLINE;
static int8_t u8g2_font_decode_get_signed_bits(u8g2_font_t* u8g2, uint8_t cnt) {
    int8_t v, d;
    if (cnt == 0U || cnt > 8U) {
        u8g2->font_read_error = 1U;
        return 0;
    }
    v = (int8_t)u8g2_font_decode_get_unsigned_bits(u8g2, cnt);
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
#if GFX_FIXED_FONT_DIRECTION_0
    (void)x;
    (void)dir;
    return dy + y;
#else
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
#endif
}

// 根据方向添加X向量
static int16_t u8g2_add_vector_x(int16_t dx, int8_t x, int8_t y, uint8_t dir) U8X8_NOINLINE;
static int16_t u8g2_add_vector_x(int16_t dx, int8_t x, int8_t y, uint8_t dir) {
#if GFX_FIXED_FONT_DIRECTION_0
    (void)y;
    (void)dir;
    return dx + x;
#else
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
#endif
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

    /* local coordinates of the glyph */
    uint8_t lx, ly;

    u8g2_font_decode_t* decode = &(u8g2->font_decode);

    cnt = len;

    /* get the local position */
    lx = decode->x;
    ly = decode->y;

    for (;;) {
        uint8_t rem;     /* remaining pixel to the right edge of the glyph */
        uint8_t current; /* number of pixels, which need to be drawn for the draw procedure */
        int16_t x, y;

        /* current is either equal to cnt or equal to rem */
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
        x = u8g2_add_vector_x(x, lx, ly,
                              GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir);
        y = u8g2_add_vector_y(y, lx, ly,
                              GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir);

        /* draw foreground and background (if required) */
        if (current > 0) /* avoid drawing zero length lines, issue #4 */
        {
            if (is_foreground) {
                u8g2->draw_hv_line(u8g2, x, y, current,
                                   GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir,
                                   decode->fg_color);
            } else if (decode->is_transparent == 0) {
                u8g2->draw_hv_line(u8g2, x, y, current,
                                   GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir,
                                   decode->bg_color);
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
static void u8g2_font_setup_decode(u8g2_font_t* u8g2, uint32_t glyph_offset) {
    u8g2_font_decode_t* decode = &(u8g2->font_decode);
    decode->decode_offset = glyph_offset;
    decode->decode_bit_pos = 0;

    /* 8 Nov 2015, this is already done in the glyph data search procedure */
    /*
    decode->decode_ptr += 1;
    decode->decode_ptr += 1;
    */

    decode->glyph_width = u8g2_font_decode_get_unsigned_bits(u8g2, u8g2->font_info.bits_per_char_width);
    decode->glyph_height = u8g2_font_decode_get_unsigned_bits(u8g2, u8g2->font_info.bits_per_char_height);
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
static int8_t u8g2_font_decode_glyph(u8g2_font_t* u8g2, uint32_t glyph_offset) {
    int8_t x, y;
    int8_t d;
    int8_t h;
    uint32_t decode_guard;
    u8g2_font_decode_t* decode = &(u8g2->font_decode);

    u8g2_font_setup_decode(u8g2, glyph_offset);
    if (u8g2->font_read_error || u8g2->glyph_end_offset <= glyph_offset) {
        return 0;
    }
    decode_guard = (u8g2->glyph_end_offset - glyph_offset) * 8U + 1U;
    h = u8g2->font_decode.glyph_height;

    x = u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_char_x);
    y = u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_char_y);
    d = u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_delta_x);

    if (decode->glyph_width > 0) {
        decode->target_x = u8g2_add_vector_x(
            decode->target_x, x, -(h + y), GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir);
        decode->target_y = u8g2_add_vector_y(
            decode->target_y, x, -(h + y), GFX_FIXED_FONT_DIRECTION_0 ? 0U : decode->dir);
        // u8g2_add_vector(&(decode->target_x), &(decode->target_y), x, -(h+y), decode->dir);

        /* reset local x/y position */
        decode->x = 0;
        decode->y = 0;

        /* decode glyph */
        for (;;) {
            if (decode_guard == 0U) {
                u8g2->font_read_error = 1U;
                break;
            }
            uint8_t a = u8g2_font_decode_get_unsigned_bits(u8g2, u8g2->font_info.bits_per_0);
            uint8_t b = u8g2_font_decode_get_unsigned_bits(u8g2, u8g2->font_info.bits_per_1);
            do {
                if (decode_guard == 0U) {
                    u8g2->font_read_error = 1U;
                    break;
                }
                u8g2_font_decode_len(u8g2, a, 0);
                u8g2_font_decode_len(u8g2, b, 1);
                decode_guard--;
            } while (u8g2_font_decode_get_unsigned_bits(u8g2, 1) != 0);

            if (u8g2->font_read_error) {
                break;
            }

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
static uint32_t u8g2_font_get_glyph_data(u8g2_font_t* u8g2, uint16_t encoding) {
    uint32_t font = 23U;
    uint32_t limit = u8g2->font_source.read != NULL ?
                     u8g2->font_source.length : UINT32_MAX;

    u8g2->font_read_error = 0;
    u8g2->glyph_end_offset = 0U;

    if (encoding <= 255) {
        // ASCII 字符处理
        if (encoding >= 'a') {
            font += u8g2->font_info.start_pos_lower_a;
        } else if (encoding >= 'A') {
            font += u8g2->font_info.start_pos_upper_A;
        }

        for (;;) {
            uint8_t glyph_size = u8g2_font_read_byte(u8g2, font + 1U);
            if (u8g2->font_read_error || glyph_size == 0) break;
            if (glyph_size < 2U || font > limit || glyph_size > limit - font) {
                u8g2->font_read_error = 1U;
                break;
            }
            if (u8g2_font_read_byte(u8g2, font) == encoding) {
                u8g2->glyph_end_offset = font + glyph_size;
                return font + 2U; /* skip encoding and glyph size */
            }
            font += glyph_size;
        }
    } else {
        // Unicode 字符处理
        uint16_t e;
        uint32_t unicode_lookup_table;
        uint32_t table_end = 0U;
        uint16_t previous = 0U;
        /* support for the new unicode lookup table */
        /* 支持新的 Unicode 查找表 */

        font += u8g2->font_info.start_pos_unicode;
        unicode_lookup_table = font;

        /* u8g2 issue 596: search for the glyph start in the unicode lookup table */
        /* u8g2 issue 596: 在 Unicode 查找表中搜索字形起始位置 */
        do {
            uint16_t delta = u8g2_font_get_word(u8g2, unicode_lookup_table);
            e = u8g2_font_get_word(u8g2, unicode_lookup_table + 2U);
            if (u8g2->font_read_error) return U8G2_FONT_INVALID_OFFSET;
            if (e == 0U && previous == 0U) return U8G2_FONT_INVALID_OFFSET;
            if (table_end == 0U) {
                table_end = unicode_lookup_table + delta;
            }
            if (e <= previous || delta == 0U || font >= limit ||
                delta >= limit - font || unicode_lookup_table > table_end ||
                table_end - unicode_lookup_table < 4U) {
                u8g2->font_read_error = 1U;
                return U8G2_FONT_INVALID_OFFSET;
            }
            font += delta;
            previous = e;
            unicode_lookup_table += 4U;
            if (u8g2->font_read_error) {
                return U8G2_FONT_INVALID_OFFSET;
            }
        } while (e < encoding);

        /* variable "font" is now updated according to the lookup table */
        /* 变量 "font" 现在已根据查找表更新 */

        for (;;) {
            uint8_t glyph_size;
            e = u8g2_font_read_byte(u8g2, font);
            e <<= 8;
            e |= u8g2_font_read_byte(u8g2, font + 1U);
            if (u8g2->font_read_error) {
                return U8G2_FONT_INVALID_OFFSET;
            }
            if (e == 0) break;
            glyph_size = u8g2_font_read_byte(u8g2, font + 2U);
            if (u8g2->font_read_error || glyph_size < 3U ||
                font > limit || glyph_size > limit - font) {
                u8g2->font_read_error = 1U;
                return U8G2_FONT_INVALID_OFFSET;
            }
            if (e == encoding) {
                u8g2->glyph_end_offset = font + glyph_size;
                return font + 3U; /* skip encoding and glyph size */
            }
            font += glyph_size;
        }
    }
    return U8G2_FONT_INVALID_OFFSET;
}

/* An external-font read can fail transiently while another SPI client is
 * finishing a transaction.  A genuine missing glyph leaves font_read_error
 * clear; only retry I/O failures so the fixed-width placeholder semantics are
 * unchanged for valid-but-incomplete font files. */
static uint32_t u8g2_font_find_glyph(u8g2_font_t* u8g2, uint16_t encoding) {
    uint32_t glyph_offset = U8G2_FONT_INVALID_OFFSET;
    uint8_t attempt;
    uint8_t i;

    u8g2->font_read_error = 0U;

    for (attempt = 0U; attempt < U8G2_FONT_READ_RETRIES &&
                       !u8g2->font_info_valid; attempt++) {
        if (u8g2->font_source.data == NULL && u8g2->font_source.read == NULL) {
            return U8G2_FONT_INVALID_OFFSET;
        }
        u8g2_font_cache_reset(u8g2);
        u8g2_read_font_info(u8g2);
    }
    if (!u8g2->font_info_valid) {
        return U8G2_FONT_INVALID_OFFSET;
    }

    for (i = 0U; i < U8G2_GLYPH_CACHE_SIZE; i++) {
        if (u8g2->glyph_cache_valid[i] &&
            u8g2->glyph_cache_source[i] == u8g2->glyph_source &&
            u8g2->glyph_cache_encoding[i] == encoding) {
            u8g2->glyph_end_offset = u8g2->glyph_cache_end[i];
            return u8g2->glyph_cache_offset[i];
        }
    }

    for (attempt = 0U; attempt < U8G2_FONT_READ_RETRIES; attempt++) {
        glyph_offset = u8g2_font_get_glyph_data(u8g2, encoding);
        if (glyph_offset != U8G2_FONT_INVALID_OFFSET ||
            !u8g2->font_read_error || u8g2->font_source.read == NULL) {
            if (!u8g2->font_read_error) {
                uint8_t cache_slot = u8g2->glyph_cache_next;
                u8g2->glyph_cache_encoding[cache_slot] = encoding;
                u8g2->glyph_cache_offset[cache_slot] = glyph_offset;
                u8g2->glyph_cache_end[cache_slot] = u8g2->glyph_end_offset;
                u8g2->glyph_cache_valid[cache_slot] = 1U;
                u8g2->glyph_cache_source[cache_slot] = u8g2->glyph_source;
                u8g2->glyph_cache_next =
                    (uint8_t)((cache_slot + 1U) % U8G2_GLYPH_CACHE_SIZE);
            }
            break;
        }
        /* FM25Q04B may briefly reject a read while another SPI transaction
         * is releasing the bus.  Retry only I/O failures; a valid font that
         * does not contain the code point must still use the placeholder. */
        u8g2_font_cache_reset(u8g2);
    }
    return glyph_offset;
}

// 绘制单个字形的内部实现。调用者已经完成字形查找，避免外置 Flash
// 字库路径再次扫描 Unicode 表。
static int16_t u8g2_font_draw_glyph_at(u8g2_font_t* u8g2,
                                       int16_t x,
                                       int16_t y,
                                       uint32_t glyph_offset) {
    u8g2->font_decode.target_x = x;
    u8g2->font_decode.target_y = y;
    // u8g2->font_decode.is_transparent = is_transparent; this is already set
    // u8g2->font_decode.dir = dir;
    return u8g2_font_decode_glyph(u8g2, glyph_offset);
}

//========================================================

// 检查字形是否存在
uint8_t u8g2_IsGlyph(u8g2_font_t* u8g2, uint16_t requested_encoding) {
    /* updated to new code */
    if (u8g2_font_find_glyph(u8g2, requested_encoding) != U8G2_FONT_INVALID_OFFSET) return 1;
    return 0;
}

/* side effect: updates u8g2->font_decode and u8g2->glyph_x_offset */
/* actually u8g2_GetGlyphWidth returns the glyph delta x and glyph width itself is set as side effect */
/* 副作用：更新 u8g2->font_decode 和 u8g2->glyph_x_offset */
/* 实际上 u8g2_GetGlyphWidth 返回字形的X增量，字形宽度本身作为副作用被设置 */
uint8_t u8g2_GetGlyphWidthIfPresent(u8g2_font_t* u8g2,
                                    uint16_t requested_encoding,
                                    int8_t* advance) {
    uint32_t glyph_offset = u8g2_font_find_glyph(u8g2, requested_encoding);
    if (glyph_offset == U8G2_FONT_INVALID_OFFSET || advance == NULL) {
        return 0U;
    }

    u8g2_font_setup_decode(u8g2, glyph_offset);
    u8g2->glyph_x_offset = u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_char_x);
    u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_char_y);

    /* glyph width is here: u8g2->font_decode.glyph_width */
    int8_t decoded = u8g2_font_decode_get_signed_bits(u8g2, u8g2->font_info.bits_per_delta_x);
    if (u8g2->font_read_error) return 0U;
    *advance = decoded;
    return 1U;
}

// 获取字形宽度
int8_t u8g2_GetGlyphWidth(u8g2_font_t* u8g2, uint16_t requested_encoding) {
    int8_t advance = 0;

    (void)u8g2_GetGlyphWidthIfPresent(u8g2, requested_encoding, &advance);
    return advance;
}

// 设置字体模式（透明/不透明）
void u8g2_SetFontMode(u8g2_font_t* u8g2, uint8_t is_transparent) {
    u8g2->font_decode.is_transparent = is_transparent;  // new font procedures
}

// 设置字体方向（0/1/2/3 对应 0°/90°/180°/270°）
void u8g2_SetFontDirection(u8g2_font_t* u8g2, uint8_t dir) {
#if GFX_FIXED_FONT_DIRECTION_0
    (void)u8g2;
    (void)dir;
#else
    u8g2->font_decode.dir = dir;
#endif
}

// 绘制单个字形（公共接口）
int16_t u8g2_DrawGlyph(u8g2_font_t* u8g2, int16_t x, int16_t y, uint16_t encoding) {
    uint32_t glyph_offset = u8g2_font_find_glyph(u8g2, encoding);
    uint8_t has_glyph = (glyph_offset != U8G2_FONT_INVALID_OFFSET) ? 1U : 0U;

    /* A font may intentionally omit ASCII whitespace (the external WQY12
     * resource stores digits and CJK glyphs only).  In the current UI these
     * spaces are formatting remnants, not a column that should move the next
     * glyph.  Skip them completely instead of drawing a placeholder or
     * retaining the placeholder advance. */
    if (!has_glyph && encoding == (uint16_t)' ' && u8g2->placeholder_enabled) {
        return 0;
    }

    int16_t delta = has_glyph ?
        u8g2_font_draw_glyph_at(u8g2, x, y, glyph_offset) : 0;
    if (u8g2->font_read_error) has_glyph = 0U;
    if (!has_glyph && u8g2->placeholder_enabled) {
        u8g2_draw_placeholder(u8g2, x, y);
        return u8g2->placeholder.advance;
    }
    return delta;
}

// 绘制字符串
int16_t u8g2_DrawStr(u8g2_font_t* u8g2, int16_t x, int16_t y, const char* s) {
    int16_t sum = 0;

    while (*s != '\0') {
        int16_t delta = u8g2_DrawGlyph(u8g2, x, y, *s);
#if GFX_FIXED_FONT_DIRECTION_0
        x += delta;
#else
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
#endif
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
        u8g2->font_source.data = font;
        u8g2->font_source.length = 0;
        u8g2->font_source.read = NULL;
        u8g2->font_source.context = NULL;
        u8g2->font_source.revision = 0;
        u8g2->placeholder_enabled = 0;
        u8g2->font_info_valid = 0U;
        u8g2_font_cache_reset(u8g2);
        u8g2_read_font_info(u8g2);
    }
}

void u8g2_SetExternalFont(u8g2_font_t* u8g2, const u8g2_font_source_t* source) U8X8_NOINLINE;
void u8g2_SetExternalFont(u8g2_font_t* u8g2, const u8g2_font_source_t* source) {
    if (u8g2 == NULL || source == NULL || source->read == NULL || source->length < 23U) {
        return;
    }

    /* GUI code may select the same font before each text run.  Preserve the
     * block cache in that case; callers that changed the resource in-place
     * can explicitly use u8g2_InvalidateFontCache(). */
    if (u8g2->font == NULL &&
        u8g2->font_source.data == source->data &&
        u8g2->font_source.length == source->length &&
        u8g2->font_source.read == source->read &&
        u8g2->font_source.context == source->context &&
        u8g2->font_source.revision == source->revision &&
        u8g2->font_info_valid) {
        return;
    }

    u8g2->font = NULL;
    u8g2->font_source = *source;
    u8g2->font_decode.is_transparent = 0;
    u8g2->placeholder_enabled = 0;
    u8g2->font_info_valid = 0U;
    u8g2->font_info = (u8g2_font_info_t){0};
    u8g2->font_cache_valid = 0;
    u8g2->font_read_error = 0;
    u8g2_select_glyph_source(u8g2, source);
    u8g2_read_font_info(u8g2);
}

void u8g2_SetPlaceholder(u8g2_font_t* u8g2, const u8g2_font_placeholder_t* placeholder) {
    if (u8g2 == NULL || placeholder == NULL || placeholder->advance <= 0) {
        return;
    }
    u8g2->placeholder = *placeholder;
    u8g2->placeholder_enabled = 1U;
    if (u8g2->font_source.read == NULL && u8g2->font_source.data == NULL) {
        u8g2_apply_placeholder_metrics(u8g2, placeholder);
    }
}

void u8g2_SetMissingFont(u8g2_font_t* u8g2, const u8g2_font_placeholder_t* placeholder) U8X8_NOINLINE;
void u8g2_SetMissingFont(u8g2_font_t* u8g2, const u8g2_font_placeholder_t* placeholder) {
    if (u8g2 == NULL || placeholder == NULL || placeholder->advance <= 0) {
        return;
    }
    u8g2->font = NULL;
    memset(&u8g2->font_source, 0, sizeof(u8g2->font_source));
    u8g2->font_info = (u8g2_font_info_t){0};
    u8g2->font_info_valid = 0U;
    u8g2->font_decode.is_transparent = 0;
    u8g2->placeholder = *placeholder;
    u8g2->placeholder_enabled = 1U;
    u8g2_apply_placeholder_metrics(u8g2, placeholder);
    u8g2_font_cache_reset(u8g2);
}

void u8g2_InvalidateFontCache(u8g2_font_t* u8g2) {
    if (u8g2 == NULL) {
        return;
    }
    u8g2->font_info_valid = 0U;
    u8g2_font_cache_reset(u8g2);
}

// 设置前景色
void u8g2_SetForegroundColor(u8g2_font_t* u8g2, uint16_t fg) { u8g2->font_decode.fg_color = fg; }

// 设置背景色
void u8g2_SetBackgroundColor(u8g2_font_t* u8g2, uint16_t bg) { u8g2->font_decode.bg_color = bg; }
