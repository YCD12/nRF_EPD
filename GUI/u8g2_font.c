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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gui_porting_hal.h"

#define U8G2_EXT_FONT_TOKEN_BASE 0xF0000000UL
#define U8G2_EXT_FONT_TOKEN_MASK 0xFF000000UL
/* u8g2 binary-searches inside external font payloads. Keep a modest block
 * cache so CJK glyph rendering avoids byte-at-a-time SPI reads while leaving
 * enough RAM margin on nRF52811.
 * We optimize this to a 3-way multi-channel cache with 128-byte block size
 * to prevent SPI cache thrashing during concurrent font renders. */
#define U8G2_EXT_FONT_CACHE_SIZE 128U
#define U8G2_EXT_FONT_CACHE_WAYS 3

typedef struct {
    bool valid;
    uint16_t font_id;
    uint32_t base_offset;
    size_t length;
    uint32_t last_used;
    uint8_t data[U8G2_EXT_FONT_CACHE_SIZE];
} u8g2_ext_font_cache_t;

static u8g2_ext_font_cache_t m_u8g2_ext_font_caches[U8G2_EXT_FONT_CACHE_WAYS];
static uint32_t m_u8g2_cache_access_count = 0;

static bool u8g2_font_external_ref(const uint8_t* font, uint16_t* font_id, uint32_t* offset) {
    uintptr_t value = (uintptr_t)font;

    if ((value & U8G2_EXT_FONT_TOKEN_MASK) != U8G2_EXT_FONT_TOKEN_BASE) {
        return false;
    }

    if (font_id != NULL) {
        *font_id = (uint16_t)((value >> 16) & 0xFFU);
    }
    if (offset != NULL) {
        *offset = (uint32_t)(value & 0xFFFFU);
    }
    return true;
}

static uint8_t u8g2_font_read_byte(const uint8_t* font) {
    uint16_t font_id = 0;
    uint32_t offset = 0;
    epd_ext_resource_entry_t entry;
    ret_code_t ret;

    if (!u8g2_font_external_ref(font, &font_id, &offset)) {
        return u8x8_pgm_read(font);
    }

    int cache_idx = -1;
    for (int i = 0; i < U8G2_EXT_FONT_CACHE_WAYS; i++) {
        if (m_u8g2_ext_font_caches[i].valid &&
            m_u8g2_ext_font_caches[i].font_id == font_id &&
            offset >= m_u8g2_ext_font_caches[i].base_offset &&
            offset < m_u8g2_ext_font_caches[i].base_offset + m_u8g2_ext_font_caches[i].length) {
            cache_idx = i;
            break;
        }
    }

    if (cache_idx != -1) {
        m_u8g2_cache_access_count++;
        m_u8g2_ext_font_caches[cache_idx].last_used = m_u8g2_cache_access_count;
        return m_u8g2_ext_font_caches[cache_idx].data[offset - m_u8g2_ext_font_caches[cache_idx].base_offset];
    }

    // Cache miss, select victim using LRU
    int victim_idx = 0;
    uint32_t min_used = 0xFFFFFFFFUL;
    for (int i = 0; i < U8G2_EXT_FONT_CACHE_WAYS; i++) {
        if (!m_u8g2_ext_font_caches[i].valid) {
            victim_idx = i;
            break;
        }
        if (m_u8g2_ext_font_caches[i].last_used < min_used) {
            min_used = m_u8g2_ext_font_caches[i].last_used;
            victim_idx = i;
        }
    }

    uint32_t base_offset = offset & ~(uint32_t)(U8G2_EXT_FONT_CACHE_SIZE - 1U);
    size_t chunk_len = U8G2_EXT_FONT_CACHE_SIZE;

    ret = res_hal_get_info(EPD_EXT_RES_TYPE_FONT, font_id, &entry);
    if (ret != NRF_SUCCESS || base_offset >= entry.length) {
        return 0;
    }
    if (chunk_len > (size_t)(entry.length - base_offset)) {
        chunk_len = (size_t)(entry.length - base_offset);
    }

    ret = res_hal_read(EPD_EXT_RES_TYPE_FONT, font_id, base_offset,
                       m_u8g2_ext_font_caches[victim_idx].data, chunk_len);
    if (ret != NRF_SUCCESS) {
        m_u8g2_ext_font_caches[victim_idx].valid = false;
        return 0;
    }

    m_u8g2_ext_font_caches[victim_idx].valid = true;
    m_u8g2_ext_font_caches[victim_idx].font_id = font_id;
    m_u8g2_ext_font_caches[victim_idx].base_offset = base_offset;
    m_u8g2_ext_font_caches[victim_idx].length = chunk_len;
    m_u8g2_cache_access_count++;
    m_u8g2_ext_font_caches[victim_idx].last_used = m_u8g2_cache_access_count;

    /* Cache miss requires an SPI read to external Flash. Drawing a CJK
     * page can incur hundreds of misses; feed the watchdog so a slow
     * paint pass cannot trigger a reset. */
    gui_porting_feed_wdt();

    return m_u8g2_ext_font_caches[victim_idx].data[offset - base_offset];
}

// 从字体数据中读取单个字节
static uint8_t u8g2_font_get_byte(const uint8_t* font, uint8_t offset) {
    font += offset;
    return u8g2_font_read_byte(font);
}

// 从字体数据中读取双字节（大端序）
static uint16_t u8g2_font_get_word(const uint8_t* font, uint8_t offset) U8X8_NOINLINE;
static uint16_t u8g2_font_get_word(const uint8_t* font, uint8_t offset) {
    uint16_t pos;
    font += offset;
    pos = u8g2_font_read_byte(font);
    font++;
    pos <<= 8;
    pos += u8g2_font_read_byte(font);
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
    val = u8g2_font_read_byte(f->decode_ptr);

    val >>= bit_pos;
    bit_pos_plus_cnt = bit_pos;
    bit_pos_plus_cnt += cnt;
    if (bit_pos_plus_cnt >= 8) {
        uint8_t s = 8;
        s -= bit_pos;
        f->decode_ptr++;
        // val |= *(f->decode_ptr) << (8-bit_pos);
        val |= u8g2_font_read_byte(f->decode_ptr) << (s);
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
        uint16_t outer_guard = 0;
        for (;;) {
            if (++outer_guard > 1000) {
                break;
            }
            a = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_0);
            b = u8g2_font_decode_get_unsigned_bits(decode, u8g2->font_info.bits_per_1);
            uint16_t inner_guard = 0;
            do {
                if (++inner_guard > 255) {
                    break;
                }
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
    uint16_t font_id = 0;
    if (u8g2_font_external_ref(u8g2->font, &font_id, NULL)) {
        epd_ext_resource_entry_t entry;
        if (res_hal_get_info(EPD_EXT_RES_TYPE_FONT, font_id, &entry) != NRF_SUCCESS) {
            return NULL;
        }
    }
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
            if (u8g2_font_read_byte(font + 1) == 0) break;
            if (u8g2_font_read_byte(font) == encoding) {
                return font + 2; /* skip encoding and glyph size */
            }
            font += u8g2_font_read_byte(font + 1);
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
        uint16_t lookup_guard = 0;
        do {
            if (++lookup_guard > 2000) {
                return NULL;
            }
            font += u8g2_font_get_word(unicode_lookup_table, 0);
            e = u8g2_font_get_word(unicode_lookup_table, 2);
            unicode_lookup_table += 4;
        } while (e < encoding);

        /* variable "font" is now updated according to the lookup table */
        /* 变量 "font" 现在已根据查找表更新 */

        uint16_t search_guard = 0;
        for (;;) {
            if (++search_guard > 1000) {
                break;
            }
            e = u8g2_font_read_byte(font);
            e <<= 8;
            e |= u8g2_font_read_byte(font + 1);
            if (e == 0) break;
            if (e == encoding) {
                return font + 3; /* skip encoding and glyph size */
            }
            uint8_t step = u8g2_font_read_byte(font + 2);
            if (step == 0) break;
            font += step;
        }
    }
    return NULL;
}

/*
 * Glyph Fallback（缺字优雅回退，见设计文档优化 10）
 * ------------------------------------------------------------------
 * 字库子集化后，若渲染到不在子集内的汉字（如未来新增节日名、生僻字），
 * u8g2_font_get_glyph_data 返回 NULL。原行为是"跳过且步进为 0"——文字既空白
 * 又塌缩，用户无法察觉缺字。此处改为绘制一个字体无关的空心占位框（□），
 * 并给出正常步进，保证：
 *   1. 缺字位置有可见占位（诚实降级，而非静默空白）；
 *   2. 后续字符排版不塌缩错位；
 *   3. 绘制管线不中断（不依赖任何具体字体是否含 '?'）。
 * 仅对可见字符(encoding >= 0x21)回退；空格/控制字符不画框。
 */
/* 缺字占位框的字高（基线向上的高度）：优先用大写字高 ascent_A。 */
static int16_t u8g2_missing_glyph_cap(u8g2_font_t* u8g2) {
    int16_t cap = (int16_t)u8g2->font_info.ascent_A;
    if (cap <= 0) {
        cap = (int16_t)u8g2->font_info.max_char_height;
    }
    if (cap <= 2) {
        cap = 6; /* 极端兜底，避免退化成 0/负 */
    }
    return cap;
}

/* 缺字占位框宽度（约为字高的 3/5，下限 3px）。 */
static int16_t u8g2_missing_glyph_width(u8g2_font_t* u8g2) {
    int16_t box_w = (u8g2_missing_glyph_cap(u8g2) * 3) / 5;
    if (box_w < 3) {
        box_w = 3;
    }
    return box_w;
}

/* 缺字步进 = 占位框宽 + 1px 间隙，测宽与绘制共用以保证居中排版一致。 */
static int16_t u8g2_missing_glyph_advance(u8g2_font_t* u8g2) {
    return u8g2_missing_glyph_width(u8g2) + 1;
}

static int16_t u8g2_font_draw_missing_glyph(u8g2_font_t* u8g2, int16_t x, int16_t y) {
    int16_t box_w = u8g2_missing_glyph_width(u8g2);
    int16_t box_h = u8g2_missing_glyph_cap(u8g2);
    int16_t y_top;
    uint16_t fg = u8g2->font_decode.fg_color;

    y_top = y - box_h + 1; /* 基线在 y，占位框自基线向上 */

    if (u8g2->draw_hv_line != NULL) {
        u8g2->draw_hv_line(u8g2, x, y_top, box_w, 0, fg);                    /* 上边 */
        u8g2->draw_hv_line(u8g2, x, y, box_w, 0, fg);                        /* 下边(基线) */
        u8g2->draw_hv_line(u8g2, x, y_top, box_h, 1, fg);                    /* 左边 */
        u8g2->draw_hv_line(u8g2, x + box_w - 1, y_top, box_h, 1, fg);        /* 右边 */
    }

    return (int16_t)(box_w + 1); /* 步进：占位框宽 + 1px 间隙 */
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
    } else if (encoding >= 0x21) {
        /* 缺字回退：仅对可见字符绘制占位框，空格/控制字符仍按 0 步进跳过。 */
        dx = u8g2_font_draw_missing_glyph(u8g2, x, y);
    }
    return dx;
}

/* 步进为 int8_t 返回；占位框步进在常见字号(9~18px)下不超过 12，安全落入范围。 */
static int8_t u8g2_missing_glyph_delta(u8g2_font_t* u8g2) {
    int16_t adv = u8g2_missing_glyph_advance(u8g2);
    if (adv > 127) {
        adv = 127;
    }
    return (int8_t)adv;
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
    if (glyph_data == NULL) {
        /* 缺字：可见字符返回占位框步进，与实际绘制一致，避免居中排版塌缩错位。 */
        return (requested_encoding >= 0x21) ? u8g2_missing_glyph_delta(u8g2) : 0;
    }

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
