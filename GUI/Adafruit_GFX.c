/*
This is the core graphics library for all our displays, providing a common
set of graphics primitives (points, lines, circles, etc.).  It needs to be
paired with a hardware-specific library for each display device we carry
(to handle the lower-level functions).

这是所有显示设备的核心图形库，提供了一套通用的图形绘制原语（点、线、圆等）。
它需要与硬件特定的库配合使用，以处理底层硬件函数。

Adafruit invests time and resources providing this open source code, please
support Adafruit & open-source hardware by purchasing products from Adafruit!

Copyright (c) 2013 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
 */

#include "Adafruit_GFX.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 实用宏定义 | Utility macros
#ifndef ABS
#define ABS(x) ((x) > 0 ? (x) : -(x))  // 取绝对值 | Get absolute value
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))  // 取最小值 | Get minimum value
#endif
#ifndef SWAP
// 交换两个变量的值 | Swap values of two variables
#define SWAP(a, b, T) \
    do {              \
        T t = a;      \
        a = b;        \
        b = t;        \
    } while (0)
#endif
#ifndef CONTAINER_OF
// 通过结构体成员指针获取包含它的结构体指针 | Get struct pointer from member pointer
#define CONTAINER_OF(ptr, type, member) (type*)((char*)ptr - offsetof(type, member))
#endif

/**************************************************************************/
/**
 * @brief   U8G2字体库绘制水平/垂直线的回调函数
 *          Callback function for U8G2 font library to draw horizontal/vertical lines
 * @param   u8g2   U8G2字体上下文指针 | U8G2 font context pointer
 * @param   x      起始X坐标 | Starting X coordinate
 * @param   y      起始Y坐标 | Starting Y coordinate
 * @param   len    线段长度 | Line length
 * @param   dir    方向: 0=右, 1=下, 2=左, 3=上 | Direction: 0=right, 1=down, 2=left, 3=up
 * @param   color  颜色值 | Color value
 */
/**************************************************************************/
static void GFX_u8g2_draw_hv_line(u8g2_font_t* u8g2, int16_t x, int16_t y, int16_t len, uint8_t dir, uint16_t color) {
    // 通过u8g2成员指针反向获取Adafruit_GFX结构体指针
    // Get Adafruit_GFX struct pointer from u8g2 member pointer
    Adafruit_GFX* gfx = CONTAINER_OF(u8g2, Adafruit_GFX, u8g2);
    switch (dir) {
        case 0:  // 向右画水平线 | Draw horizontal line to the right
            GFX_drawFastHLine(gfx, x, y, len, color);
            break;
        case 1:  // 向下画垂直线 | Draw vertical line downward
            GFX_drawFastVLine(gfx, x, y, len, color);
            break;
        case 2:  // 向左画水平线 | Draw horizontal line to the left
            GFX_drawFastHLine(gfx, x - len + 1, y, len, color);
            break;
        case 3:  // 向上画垂直线 | Draw vertical line upward
            GFX_drawFastVLine(gfx, x, y - len + 1, len, color);
            break;
    }
}

/**************************************************************************/
/*!
   @brief    Instatiate a GFX context for graphics
             初始化单色图形上下文
   @param    w   Display width, in pixels
             显示宽度（像素）
   @param    h   Display height, in pixels
             显示高度（像素）
   @param    buffer_height Page buffer height
             页面缓冲区高度
   @note     分页缓冲区机制说明：
             由于嵌入式设备内存有限，无法一次性分配整个屏幕的缓冲区，
             因此采用分页绘制方式。每次只分配一页的缓冲区，
             绘制完成后通过回调函数将数据发送到显示器，然后清空缓冲区绘制下一页。

             Paged buffer mechanism:
             Due to limited memory in embedded devices, we cannot allocate
             a buffer for the entire screen at once. Instead, we use paged
             drawing. Each page is drawn, sent to display via callback,
             then the buffer is cleared for the next page.
*/
/**************************************************************************/
void GFX_begin(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height) {
    // 清零整个GFX结构体 | Zero out the entire GFX structure
    memset(gfx, 0, sizeof(Adafruit_GFX));
    memset(&gfx->u8g2, 0, sizeof(gfx->u8g2));

    // 设置屏幕尺寸 | Set screen dimensions
    gfx->WIDTH = gfx->_width = w;
    gfx->HEIGHT = gfx->_height = h;

    // 设置U8G2字体库的绘线回调函数 | Set U8G2 font library's line drawing callback
    gfx->u8g2.draw_hv_line = GFX_u8g2_draw_hv_line;

    // 分配缓冲区：每行使用 (宽度+7)/8 字节（向上取整到字节边界）
    // Allocate buffer: each row uses (width+7)/8 bytes (rounded up to byte boundary)
    gfx->buffer = malloc(((gfx->WIDTH + 7) / 8) * buffer_height);
    gfx->page_height = buffer_height;

    // 计算总页数（向上取整）| Calculate total pages (rounded up)
    gfx->total_pages = (gfx->HEIGHT / gfx->page_height) + (gfx->HEIGHT % gfx->page_height > 0);

    // 设置默认窗口为整个屏幕 | Set default window to entire screen
    GFX_setWindow(gfx, 0, 0, gfx->WIDTH, gfx->HEIGHT);
}

/**************************************************************************/
/*!
   @brief    Instatiate a 3-color GFX context for graphics
             初始化三色图形上下文（黑/白/红 或 黑/白/黄）
   @param    w   Display width, in pixels
             显示宽度（像素）
   @param    h   Display height, in pixels
             显示高度（像素）
   @param    buffer_height Page buffer height, should be multiple of 2
             页面缓冲区高度，应为2的倍数
   @note     三色模式下，缓冲区被分为两部分：
             - buffer: 黑色像素缓冲区
             - color: 彩色像素缓冲区（红色或黄色）

             In 3-color mode, the buffer is split into two parts:
             - buffer: black pixel buffer
             - color: color pixel buffer (red or yellow)
*/
/**************************************************************************/
void GFX_begin_3c(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height) {
    // 首先按单色模式初始化 | First initialize in monochrome mode
    GFX_begin(gfx, w, h, buffer_height);

    // 页高度减半，因为要分出一半给彩色缓冲区
    // Halve page height because half is needed for color buffer
    gfx->page_height /= 2;

    // 彩色缓冲区指向黑色缓冲区的后半部分
    // Color buffer points to the second half of black buffer
    gfx->color = gfx->buffer + ((gfx->WIDTH + 7) / 8) * gfx->page_height;

    // 重新计算总页数 | Recalculate total pages
    gfx->total_pages = (gfx->HEIGHT / gfx->page_height) + (gfx->HEIGHT % gfx->page_height > 0);
}

/**************************************************************************/
/*!
   @brief    Instatiate a 4-color GFX context for graphics
             初始化四色图形上下文（黑/白/红/黄 或其他四色组合）
   @param    w   Display width, in pixels
             显示宽度（像素）
   @param    h   Display height, in pixels
             显示高度（像素）
   @param    buffer_height Page buffer height, should be multiple of 2
             页面缓冲区高度，应为2的倍数
   @note     四色模式下，每个像素使用2位来表示颜色（00=黑，01=白，10=黄，11=红）。
             缓冲区布局与三色模式不同，color指针指向buffer自身，用于标识4色模式。

             In 4-color mode, each pixel uses 2 bits for color (00=black, 01=white, 10=yellow, 11=red).
             Buffer layout differs from 3-color mode; color pointer points to buffer itself
             to identify 4-color mode.
*/
/**************************************************************************/
void GFX_begin_4c(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height) {
    GFX_begin(gfx, w, h, buffer_height);
    gfx->page_height /= 2;
    // color指向buffer自身，作为4色模式的标识
    // color points to buffer itself as a marker for 4-color mode
    gfx->color = gfx->buffer;
    gfx->total_pages = (gfx->HEIGHT / gfx->page_height) + (gfx->HEIGHT % gfx->page_height > 0);
}

/**************************************************************************/
/**
 * @brief   结束绘图并释放分配的缓冲区内存
 *          End drawing and free allocated buffer memory
 * @param   gfx  图形上下文指针 | Graphics context pointer
 */
/**************************************************************************/
void GFX_end(Adafruit_GFX* gfx) {
    if (gfx->buffer) free(gfx->buffer);
}

/**************************************************************************/
/**
 * @brief   开始第一页的绘制，清空缓冲区并重置页面计数器
 *          Start drawing the first page, clear buffer and reset page counter
 * @param   gfx  图形上下文指针 | Graphics context pointer
 */
/**************************************************************************/
void GFX_firstPage(Adafruit_GFX* gfx) {
    // 用白色填充屏幕（清空缓冲区）| Fill screen with white (clear buffer)
    GFX_fillScreen(gfx, GFX_WHITE);
    gfx->current_page = 0;
}

/**************************************************************************/
/**
 * @brief   处理下一页的绘制
 *          Process drawing of the next page
 * @param   gfx       图形上下文指针 | Graphics context pointer
 * @param   callback  缓冲区数据回调函数，用于将当前页数据发送到显示器
 *                    Buffer data callback for sending current page to display
 * @param   user_data 传递给回调函数的用户数据 | User data passed to callback
 * @return  true - 还有更多页需要绘制 | More pages to draw
 *          false - 所有页面绘制完成 | All pages completed
 * @note    分页绘制工作流程：
 *          1. 调用 GFX_firstPage() 初始化
 *          2. 执行绘图操作（drawXXX函数）
 *          3. 调用 GFX_nextPage() 输出当前页并准备下一页
 *          4. 重复步骤2-3直到 GFX_nextPage() 返回 false
 *
 *          Paged drawing workflow:
 *          1. Call GFX_firstPage() to initialize
 *          2. Perform drawing operations (drawXXX functions)
 *          3. Call GFX_nextPage() to output current page and prepare next
 *          4. Repeat steps 2-3 until GFX_nextPage() returns false
 */
/**************************************************************************/
bool GFX_nextPage(Adafruit_GFX* gfx, buffer_callback callback, void* user_data) {
    if (callback) {
        // 计算当前页的起始Y坐标 | Calculate starting Y coordinate of current page
        int16_t page_ys = gfx->current_page * gfx->page_height;

        // 检查是否设置了局部窗口（非全屏）| Check if partial window is set (not full screen)
        if (gfx->px != 0 || gfx->py != 0 || gfx->pw != gfx->_width || gfx->ph != gfx->_height) {
            // 局部刷新模式 | Partial refresh mode
            int16_t page_ye = gfx->current_page < gfx->total_pages - 1 ? page_ys + gfx->page_height : gfx->HEIGHT;
            uint16_t dest_ys = gfx->py + page_ys;  // transposed 转换后的起始Y
            uint16_t dest_ye = MIN(gfx->py + gfx->ph, gfx->py + page_ye);
            if (dest_ye > dest_ys)
                callback(user_data, gfx->buffer, gfx->color, gfx->px, dest_ys, gfx->pw, dest_ye - dest_ys);
        } else {
            // 全屏刷新模式 | Full screen refresh mode
            int16_t height = MIN(gfx->page_height, gfx->HEIGHT - page_ys);
            callback(user_data, gfx->buffer, gfx->color, 0, page_ys, gfx->WIDTH, height);
        }
    }

    // 准备下一页 | Prepare for next page
    gfx->current_page++;
    GFX_fillScreen(gfx, GFX_WHITE);  // 清空缓冲区 | Clear buffer

    return gfx->current_page < gfx->total_pages;
}

/**************************************************************************/
/*!
    @brief      Set rotation setting for display
                设置显示器的旋转角度
    @param  r   0 thru 3 corresponding to 4 cardinal rotations
                0到3对应4个基本旋转方向
    @note       旋转会影响坐标系统：
                - 0度：原点在左上角，X向右，Y向下（默认）
                - 90度：原点在右上角，X向下，Y向左
                - 180度：原点在右下角，X向左，Y向上
                - 270度：原点在左下角，X向上，Y向右

                Rotation affects the coordinate system:
                - 0 deg: origin at top-left, X to right, Y down (default)
                - 90 deg: origin at top-right, X down, Y left
                - 180 deg: origin at bottom-right, X left, Y up
                - 270 deg: origin at bottom-left, X up, Y right
*/
/**************************************************************************/
void GFX_setRotation(Adafruit_GFX* gfx, GFX_Rotate r) {
    gfx->rotation = r;
    switch (gfx->rotation) {
        case GFX_ROTATE_0:
        case GFX_ROTATE_180:
            // 0度和180度旋转时，宽高保持不变 | Width/height unchanged for 0 and 180 degrees
            gfx->_width = gfx->WIDTH;
            gfx->_height = gfx->HEIGHT;
            break;
        case GFX_ROTATE_90:
        case GFX_ROTATE_270:
            // 90度和270度旋转时，宽高互换 | Width/height swapped for 90 and 270 degrees
            gfx->_width = gfx->HEIGHT;
            gfx->_height = gfx->WIDTH;
            break;
    }
}

/**************************************************************************/
/*!
    setWindow, use parameters according to actual rotation.
    设置局部刷新窗口，参数根据实际旋转角度使用。

    x and w should be multiple of 8, for rotation 0 or 2,
    y and h should be multiple of 8, for rotation 1 or 3,
    else window is increased as needed,
    this is an addressing limitation of the e-paper controllers

    对于0度或180度旋转，x和w应该是8的倍数；
    对于90度或270度旋转，y和h应该是8的倍数；
    否则窗口会根据需要自动扩大。
    这是电子纸控制器的寻址限制。
*/
/**************************************************************************/
void GFX_setWindow(Adafruit_GFX* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    // 限制窗口参数在有效范围内 | Clamp window parameters to valid range
    gfx->px = MIN(x, gfx->_width);
    gfx->py = MIN(y, gfx->_height);
    gfx->pw = MIN(w, gfx->_width - gfx->px);
    gfx->ph = MIN(h, gfx->_height - gfx->py);

    // 根据旋转角度转换坐标 | Transform coordinates based on rotation
    switch (gfx->rotation) {
        case GFX_ROTATE_0:
            // 无需转换 | No transformation needed
            break;
        case GFX_ROTATE_90:
            // 交换x/y和宽/高，然后调整x坐标 | Swap x/y and w/h, then adjust x
            SWAP(gfx->px, gfx->py, uint16_t);
            SWAP(gfx->pw, gfx->ph, uint16_t);
            gfx->px = gfx->WIDTH - gfx->px - gfx->pw;
            break;
        case GFX_ROTATE_180:
            // 翻转x和y坐标 | Flip x and y coordinates
            gfx->px = gfx->WIDTH - gfx->px - gfx->pw;
            gfx->py = gfx->HEIGHT - gfx->py - gfx->ph;
            break;
        case GFX_ROTATE_270:
            // 交换x/y和宽/高，然后调整y坐标 | Swap x/y and w/h, then adjust y
            SWAP(gfx->px, gfx->py, uint16_t);
            SWAP(gfx->pw, gfx->ph, uint16_t);
            gfx->py = gfx->HEIGHT - gfx->py - gfx->ph;
            break;
    }

    // 调整宽度和x坐标到8的倍数边界（电子纸控制器要求）
    // Align width and x to 8-pixel boundary (e-paper controller requirement)
    gfx->pw += gfx->px % 8;                          // 补偿x偏移 | Compensate for x offset
    if (gfx->pw % 8 > 0) gfx->pw += 8 - (gfx->pw % 8);  // 向上取整到8的倍数 | Round up to multiple of 8
    gfx->px -= gfx->px % 8;                          // 向下对齐到8的倍数 | Align down to multiple of 8
}

/**************************************************************************/
/**
 * @brief   将16位RGB565颜色转换为4色电子纸的2位颜色值
 *          Convert 16-bit RGB565 color to 2-bit color value for 4-color e-paper
 * @param   color  16位RGB565颜色值 | 16-bit RGB565 color value
 * @return  2位颜色值: 0x00=黑, 0x01=白, 0x02=黄, 0x03=红
 *          2-bit color value: 0x00=black, 0x01=white, 0x02=yellow, 0x03=red
 * @note    使用静态变量缓存上次转换结果以提高性能
 *          Uses static variables to cache last conversion result for performance
 */
/**************************************************************************/
static uint8_t color4(uint16_t color) {
    // 缓存上次转换结果 | Cache last conversion result
    static uint16_t _prev_color = GFX_BLACK;
    static uint8_t _prev_color4 = 0x00;  // black 黑色
    if (color == _prev_color) return _prev_color4;

    uint8_t cv4 = 0x00;
    // 处理预定义颜色 | Handle predefined colors
    switch (color) {
        case GFX_BLACK:
            cv4 = 0x00;  // 黑色 | Black
            break;
        case GFX_WHITE:
            cv4 = 0x01;  // 白色 | White
            break;
        case GFX_GREEN:
            cv4 = 0x02;  // 绿色映射为黄色 | Green maps to yellow
            break;  // use yellow?
        case GFX_BLUE:
            cv4 = 0x00;  // 蓝色映射为黑色 | Blue maps to black
            break;  // use black
        case GFX_RED:
            cv4 = 0x03;  // 红色 | Red
            break;
        case GFX_YELLOW:
            cv4 = 0x02;  // 黄色 | Yellow
            break;
        case GFX_ORANGE:
            cv4 = 0x02;  // 橙色映射为黄色 | Orange maps to yellow
            break;  // use yellow?
        default: {
            // 根据RGB分量判断最接近的4色 | Determine closest 4-color based on RGB components
            // 从RGB565格式提取并扩展各颜色分量 | Extract and expand color components from RGB565
            uint16_t red = color & 0xF800;           // 红色分量（高5位）
            uint16_t green = (color & 0x07E0) << 5;  // 绿色分量（中6位），左移对齐
            uint16_t blue = (color & 0x001F) << 11;  // 蓝色分量（低5位），左移对齐
            if ((red < 0x8000) && (green < 0x8000) && (blue < 0x8000))
                cv4 = 0x00;  // black 暗色 -> 黑色
            else if ((red >= 0x8000) && (green >= 0x8000) && (blue >= 0x8000))
                cv4 = 0x01;  // white 亮色 -> 白色
            else if ((red >= 0x8000) && (blue >= 0x8000))
                cv4 = 0x03;  //  red, blue > red 红蓝混合 -> 红色
            else if ((green >= 0x8000) && (blue >= 0x8000))
                cv4 = 0x01;  //  green, blue > white 青色 -> 白色
            else if ((red >= 0x8000) && (green >= 0xC000))
                cv4 = 0x02;  // yellow 高亮黄 -> 黄色
            else if ((red >= 0x8000) && (green >= 0x4000))
                cv4 = 0x03;  // orange > red 橙色 -> 红色
            else if (red >= 0x8000)
                cv4 = 0x03;  // red 纯红 -> 红色
            else if (green >= 0x8000)
                cv4 = 0x00;  // green > black 纯绿 -> 黑色
            else
                cv4 = 0x03;  // blue 蓝色 -> 红色
        } break;
    }
    // 更新缓存 | Update cache
    _prev_color = color;
    _prev_color4 = cv4;
    return cv4;
}
/**************************************************************************/
/*!
   @brief    Draw a pixel
             绘制单个像素点
    @param   x   x coordinate X坐标
    @param   y   y coordinate Y坐标
   @param    color 16-bit 5-6-5 Color to fill with
             16位RGB565格式颜色值
   @note     这是最基本的绘图函数，所有其他绘图函数最终都会调用此函数。
             函数内部处理：
             1. 坐标边界检查
             2. 根据旋转角度转换坐标
             3. 窗口裁剪
             4. 分页处理
             5. 根据颜色模式（单色/3色/4色）写入像素数据

             This is the most fundamental drawing function. All other drawing
             functions ultimately call this function. It handles:
             1. Coordinate boundary checking
             2. Coordinate transformation based on rotation
             3. Window clipping
             4. Page handling
             5. Writing pixel data based on color mode (mono/3-color/4-color)
*/
/**************************************************************************/
void GFX_drawPixel(Adafruit_GFX* gfx, int16_t x, int16_t y, uint16_t color) {
    // 边界检查：确保坐标在屏幕范围内 | Boundary check: ensure coordinates are within screen
    if (x < 0 || x >= gfx->_width || y < 0 || y >= gfx->_height) return;

    // 根据旋转角度转换坐标到物理坐标 | Transform coordinates to physical based on rotation
    switch (gfx->rotation) {
        case GFX_ROTATE_0:
            // 无需转换 | No transformation needed
            break;
        case GFX_ROTATE_90:
            // 90度顺时针旋转 | 90 degrees clockwise rotation
            SWAP(x, y, int16_t);
            x = gfx->WIDTH - x - 1;
            break;
        case GFX_ROTATE_180:
            // 180度旋转 | 180 degrees rotation
            x = gfx->WIDTH - x - 1;
            y = gfx->HEIGHT - y - 1;
            break;
        case GFX_ROTATE_270:
            // 270度顺时针旋转 | 270 degrees clockwise rotation
            SWAP(x, y, int16_t);
            y = gfx->HEIGHT - y - 1;
            break;
    }

    // transpose partial window to 0,0
    // 将坐标转换到局部窗口的相对坐标 | Convert to relative coordinates within partial window
    x -= gfx->px;
    y -= gfx->py;

    // clip to (partial) window
    // 裁剪到局部窗口范围 | Clip to partial window bounds
    if (x < 0 || x >= gfx->pw || y < 0 || y >= gfx->ph) return;

    // adjust for current page
    // 调整为当前页面的相对坐标 | Adjust to current page relative coordinates
    y -= gfx->current_page * gfx->page_height;

    // check if in current page
    // 检查是否在当前绘制页面内 | Check if within current drawing page
    if (y < 0 || y >= gfx->page_height) return;

    // 根据颜色模式写入像素数据 | Write pixel data based on color mode
    if (gfx->color == gfx->buffer) {  // 4c 四色模式
        // 四色模式：每像素2位，每字节存储4个像素
        // 4-color mode: 2 bits per pixel, 4 pixels per byte
        uint32_t i = x / 4 + ((uint32_t)y) * (gfx->pw / 4);
        uint8_t pv = color4(color);  // 转换为2位颜色值 | Convert to 2-bit color value
        // 根据像素在字节中的位置写入对应的2位
        // Write 2 bits based on pixel position within byte
        switch (x % 4) {
            case 0:  // 最高2位 | Highest 2 bits (bits 7-6)
                gfx->buffer[i] = (gfx->buffer[i] & 0x3F) | (pv << 6);
                break;
            case 1:  // 次高2位 | Second highest 2 bits (bits 5-4)
                gfx->buffer[i] = (gfx->buffer[i] & 0xCF) | (pv << 4);
                break;
            case 2:  // 次低2位 | Second lowest 2 bits (bits 3-2)
                gfx->buffer[i] = (gfx->buffer[i] & 0xF3) | (pv << 2);
                break;
            case 3:  // 最低2位 | Lowest 2 bits (bits 1-0)
                gfx->buffer[i] = (gfx->buffer[i] & 0xFC) | pv;
                break;
        }
    } else if (gfx->color != NULL) {  // 3c 三色模式
        // 三色模式：使用两个独立的缓冲区
        // - buffer: 黑白信息（1=白，0=黑）
        // - color: 彩色信息（0=彩色，1=非彩色）
        // 3-color mode: uses two separate buffers
        // - buffer: black/white info (1=white, 0=black)
        // - color: color info (0=colored, 1=not colored)
        uint16_t i = x / 8 + y * (gfx->pw / 8);
        gfx->buffer[i] |= 0x80 >> (x & 7);  // 默认设为白色 | Default to white
        gfx->color[i] |= 0x80 >> (x & 7);   // 默认设为非彩色 | Default to not colored
        if (color == GFX_BLACK)
            gfx->buffer[i] &= ~(0x80 >> (x & 7));  // 黑色：清除buffer位 | Black: clear buffer bit
        else if (color != GFX_WHITE)
            gfx->color[i] &= ~(0x80 >> (x & 7));   // 彩色：清除color位 | Colored: clear color bit
    } else {
        // 单色模式：每像素1位，每字节存储8个像素
        // Monochrome mode: 1 bit per pixel, 8 pixels per byte
        uint16_t i = x / 8 + y * (gfx->pw / 8);
        if (color == GFX_WHITE)
            gfx->buffer[i] |= 0x80 >> (x & 7);   // 白色：设置位为1 | White: set bit to 1
        else
            gfx->buffer[i] &= ~(0x80 >> (x & 7));  // 黑色：设置位为0 | Black: set bit to 0
    }
}

/**************************************************************************/
/*!
   @brief    Draw a line.  Bresenham's algorithm - thx wikpedia
             使用Bresenham算法绘制直线
    @param    x0  Start point x coordinate 起点X坐标
    @param    y0  Start point y coordinate 起点Y坐标
    @param    x1  End point x coordinate 终点X坐标
    @param    y1  End point y coordinate 终点Y坐标
    @param    color 16-bit 5-6-5 Color to draw with 绘制颜色
   @note     Bresenham算法是一种高效的直线绘制算法，
             仅使用整数运算（加减和位移），避免浮点运算。
             算法原理：通过误差累积决定每一步是否需要在副轴方向前进。

             Bresenham's algorithm is an efficient line drawing algorithm
             using only integer operations (add, subtract, shift), avoiding floating point.
             It determines whether to step in the minor axis direction based on error accumulation.
*/
/**************************************************************************/
void GFX_drawLine(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    // 判断是否为陡峭线（斜率>1）| Check if line is steep (slope > 1)
    int16_t steep = ABS(y1 - y0) > ABS(x1 - x0);
    if (steep) {
        // 对于陡峭线，交换x和y，使迭代沿y轴进行
        // For steep lines, swap x and y to iterate along y-axis
        SWAP(x0, y0, int16_t);
        SWAP(x1, y1, int16_t);
    }

    // 确保从左向右绘制 | Ensure drawing from left to right
    if (x0 > x1) {
        SWAP(x0, x1, int16_t);
        SWAP(y0, y1, int16_t);
    }

    int16_t dx, dy;
    dx = x1 - x0;        // x方向差值 | Delta in x direction
    dy = ABS(y1 - y0);   // y方向差值（绝对值）| Delta in y direction (absolute)

    int16_t err = dx / 2;  // 误差累积器，初始化为dx/2 | Error accumulator, initialized to dx/2
    int16_t ystep;

    // 确定y方向步进值 | Determine y step direction
    if (y0 < y1) {
        ystep = 1;   // 向下 | Downward
    } else {
        ystep = -1;  // 向上 | Upward
    }

    // 主循环：沿主轴（x或交换后的y）逐像素绘制
    // Main loop: draw pixel by pixel along major axis
    for (; x0 <= x1; x0++) {
        if (steep) {
            GFX_drawPixel(gfx, y0, x0, color);  // 陡峭线：交换回原坐标 | Steep: swap back
        } else {
            GFX_drawPixel(gfx, x0, y0, color);  // 非陡峭线：直接绘制 | Non-steep: draw directly
        }
        err -= dy;  // 累加误差 | Accumulate error
        if (err < 0) {
            y0 += ystep;  // 副轴步进 | Step in minor axis
            err += dx;    // 重置误差 | Reset error
        }
    }
}

/**************************************************************************/
/*!
   @brief    Draw a dotted line.  Bresenham's algorithm - thx wikpedia
             使用Bresenham算法绘制虚线
    @param    x0  Start point x coordinate 起点X坐标
    @param    y0  Start point y coordinate 起点Y坐标
    @param    x1  End point x coordinate 终点X坐标
    @param    y1  End point y coordinate 终点Y坐标
    @param    color 16-bit 5-6-5 Color to draw with 绘制颜色
    @param    dot_len 每段实线的像素长度 | Length of each dot segment in pixels
    @param    space_len 每段空白的像素长度 | Length of each space segment in pixels
   @note     在Bresenham算法基础上添加虚线控制逻辑。
             通过计数器交替切换"绘制"和"跳过"状态实现虚线效果。

             Based on Bresenham's algorithm with added dotted line control.
             Alternates between "draw" and "skip" states using counters.
*/
/**************************************************************************/
void GFX_drawDottedLine(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color,
                        uint8_t dot_len, uint8_t space_len) {
    int16_t steep = ABS(y1 - y0) > ABS(x1 - x0);
    if (steep) {
        SWAP(x0, y0, int16_t);
        SWAP(x1, y1, int16_t);
    }

    if (x0 > x1) {
        SWAP(x0, x1, int16_t);
        SWAP(y0, y1, int16_t);
    }

    int16_t dx, dy;
    dx = x1 - x0;
    dy = ABS(y1 - y0);

    int16_t err = dx / 2;
    int16_t ystep;

    if (y0 < y1) {
        ystep = 1;
    } else {
        ystep = -1;
    }

    uint8_t draw = 1;    // 绘制状态标志 | Draw state flag (1=drawing, 0=skipping)
    uint8_t len = 0;     // 当前段的像素计数 | Pixel counter for current segment
    for (; x0 <= x1; x0++) {
        if (draw) {
            // 绘制状态：画像素 | Drawing state: draw pixel
            if (steep) {
                GFX_drawPixel(gfx, y0, x0, color);
            } else {
                GFX_drawPixel(gfx, x0, y0, color);
            }
            len++;
            if (len >= dot_len) {
                // 达到实线长度，切换到空白状态 | Reached dot length, switch to space state
                len = 0;
                draw = 0;
            }
        } else {
            // 空白状态：跳过像素 | Space state: skip pixel
            len++;
            if (len >= space_len) {
                // 达到空白长度，切换到绘制状态 | Reached space length, switch to draw state
                len = 0;
                draw = 1;
            }
        }
        err -= dy;
        if (err < 0) {
            y0 += ystep;
            err += dx;
        }
    }
}

/**************************************************************************/
/*!
   @brief    Draw a perfectly vertical line
             绘制垂直线（优化版本）
    @param    x   Top-most x coordinate 顶部X坐标
    @param    y   Top-most y coordinate 顶部Y坐标
    @param    h   Height in pixels 高度（像素）
   @param    color 16-bit 5-6-5 Color to fill with 填充颜色
*/
/**************************************************************************/
void GFX_drawFastVLine(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t h, uint16_t color) {
    // 通过调用通用直线函数实现 | Implemented by calling generic line function
    GFX_drawLine(gfx, x, y, x, y + h - 1, color);
}

/**************************************************************************/
/*!
   @brief    Draw a perfectly horizontal line
             绘制水平线（优化版本）
    @param    x   Left-most x coordinate 最左侧X坐标
    @param    y   Left-most y coordinate 最左侧Y坐标
    @param    w   Width in pixels 宽度（像素）
   @param    color 16-bit 5-6-5 Color to fill with 填充颜色
*/
/**************************************************************************/
void GFX_drawFastHLine(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, uint16_t color) {
    // 通过调用通用直线函数实现 | Implemented by calling generic line function
    GFX_drawLine(gfx, x, y, x + w - 1, y, color);
}

/**************************************************************************/
/*!
   @brief    Fill a rectangle completely with one color.
             用单一颜色填充矩形
    @param    x   Top left corner x coordinate 左上角X坐标
    @param    y   Top left corner y coordinate 左上角Y坐标
    @param    w   Width in pixels 宽度（像素）
    @param    h   Height in pixels 高度（像素）
   @param    color 16-bit 5-6-5 Color to fill with 填充颜色
   @note     通过绘制多条垂直线实现矩形填充
             Implements rectangle filling by drawing multiple vertical lines
*/
/**************************************************************************/
void GFX_fillRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    for (int16_t i = x; i < x + w; i++) {
        GFX_drawFastVLine(gfx, i, y, h, color);
    }
}

/**************************************************************************/
/*!
   @brief    Fill the screen completely with one color.
             用单一颜色填充整个屏幕（清屏）
    @param    color 16-bit 5-6-5 Color to fill with 填充颜色
   @note     直接操作缓冲区内存，比逐像素绘制效率高得多。
             根据颜色模式（单色/3色/4色）使用不同的填充策略。

             Directly manipulates buffer memory, much more efficient than pixel-by-pixel drawing.
             Uses different filling strategies based on color mode (mono/3-color/4-color).
*/
/**************************************************************************/
void GFX_fillScreen(Adafruit_GFX* gfx, uint16_t color) {
    // 计算单页缓冲区大小（字节数）| Calculate page buffer size in bytes
    uint32_t size = ((gfx->WIDTH + 7) / 8) * gfx->page_height;

    if (gfx->color == gfx->buffer) {        // 4c 四色模式
        // 四色模式：每字节4个像素，用转换后的颜色值复制填充整个字节
        // 4-color mode: 4 pixels per byte, replicate converted color to fill entire byte
        uint8_t pv = color4(color) * 0x55;  // 0b01010101 将2位颜色值扩展到8位
        memset(gfx->buffer, pv, size * 2);  // 两倍大小（包含黑白和彩色数据）
    } else {
        // 单色或三色模式 | Mono or 3-color mode
        // 填充黑白缓冲区 | Fill black/white buffer
        memset(gfx->buffer, color == GFX_WHITE ? 0xFF : 0x00, size);
        // 如果是三色模式，填充彩色缓冲区 | If 3-color mode, fill color buffer
        if (gfx->color != NULL) memset(gfx->color, color == GFX_RED ? 0x00 : 0xFF, size);
    }
}

/**************************************************************************/
/*!
   @brief    Draw a circle outline
             绘制空心圆
    @param    x0   Center-point x coordinate 圆心X坐标
    @param    y0   Center-point y coordinate 圆心Y坐标
    @param    r   Radius of circle 圆的半径
    @param    color 16-bit 5-6-5 Color to draw with 绘制颜色
   @note     使用中点圆算法（Midpoint Circle Algorithm），
             利用圆的8对称性，只需计算1/8圆弧即可绘制整个圆。

             Uses Midpoint Circle Algorithm, leveraging the 8-fold symmetry
             of a circle to draw the entire circle by computing only 1/8 of it.
*/
/**************************************************************************/
void GFX_drawCircle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    int16_t f = 1 - r;      // 决策参数 | Decision parameter
    int16_t ddF_x = 1;      // x方向增量 | Delta for x direction
    int16_t ddF_y = -2 * r; // y方向增量 | Delta for y direction
    int16_t x = 0;
    int16_t y = r;

    // 绘制圆的4个基本点（上下左右）| Draw 4 cardinal points
    GFX_drawPixel(gfx, x0, y0 + r, color);  // 底部 | Bottom
    GFX_drawPixel(gfx, x0, y0 - r, color);  // 顶部 | Top
    GFX_drawPixel(gfx, x0 + r, y0, color);  // 右侧 | Right
    GFX_drawPixel(gfx, x0 - r, y0, color);  // 左侧 | Left

    // 利用8对称性绘制圆 | Draw circle using 8-fold symmetry
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        // 绘制8个对称点 | Draw 8 symmetric points
        GFX_drawPixel(gfx, x0 + x, y0 + y, color);  // 第4象限 | Quadrant 4
        GFX_drawPixel(gfx, x0 - x, y0 + y, color);  // 第3象限 | Quadrant 3
        GFX_drawPixel(gfx, x0 + x, y0 - y, color);  // 第1象限 | Quadrant 1
        GFX_drawPixel(gfx, x0 - x, y0 - y, color);  // 第2象限 | Quadrant 2
        GFX_drawPixel(gfx, x0 + y, y0 + x, color);  // 第4象限（反射）| Quadrant 4 (reflected)
        GFX_drawPixel(gfx, x0 - y, y0 + x, color);  // 第3象限（反射）| Quadrant 3 (reflected)
        GFX_drawPixel(gfx, x0 + y, y0 - x, color);  // 第1象限（反射）| Quadrant 1 (reflected)
        GFX_drawPixel(gfx, x0 - y, y0 - x, color);  // 第2象限（反射）| Quadrant 2 (reflected)
    }
}

/**************************************************************************/
/*!
    @brief    Quarter-circle drawer, used to do circles and roundrects
    @param    x0   Center-point x coordinate
    @param    y0   Center-point y coordinate
    @param    r   Radius of circle
    @param    cornername  Mask bit #1 or bit #2 to indicate which quarters of
   the circle we're doing
    @param    color 16-bit 5-6-5 Color to draw with
*/
/**************************************************************************/
void GFX_drawCircleHelper(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint8_t cornername, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        if (cornername & 0x4) {
            GFX_drawPixel(gfx, x0 + x, y0 + y, color);
            GFX_drawPixel(gfx, x0 + y, y0 + x, color);
        }
        if (cornername & 0x2) {
            GFX_drawPixel(gfx, x0 + x, y0 - y, color);
            GFX_drawPixel(gfx, x0 + y, y0 - x, color);
        }
        if (cornername & 0x8) {
            GFX_drawPixel(gfx, x0 - y, y0 + x, color);
            GFX_drawPixel(gfx, x0 - x, y0 + y, color);
        }
        if (cornername & 0x1) {
            GFX_drawPixel(gfx, x0 - y, y0 - x, color);
            GFX_drawPixel(gfx, x0 - x, y0 - y, color);
        }
    }
}

/**************************************************************************/
/*!
   @brief    Draw a circle with filled color
    @param    x0   Center-point x coordinate
    @param    y0   Center-point y coordinate
    @param    r   Radius of circle
    @param    color 16-bit 5-6-5 Color to fill with
*/
/**************************************************************************/
void GFX_fillCircle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    GFX_drawFastVLine(gfx, x0, y0 - r, 2 * r + 1, color);
    GFX_fillCircleHelper(gfx, x0, y0, r, 3, 0, color);
}

/**************************************************************************/
/*!
    @brief  Quarter-circle drawer with fill, used for circles and roundrects
    @param  x0       Center-point x coordinate
    @param  y0       Center-point y coordinate
    @param  r        Radius of circle
    @param  corners  Mask bits indicating which quarters we're doing
    @param  delta    Offset from center-point, used for round-rects
    @param  color    16-bit 5-6-5 Color to fill with
*/
/**************************************************************************/
void GFX_fillCircleHelper(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint8_t corners, int16_t delta,
                          uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;
    int16_t px = x;
    int16_t py = y;

    delta++;  // Avoid some +1's in the loop

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        // These checks avoid double-drawing certain lines, important
        // for the SSD1306 library which has an INVERT drawing mode.
        if (x < (y + 1)) {
            if (corners & 1) GFX_drawFastVLine(gfx, x0 + x, y0 - y, 2 * y + delta, color);
            if (corners & 2) GFX_drawFastVLine(gfx, x0 - x, y0 - y, 2 * y + delta, color);
        }
        if (y != py) {
            if (corners & 1) GFX_drawFastVLine(gfx, x0 + py, y0 - px, 2 * px + delta, color);
            if (corners & 2) GFX_drawFastVLine(gfx, x0 - py, y0 - px, 2 * px + delta, color);
            py = y;
        }
        px = x;
    }
}

/**************************************************************************/
/*!
   @brief   Draw a rectangle with no fill color
    @param    x   Top left corner x coordinate
    @param    y   Top left corner y coordinate
    @param    w   Width in pixels
    @param    h   Height in pixels
    @param    color 16-bit 5-6-5 Color to draw with
*/
/**************************************************************************/
void GFX_drawRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    GFX_drawFastHLine(gfx, x, y, w, color);
    GFX_drawFastHLine(gfx, x, y + h - 1, w, color);
    GFX_drawFastVLine(gfx, x, y, h, color);
    GFX_drawFastVLine(gfx, x + w - 1, y, h, color);
}

/**************************************************************************/
/*!
   @brief   Draw a rounded rectangle with no fill color
    @param    x   Top left corner x coordinate
    @param    y   Top left corner y coordinate
    @param    w   Width in pixels
    @param    h   Height in pixels
    @param    r   Radius of corner rounding
    @param    color 16-bit 5-6-5 Color to draw with
*/
/**************************************************************************/
void GFX_drawRoundRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    int16_t max_radius = ((w < h) ? w : h) / 2;  // 1/2 minor axis
    if (r > max_radius) r = max_radius;
    // smarter version
    GFX_drawFastHLine(gfx, x + r, y, w - 2 * r, color);          // Top
    GFX_drawFastHLine(gfx, x + r, y + h - 1, w - 2 * r, color);  // Bottom
    GFX_drawFastVLine(gfx, x, y + r, h - 2 * r, color);          // Left
    GFX_drawFastVLine(gfx, x + w - 1, y + r, h - 2 * r, color);  // Right
    // draw four corners
    GFX_drawCircleHelper(gfx, x + r, y + r, r, 1, color);
    GFX_drawCircleHelper(gfx, x + w - r - 1, y + r, r, 2, color);
    GFX_drawCircleHelper(gfx, x + w - r - 1, y + h - r - 1, r, 4, color);
    GFX_drawCircleHelper(gfx, x + r, y + h - r - 1, r, 8, color);
}

/**************************************************************************/
/*!
   @brief   Draw a rounded rectangle with fill color
    @param    x   Top left corner x coordinate
    @param    y   Top left corner y coordinate
    @param    w   Width in pixels
    @param    h   Height in pixels
    @param    r   Radius of corner rounding
    @param    color 16-bit 5-6-5 Color to draw/fill with
*/
/**************************************************************************/
void GFX_fillRoundRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    int16_t max_radius = ((w < h) ? w : h) / 2;  // 1/2 minor axis
    if (r > max_radius) r = max_radius;
    // smarter version
    GFX_fillRect(gfx, x + r, y, w - 2 * r, h, color);
    // draw four corners
    GFX_fillCircleHelper(gfx, x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
    GFX_fillCircleHelper(gfx, x + r, y + r, r, 2, h - 2 * r - 1, color);
}

/**************************************************************************/
/*!
   @brief   Draw a triangle with no fill color
    @param    x0  Vertex #0 x coordinate
    @param    y0  Vertex #0 y coordinate
    @param    x1  Vertex #1 x coordinate
    @param    y1  Vertex #1 y coordinate
    @param    x2  Vertex #2 x coordinate
    @param    y2  Vertex #2 y coordinate
    @param    color 16-bit 5-6-5 Color to draw with
*/
/**************************************************************************/
void GFX_drawTriangle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color) {
    GFX_drawLine(gfx, x0, y0, x1, y1, color);
    GFX_drawLine(gfx, x1, y1, x2, y2, color);
    GFX_drawLine(gfx, x2, y2, x0, y0, color);
}

/**************************************************************************/
/*!
   @brief     Draw a triangle with color-fill
    @param    x0  Vertex #0 x coordinate
    @param    y0  Vertex #0 y coordinate
    @param    x1  Vertex #1 x coordinate
    @param    y1  Vertex #1 y coordinate
    @param    x2  Vertex #2 x coordinate
    @param    y2  Vertex #2 y coordinate
    @param    color 16-bit 5-6-5 Color to fill/draw with
*/
/**************************************************************************/
void GFX_fillTriangle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color) {
    int16_t a, b, y, last;

    // Sort coordinates by Y order (y2 >= y1 >= y0)
    if (y0 > y1) {
        SWAP(y0, y1, int16_t);
        SWAP(x0, x1, int16_t);
    }
    if (y1 > y2) {
        SWAP(y2, y1, int16_t);
        SWAP(x2, x1, int16_t);
    }
    if (y0 > y1) {
        SWAP(y0, y1, int16_t);
        SWAP(x0, x1, int16_t);
    }

    if (y0 == y2) {  // Handle awkward all-on-same-line case as its own thing
        a = b = x0;
        if (x1 < a)
            a = x1;
        else if (x1 > b)
            b = x1;
        if (x2 < a)
            a = x2;
        else if (x2 > b)
            b = x2;
        GFX_drawFastHLine(gfx, a, y0, b - a + 1, color);
        return;
    }

    int16_t dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1, dy12 = y2 - y1;
    int32_t sa = 0, sb = 0;

    // For upper part of triangle, find scanline crossings for segments
    // 0-1 and 0-2.  If y1=y2 (flat-bottomed triangle), the scanline y1
    // is included here (and second loop will be skipped, avoiding a /0
    // error there), otherwise scanline y1 is skipped here and handled
    // in the second loop...which also avoids a /0 error here if y0=y1
    // (flat-topped triangle).
    if (y1 == y2)
        last = y1;  // Include y1 scanline
    else
        last = y1 - 1;  // Skip it

    for (y = y0; y <= last; y++) {
        a = x0 + sa / dy01;
        b = x0 + sb / dy02;
        sa += dx01;
        sb += dx02;
        /* longhand:
        a = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        b = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        */
        if (a > b) SWAP(a, b, int16_t);
        GFX_drawFastHLine(gfx, a, y, b - a + 1, color);
    }

    // For lower part of triangle, find scanline crossings for segments
    // 0-2 and 1-2.  This loop is skipped if y1=y2.
    sa = (int32_t)dx12 * (y - y1);
    sb = (int32_t)dx02 * (y - y0);
    for (; y <= y2; y++) {
        a = x1 + sa / dy12;
        b = x0 + sb / dy02;
        sa += dx12;
        sb += dx02;
        /* longhand:
        a = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        b = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        */
        if (a > b) SWAP(a, b, int16_t);
        GFX_drawFastHLine(gfx, a, y, b - a + 1, color);
    }
}

/**************************************************************************/
/*!
   @brief      Draw a RAM-resident 1-bit image at the specified (x,y) position,
   using the specified foreground color (unset bits are transparent).
    @param    x   Top left corner x coordinate
    @param    y   Top left corner y coordinate
    @param    bitmap  byte array with monochrome bitmap
    @param    w   Width of bitmap in pixels
    @param    h   Height of bitmap in pixels
    @param    color 16-bit 5-6-5 Color to draw with
    @param    invert When true, will invert the bitmap
*/
/**************************************************************************/
void GFX_drawBitmap(Adafruit_GFX* gfx, int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h,
                    uint16_t color, bool invert) {
    int16_t byteWidth = (w + 7) / 8;  // Bitmap scanline pad = whole byte
    uint8_t byte = 0;

    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            if (i & 7)
                byte <<= 1;
            else
                byte = bitmap[j * byteWidth + i / 8];
            if (((byte & 0x80) == 0x80) ^ invert) GFX_drawPixel(gfx, x + i, y + j, color);
        }
    }
}

/*

  U8g2_for_Adafruit_GFX.cpp

  Add unicode support and U8g2 fonts to Adafruit GFX libraries.
  为Adafruit GFX库添加Unicode支持和U8G2字体。

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

/**************************************************************************/
/**
 * @brief   设置文本光标位置并重置UTF-8解码器状态
 *          Set text cursor position and reset UTF-8 decoder state
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @param   x    光标X坐标 | Cursor X coordinate
 * @param   y    光标Y坐标 | Cursor Y coordinate
 */
/**************************************************************************/
void GFX_setCursor(Adafruit_GFX* gfx, int16_t x, int16_t y) {
    gfx->tx = x;
    gfx->ty = y;
    gfx->utf8_state = 0;  // 重置UTF-8解码器 | Reset UTF-8 decoder
}

/**************************************************************************/
/**
 * @brief   设置当前字体
 *          Set current font
 * @param   gfx   图形上下文指针 | Graphics context pointer
 * @param   font  U8G2字体数据指针 | U8G2 font data pointer
 */
/**************************************************************************/
void GFX_setFont(Adafruit_GFX* gfx, const uint8_t* font) { u8g2_SetFont(&gfx->u8g2, font); }

/**************************************************************************/
/**
 * @brief   设置字体渲染模式
 *          Set font rendering mode
 * @param   gfx            图形上下文指针 | Graphics context pointer
 * @param   is_transparent 1=透明背景，0=不透明背景 | 1=transparent background, 0=opaque background
 */
/**************************************************************************/
void GFX_setFontMode(Adafruit_GFX* gfx, uint8_t is_transparent) { u8g2_SetFontMode(&gfx->u8g2, is_transparent); }

/**************************************************************************/
/**
 * @brief   设置字体方向
 *          Set font direction
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @param   d    方向（0=水平，1/2/3=其他方向）| Direction (0=horizontal, 1/2/3=other directions)
 */
/**************************************************************************/
void GFX_setFontDirection(Adafruit_GFX* gfx, GFX_Rotate d) { u8g2_SetFontDirection(&gfx->u8g2, (uint8_t)d); }

/**************************************************************************/
/**
 * @brief   设置文本前景色和背景色
 *          Set text foreground and background colors
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @param   fg   前景色 | Foreground color
 * @param   bg   背景色 | Background color
 */
/**************************************************************************/
void GFX_setTextColor(Adafruit_GFX* gfx, uint16_t fg, uint16_t bg) {
    u8g2_SetForegroundColor(&gfx->u8g2, fg);
    u8g2_SetBackgroundColor(&gfx->u8g2, bg);
}

/**************************************************************************/
/**
 * @brief   获取字体上升高度（基线以上的高度）
 *          Get font ascent (height above baseline)
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @return  上升高度（像素）| Ascent height in pixels
 */
/**************************************************************************/
int8_t GFX_getFontAscent(Adafruit_GFX* gfx) { return gfx->u8g2.font_info.ascent_A; }

/**************************************************************************/
/**
 * @brief   获取字体下降高度（基线以下的高度，通常为负值）
 *          Get font descent (height below baseline, usually negative)
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @return  下降高度（像素）| Descent height in pixels
 */
/**************************************************************************/
int8_t GFX_getFontDescent(Adafruit_GFX* gfx) { return gfx->u8g2.font_info.descent_g; }

/**************************************************************************/
/**
 * @brief   获取字体总高度
 *          Get total font height
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @return  字体总高度（像素）= 上升高度 - 下降高度
 *          Total font height in pixels = ascent - descent
 */
/**************************************************************************/
int8_t GFX_getFontHeight(Adafruit_GFX* gfx) { return gfx->u8g2.font_info.ascent_A - gfx->u8g2.font_info.descent_g; }

/**************************************************************************/
/**
 * @brief   绘制单个字形
 *          Draw a single glyph
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @param   x    起始X坐标 | Starting X coordinate
 * @param   y    起始Y坐标（基线位置）| Starting Y coordinate (baseline position)
 * @param   e    Unicode编码值 | Unicode code point
 * @return  字形宽度（像素）| Glyph width in pixels
 */
/**************************************************************************/
int16_t GFX_drawGlyph(Adafruit_GFX* gfx, int16_t x, int16_t y, uint16_t e) {
    return u8g2_DrawGlyph(&gfx->u8g2, x, y, e);
}

/**************************************************************************/
/**
 * @brief   绘制ASCII字符串
 *          Draw an ASCII string
 * @param   gfx  图形上下文指针 | Graphics context pointer
 * @param   x    起始X坐标 | Starting X coordinate
 * @param   y    起始Y坐标（基线位置）| Starting Y coordinate (baseline position)
 * @param   s    ASCII字符串 | ASCII string
 * @return  字符串总宽度（像素）| Total string width in pixels
 */
/**************************************************************************/
int16_t GFX_drawStr(Adafruit_GFX* gfx, int16_t x, int16_t y, const char* s) {
    return u8g2_DrawStr(&gfx->u8g2, x, y, s);
}

static uint16_t utf8_next(Adafruit_GFX* gfx, uint8_t b) {
    if (b == 0)         /* '\n' terminates the string to support the string list procedures */
        return 0x0ffff; /* end of string detected, pending UTF8 is discarded */
    if (gfx->utf8_state == 0) {
        if (b >= 0xfc) /* 6 byte sequence */
        {
            gfx->utf8_state = 5;
            b &= 1;
        } else if (b >= 0xf8) {
            gfx->utf8_state = 4;
            b &= 3;
        } else if (b >= 0xf0) {
            gfx->utf8_state = 3;
            b &= 7;
        } else if (b >= 0xe0) {
            gfx->utf8_state = 2;
            b &= 15;
        } else if (b >= 0xc0) {
            gfx->utf8_state = 1;
            b &= 0x01f;
        } else {
            /* do nothing, just use the value as encoding */
            return b;
        }
        gfx->encoding = b;
        return 0x0fffe;
    } else {
        gfx->utf8_state--;
        /* The case b < 0x080 (an illegal UTF8 encoding) is not checked here. */
        gfx->encoding <<= 6;
        b &= 0x03f;
        gfx->encoding |= b;
        if (gfx->utf8_state != 0) return 0x0fffe; /* nothing to do yet */
    }
    return gfx->encoding;
}

int16_t GFX_drawUTF8(Adafruit_GFX* gfx, int16_t x, int16_t y, const char* str) {
    uint16_t e;
    int16_t delta, sum;

    gfx->utf8_state = 0;
    sum = 0;
    for (;;) {
        e = utf8_next(gfx, (uint8_t)*str);
        if (e == 0x0ffff) break;
        str++;
        if (e != 0x0fffe) {
            delta = u8g2_DrawGlyph(&gfx->u8g2, x, y, e);

            switch (gfx->u8g2.font_decode.dir) {
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
        }
    }
    return sum;
}

int16_t GFX_getUTF8Width(Adafruit_GFX* gfx, const char* str) {
    uint16_t e;
    int16_t dx, w;

    gfx->u8g2.font_decode.glyph_width = 0;
    gfx->utf8_state = 0;
    w = 0;
    dx = 0;
    for (;;) {
        e = utf8_next(gfx, (uint8_t)*str);
        if (e == 0x0ffff) break;
        str++;
        if (e != 0x0fffe) {
            dx = u8g2_GetGlyphWidth(&gfx->u8g2, e);
            w += dx;
        }
    }
    /* adjust the last glyph, check for issue #16: do not adjust if width is 0 */
    if (gfx->u8g2.font_decode.glyph_width != 0) {
        w -= dx;
        w += gfx->u8g2.font_decode.glyph_width; /* the real pixel width of the glyph, sideeffect of GetGlyphWidth */
        /* issue #46: we have to add the x offset also */
        w += gfx->u8g2.glyph_x_offset; /* this value is set as a side effect of u8g2_GetGlyphWidth() */
    }

    return w;
}

int16_t GFX_getUTF8Widthf(Adafruit_GFX* gfx, const char* format, ...) {
    char buf[64] = {0};
    char* str = buf;
    size_t len;
    va_list va;

    va_start(va, format);
    len = vsnprintf(buf, sizeof(buf), format, va);
    va_end(va);

    if (len > sizeof(buf) - 1) {
        str = malloc(len + 1);
        if (str == NULL) return 0;
        va_start(va, format);
        vsnprintf(str, len + 1, format, va);
        va_end(va);
    }

    int16_t w = GFX_getUTF8Width(gfx, str);

    if (str != buf) free(str);

    return w;
}

size_t GFX_print(Adafruit_GFX* gfx, const char c) {
    int16_t delta;
    uint16_t e = utf8_next(gfx, (uint8_t)c);
    if (e == '\n') {
        gfx->tx = 0;
        gfx->ty += gfx->u8g2.font_info.ascent_para - gfx->u8g2.font_info.descent_para;
    } else if (e == '\r') {
        gfx->tx = 0;
    } else if (e < 0x0fffe) {
        delta = u8g2_DrawGlyph(&gfx->u8g2, gfx->tx, gfx->ty, e);
        switch (gfx->u8g2.font_decode.dir) {
            case 0:
                gfx->tx += delta;
                break;
            case 1:
                gfx->ty += delta;
                break;
            case 2:
                gfx->tx -= delta;
                break;
            case 3:
                gfx->ty -= delta;
                break;
        }
    }
    return 1;
}

size_t GFX_write(Adafruit_GFX* gfx, const char* buffer, size_t size) {
    size_t cnt = 0;
    while (size > 0) {
        cnt += GFX_print(gfx, *buffer++);
        size--;
    }
    return cnt;
}

size_t GFX_printf(Adafruit_GFX* gfx, const char* format, ...) {
    va_list va;
    char tmp[64] = {0};
    char* buf = tmp;
    size_t len;

    va_start(va, format);
    len = vsnprintf(tmp, sizeof(tmp), format, va);
    va_end(va);

    if (len > sizeof(tmp) - 1) {
        buf = malloc(len + 1);
        if (buf == NULL) return 0;
        va_start(va, format);
        vsnprintf(buf, len + 1, format, va);
        va_end(va);
    }

    len = GFX_write(gfx, buf, len);
    if (buf != tmp) free(buf);

    return len;
}
