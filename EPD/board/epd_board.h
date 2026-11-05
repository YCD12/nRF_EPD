/**
 * @file epd_board.h
 * @brief 板级默认配置（board 层）
 *
 * 新板子只需改这里（引脚/默认型号/显示模式/全刷间隔），无需改 service 代码。
 * 持久化配置 epd_config_t 的字段顺序/大小受外置 Flash 记录与网页协议约束，
 * 修改必须同步 docs/ble-protocol.md 与网页迁移逻辑。
 */
#ifndef EPD_BOARD_H
#define EPD_BOARD_H

#include "epd_panel.h"

/* 默认 EPD 引脚映射：MOSI/SCLK/CS/DC/RST/BUSY/BS，唤醒 0xFF=未用，LED 0x12，EN 0x07。
 * 尾部 display_mode/week_start/full_refresh_minutes 由结构体零初始化（图片/周日/每小时）。 */
#define EPD_BOARD_DEFAULT_CONFIG                                         \
    {                                                                    \
        0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02,                        \
        EPD_UC8179_750_BWR, 0xFF, 0x12, 0x07,                            \
        0x00, 0x00, 0x00                                                 \
    }

#define EPD_BOARD_DEFAULT_MODEL EPD_UC8179_750_BWR

#endif /* EPD_BOARD_H */
