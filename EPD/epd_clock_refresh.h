#ifndef EPD_CLOCK_REFRESH_H
#define EPD_CLOCK_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"
#include "GUI.h"

void epd_clock_refresh_invalidate(void);              /* 丢弃旧时间基准，下次须全刷 */
void epd_clock_refresh_mark_valid(uint32_t timestamp); /* 全刷成功后记下当前时刻 */
bool epd_clock_refresh_is_valid(void);                 /* 是否已有可用的全刷基准 */
bool epd_clock_refresh_try(const epd_model_t *epd, const gui_data_t *data); /* 真 OLD/NEW 局刷，失败返回 false */

#endif
