#ifndef _ADAFRUIT_GFX_H
#define _ADAFRUIT_GFX_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "u8g2_font.h"

// 颜色定义 - RGB565 格式 (16位：5位红色，6位绿色，5位蓝色)
// Color definitions - RGB565 format (16-bit: 5-bit red, 6-bit green, 5-bit blue)
#define GFX_BLACK 0x0000   // 黑色 | Black
#define GFX_WHITE 0xFFFF   // 白色 | White
#define GFX_RED 0xF800     // 红色 | Red (255,   0,   0)
#define GFX_YELLOW 0xFFE0  // 黄色 | Yellow (255, 255,   0)
#define GFX_BLUE 0x001F    // 蓝色 | Blue (0,   0, 255)
#define GFX_GREEN 0x07E0   // 绿色 | Green (0, 255,   0)
#define GFX_ORANGE 0xFC00  // 橙色 | Orange (255, 128,   0)

// 缓冲区回调函数类型定义
// Buffer callback function type definition
// 用于在页面渲染完成后将缓冲区数据传递给硬件层
// Used to pass buffer data to hardware layer after page rendering is complete
// @param user_data 用户自定义数据指针 | User-defined data pointer
// @param black 黑色像素缓冲区 | Black pixel buffer
// @param color 彩色像素缓冲区（仅用于3色/4色显示器）| Color pixel buffer (for 3-color/4-color displays only)
// @param x, y 缓冲区在屏幕上的起始坐标 | Starting coordinates of buffer on screen
// @param w, h 缓冲区的宽度和高度 | Width and height of buffer
typedef void (*buffer_callback)(void* user_data, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                                uint16_t h);

// 屏幕旋转枚举
// Screen rotation enumeration
typedef enum {
    GFX_ROTATE_0 = 0,    // 0度（默认方向）| 0 degrees (default orientation)
    GFX_ROTATE_90 = 1,   // 顺时针旋转90度 | 90 degrees clockwise
    GFX_ROTATE_180 = 2,  // 旋转180度 | 180 degrees
    GFX_ROTATE_270 = 3,  // 顺时针旋转270度（逆时针90度）| 270 degrees clockwise (90 degrees counter-clockwise)
} GFX_Rotate;

// GRAPHICS CONTEXT
// 图形上下文结构体 - 包含所有绘图所需的状态信息
// Graphics context structure - contains all state information needed for drawing
typedef struct {
    int16_t WIDTH;        // This is the 'raw' display width - never changes
                          // 原始显示宽度 - 永不改变
    int16_t HEIGHT;       // This is the 'raw' display height - never changes
                          // 原始显示高度 - 永不改变
    int16_t _width;       // Display width as modified by current rotation
                          // 根据当前旋转角度调整后的显示宽度
    int16_t _height;      // Display height as modified by current rotation
                          // 根据当前旋转角度调整后的显示高度
    GFX_Rotate rotation;  // Display rotation (0 thru 3)
                          // 显示旋转角度（0到3）

    // U8G2字体渲染引擎 | U8G2 font rendering engine
    u8g2_font_t u8g2;
    int16_t tx, ty;     // current position for the print command
                        // print命令的当前位置（文本光标位置）
    uint16_t encoding;  // the unicode, detected by the utf-8 decoder
                        // UTF-8解码器检测到的Unicode编码值
    uint8_t
        utf8_state;  // current state of the utf-8 decoder, contains the remaining bytes for a detected unicode glyph
                     // UTF-8解码器的当前状态，包含已检测到的Unicode字符的剩余字节数

    // 分页缓冲区管理 | Paged buffer management
    uint8_t* buffer;          // black pixel buffer 黑色像素缓冲区
    uint8_t* color;           // color pixel buffer (3c only) 彩色像素缓冲区（仅用于3色显示器）
    uint16_t px, py, pw, ph;  // partial window offset and size 局部窗口偏移和大小
    int16_t page_height;      // height to be drawn in one page 单页绘制的高度
    int16_t current_page;     // index of the current drawing page 当前绘制页的索引
    int16_t total_pages;      // total number of pages to be drawn 需要绘制的总页数
} Adafruit_GFX;

// =============================================================================
// CONTROL API - 控制接口
// =============================================================================

// 初始化单色图形上下文 | Initialize monochrome graphics context
void GFX_begin(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height);
// 初始化三色图形上下文（黑/白/红或黄）| Initialize 3-color graphics context (black/white/red or yellow)
void GFX_begin_3c(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height);
// 初始化四色图形上下文 | Initialize 4-color graphics context
void GFX_begin_4c(Adafruit_GFX* gfx, int16_t w, int16_t h, int16_t buffer_height);
// 设置屏幕旋转角度 | Set screen rotation
void GFX_setRotation(Adafruit_GFX* gfx, GFX_Rotate r);
// 设置局部刷新窗口 | Set partial refresh window
void GFX_setWindow(Adafruit_GFX* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
// 开始第一页的绘制 | Start drawing the first page
void GFX_firstPage(Adafruit_GFX* gfx);
// 处理下一页的绘制，返回true表示还有更多页需要绘制 | Process next page, returns true if more pages to draw
bool GFX_nextPage(Adafruit_GFX* gfx, buffer_callback callback, void* user_data);
// 结束绘图并释放资源 | End drawing and release resources
void GFX_end(Adafruit_GFX* gfx);

// =============================================================================
// DRAW API - 绘图接口
// =============================================================================

// 基本图形绘制函数 | Basic drawing functions

// 绘制单个像素点 | Draw a single pixel
void GFX_drawPixel(Adafruit_GFX* gfx, int16_t x, int16_t y, uint16_t color);
// 绘制任意斜率的直线（使用Bresenham算法）| Draw a line with any slope (using Bresenham's algorithm)
void GFX_drawLine(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
// 绘制虚线 | Draw a dotted line
void GFX_drawDottedLine(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color,
                        uint8_t dot_len, uint8_t space_len);
// 绘制垂直快速线（优化版）| Draw a fast vertical line (optimized)
void GFX_drawFastVLine(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t h, uint16_t color);
// 绘制水平快速线（优化版）| Draw a fast horizontal line (optimized)
void GFX_drawFastHLine(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, uint16_t color);

// 矩形绘制函数 | Rectangle drawing functions

// 绘制填充矩形 | Draw a filled rectangle
void GFX_fillRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
// 填充整个屏幕 | Fill the entire screen
void GFX_fillScreen(Adafruit_GFX* gfx, uint16_t color);
// 绘制空心矩形（边框）| Draw a hollow rectangle (outline only)
void GFX_drawRect(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// 圆形绘制函数 | Circle drawing functions

// 绘制空心圆 | Draw a circle outline
void GFX_drawCircle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint16_t color);
// 绘制圆的指定象限（用于圆角矩形）| Draw specified quadrants of a circle (for rounded rectangles)
void GFX_drawCircleHelper(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint8_t cornername, uint16_t color);
// 绘制填充圆 | Draw a filled circle
void GFX_fillCircle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint16_t color);
// 绘制填充圆的指定象限（用于圆角矩形填充）| Draw filled quadrants of a circle (for filled rounded rectangles)
void GFX_fillCircleHelper(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t r, uint8_t cornername, int16_t delta,
                          uint16_t color);

// 三角形绘制函数 | Triangle drawing functions

// 绘制空心三角形 | Draw a triangle outline
void GFX_drawTriangle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color);
// 绘制填充三角形 | Draw a filled triangle
void GFX_fillTriangle(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color);

// 圆角矩形绘制函数 | Rounded rectangle drawing functions

// 绘制空心圆角矩形 | Draw a rounded rectangle outline
void GFX_drawRoundRect(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color);
// 绘制填充圆角矩形 | Draw a filled rounded rectangle
void GFX_fillRoundRect(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color);

// 位图绘制函数 | Bitmap drawing function

// 绘制单色位图 | Draw a monochrome bitmap
void GFX_drawBitmap(Adafruit_GFX* gfx, int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h,
                    uint16_t color, bool invert);

// =============================================================================
// U8G2 FONT API - U8G2字体渲染接口
// =============================================================================

// 设置文本光标位置 | Set text cursor position
void GFX_setCursor(Adafruit_GFX* gfx, int16_t x, int16_t y);
// 设置当前字体 | Set current font
void GFX_setFont(Adafruit_GFX* gfx, const uint8_t* font);
// 设置字体模式（透明/不透明）| Set font mode (transparent/opaque)
void GFX_setFontMode(Adafruit_GFX* gfx, uint8_t is_transparent);
// 设置字体方向 | Set font direction
void GFX_setFontDirection(Adafruit_GFX* gfx, GFX_Rotate d);
// 设置文本颜色（前景色和背景色）| Set text colors (foreground and background)
void GFX_setTextColor(Adafruit_GFX* gfx, uint16_t fg, uint16_t bg);
// 获取字体上升高度（基线以上）| Get font ascent (above baseline)
int8_t GFX_getFontAscent(Adafruit_GFX* gfx);
// 获取字体下降高度（基线以下）| Get font descent (below baseline)
int8_t GFX_getFontDescent(Adafruit_GFX* gfx);
// 获取字体总高度 | Get total font height
int8_t GFX_getFontHeight(Adafruit_GFX* gfx);
// 绘制单个字形 | Draw a single glyph
int16_t GFX_drawGlyph(Adafruit_GFX* gfx, int16_t x, int16_t y, uint16_t e);
// 绘制ASCII字符串 | Draw an ASCII string
int16_t GFX_drawStr(Adafruit_GFX* gfx, int16_t x, int16_t y, const char* s);
// 绘制UTF-8编码字符串（支持中文等多字节字符）| Draw UTF-8 encoded string (supports Chinese and other multi-byte characters)
int16_t GFX_drawUTF8(Adafruit_GFX* gfx, int16_t x, int16_t y, const char* str);
// 计算UTF-8字符串的像素宽度 | Calculate pixel width of UTF-8 string
int16_t GFX_getUTF8Width(Adafruit_GFX* gfx, const char* str);
// 计算格式化UTF-8字符串的像素宽度 | Calculate pixel width of formatted UTF-8 string
int16_t GFX_getUTF8Widthf(Adafruit_GFX* gfx, const char* format, ...);
// 打印单个字符（支持UTF-8）| Print a single character (UTF-8 supported)
size_t GFX_print(Adafruit_GFX* gfx, const char c);
// 写入字符缓冲区 | Write character buffer
size_t GFX_write(Adafruit_GFX* gfx, const char* buffer, size_t size);
// 格式化打印（类似printf）| Formatted print (similar to printf)
size_t GFX_printf(Adafruit_GFX* gfx, const char* format, ...);

#endif  // _ADAFRUIT_GFX_H
