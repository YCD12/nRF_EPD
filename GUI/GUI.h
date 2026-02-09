#ifndef __GUI_H
#define __GUI_H

#include "Adafruit_GFX.h"

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

// Draw only the time portion for partial refresh in MODE_CLOCK_CALENDAR
// This draws to a small area containing only the 7-segment time display
// 仅绘制时间部分用于MODE_CLOCK_CALENDAR模式的局部刷新
// 该函数只绘制包含7段数码管样式时间显示的小区域，大幅减少刷新面积，延长屏幕寿命
void DrawGUI_ClockOnly(gui_data_t* data, buffer_callback callback, void* callback_data);

// Get the time refresh area parameters for EPD_service
// timestamp is the current UNIX time used to decide whether we can
// shrink the window to only the minute-ones digit (e.g. 00->01, 01->02, ...).
// 获取时间刷新区域参数供EPD服务使用
// timestamp: 当前UNIX时间戳，用于判断是否可以将刷新窗口缩小为仅刷新分钟个位数字
// 智能优化：当仅分钟个位变化时(如00->01, 01->02, ..., 08->09)，只刷新最右侧的一个数字，
// 最大程度减少局部刷新面积，降低功耗和屏幕磨损
void GetTimeRefreshArea(uint32_t timestamp, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h);

// Get the base origin (top-left) of the time refresh area on the full screen
// 获取时间刷新区域在整个屏幕上的基准起点(左上角坐标)
void GetTimeRefreshOrigin(uint16_t* x, uint16_t* y);

#endif
