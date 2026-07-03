#ifndef _GUI_PORTING_HAL_H
#define _GUI_PORTING_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 看门狗喂狗接口 - Watchdog feeding interface
 *        解耦 GUI 核心与具体硬件平台的看门狗实现
 */
void gui_porting_feed_wdt(void);

#ifdef __cplusplus
}
#endif

#endif // _GUI_PORTING_HAL_H
