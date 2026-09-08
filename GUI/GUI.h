#ifndef __GUI_H
#define __GUI_H

#include "Adafruit_GFX.h"

// 显示模式
typedef enum {
    MODE_PICTURE = 0,        // 图片
    MODE_CALENDAR = 1,       // 全屏日历
    MODE_CLOCK_CALENDAR = 2, // 分屏：左时钟 + 右日历，时钟走局刷
    MODE_CLOCK = 3,          // 全屏时钟：局刷流程与分屏相同，数字更大
} display_mode_t;

// 界面绘制所需参数
typedef struct {
    display_mode_t mode;  // 显示模式
    uint16_t color;       // 颜色模式（1:黑白, 2:三色, 3:四色）
    uint16_t width;       // 屏宽（像素）
    uint16_t height;      // 屏高（像素）
    uint32_t timestamp;   // UNIX 时间戳（秒）
    uint8_t week_start;   // 一周起始日（0:周日, 1:周一）
    int8_t temperature;   // 温度（℃）
    uint16_t voltage_mv;  // 电池电压（毫伏，例如 3300 表示 3.3V）
    char ssid[32];        // 蓝牙/WiFi 名称
} gui_data_t;

// 按显示模式绘制完整界面
// data：显示参数；callback / callback_data：分页缓冲回调
void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data);

// 仅绘制 HH:MM，供时钟局刷。画布尺寸由 clock_refresh_geom() 决定（分屏 TIME_REFRESH_* 或全屏数字框）
void DrawGUI_ClockOnly(gui_data_t* data, buffer_callback callback, void* callback_data);

// 屏幕坐标系下的 HH:MM（或分钟个位）窗口。data->mode/width/height 决定画布。
// prev_ts 必须是上一次成功的时钟刷新（全刷或局刷），不能用 GUI 本地副本。
void GetTimeRefreshArea(const gui_data_t *data, uint32_t prev_ts,
                        uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h);

/* 应用画布局部时钟窗口，并返回其屏幕矩形 */
void GetTimeRefreshAreaFromLocal(const gui_data_t *data,
                                 uint16_t lx, uint16_t ly, uint16_t lw, uint16_t lh,
                                 uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h);

#endif
