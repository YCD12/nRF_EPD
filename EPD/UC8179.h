#ifndef UC8179_H
#define UC8179_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"

bool UC8179_HadBusyTimeout(void);   /* 上次 WaitBusy 是否超时 */
void UC8179_ClearBusyTimeout(void); /* 开始新一轮刷新前清除 */

void UC8179_Init_Partial(epd_model_t *epd); /* 进寄存器 LUT / OLD-NEW 局刷 */
bool UC8179_Refresh_Partial(epd_model_t *epd, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h); /* PON→PTIN/PTL→DRF→PTOUT→POF */
/* 同一 PTL 窗口写入打包的 OLD 与 NEW 位图 */
bool UC8179_Partial_Diff_Push(epd_model_t *epd, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const uint8_t *old_bm, const uint8_t *new_bm);

#endif /* UC8179_H */
