#ifndef __GUI_H
#define __GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "Adafruit_GFX.h"
#include "sdk_errors.h"

#define GUI_UPDATE_MANUAL 0U
#define GUI_UPDATE_DAILY  1U
#define GUI_UPDATE_MINUTE 2U

// 显示模式枚举类型
// Display mode enumeration type
typedef enum {
    MODE_PICTURE = 0,       // 图片模式 | Picture mode
    MODE_CALENDAR = 1,      // 日历模式(全屏日历) | Calendar mode (full screen calendar)
    MODE_CLOCK_CALENDAR = 2,  // Split screen: clock (left) + calendar (right), clock uses partial refresh
                              // 分屏模式：时钟(左侧) + 日历(右侧)，时钟使用局部刷新以节省电量
    MODE_CLOCK = 3,           // Full screen clock only
                              // 时钟模式(全屏时钟) | Clock mode (full screen clock)
} display_mode_t;

// GUI数据结构体，包含所有界面显示所需的参数
// GUI data structure containing all parameters needed for interface display
typedef struct {
    display_mode_t mode;  // 显示模式 | Display mode
    uint16_t color;       // 颜色模式(1:黑白, 2:三色, 3:四色) | Color mode (1:BW, 2:3-color, 3:4-color)
    uint16_t width;       // 屏幕宽度(像素) | Screen width (pixels)
    uint16_t height;      // 屏幕高度(像素) | Screen height (pixels)
    uint32_t timestamp;   // UNIX时间戳(秒) | UNIX timestamp (seconds)
    uint8_t week_start;   // 0: Sunday, 1: Monday
                          // 一周起始日(0:周日, 1:周一) | Week start day
    int8_t temperature;   // 温度(摄氏度) | Temperature (Celsius)
    uint16_t voltage_mv;  // Voltage in millivolts (e.g., 3300 for 3.3V)
                          // 电池电压(毫伏，例如3300表示3.3V) | Battery voltage in millivolts
    char ssid[32];        // 蓝牙/WiFi名称 | Bluetooth/WiFi name
} gui_data_t;

// 绘制GUI主函数 - 根据显示模式绘制完整界面
// Draw GUI main function - draws the complete interface based on display mode
// @param data: GUI数据指针，包含所有显示参数 | Pointer to GUI data with all display parameters
// @param callback: 缓冲区回调函数，用于传输数据到屏幕 | Buffer callback function for transferring data to screen
// @param callback_data: 回调函数的用户数据 | User data for callback function
void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data);



// Get the time refresh area parameters for EPD_service
// timestamp is the current UNIX time used to shrink the partial window to
// the digit(s) that changed since the last displayed time.
// 获取时间刷新区域参数供EPD服务使用
// timestamp: 当前UNIX时间戳，用于将刷新窗口缩小到相对上次显示发生变化的数字
void GetTimeRefreshArea(uint32_t timestamp, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h);

// Mark the current time as already fully drawn. Call after a full refresh so
// the next partial refresh compares against the real visible time.
// 标记当前时间已经通过全刷完整显示，避免下一次局刷误判为大范围变化。
void GUI_MarkTimeRefreshTimestamp(uint32_t timestamp);

// Get the base origin (top-left) of the time refresh area on the full screen
// 获取时间刷新区域在整个屏幕上的基准起点(左上角坐标)
void GetTimeRefreshOrigin(uint16_t* x, uint16_t* y);

bool GUI_ModeExists(uint8_t mode, uint16_t width, uint16_t height);
uint8_t GUI_GetModeUpdatePolicy(uint8_t mode, uint16_t width, uint16_t height);
ret_code_t GUI_ReloadUICache(uint16_t width, uint16_t height);

void DrawResourceMissingScreen(gui_data_t* data, buffer_callback callback, void* callback_data);
void Draw7Number(Adafruit_GFX *gfx, int n, unsigned int xLoc, unsigned int yLoc, char cS, unsigned int fC, unsigned int bC, int nD);
#endif
