#ifndef EPD_CLOCK_REFRESH_H
#define EPD_CLOCK_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"
#include "GUI.h"

void epd_clock_refresh_invalidate(void);              /* 丢弃旧时间基准，下次须全刷 */
void epd_clock_refresh_mark_valid(uint32_t timestamp); /* 全刷成功后记下当前时刻 */
bool epd_clock_refresh_is_valid(void);                 /* 是否已有可用的全刷基准 */
/* 真 OLD/NEW 局刷，失败返回 false。
 * prepare_only=true 时只跑与真正局刷相同的资格检查（旧帧基准、UI 几何、
 * 条件时钟/覆盖层限制），不绘制、不触碰面板；服务层在 init_partial() 之前
 * 用它选择刷新类型，避免条件时钟布局每周期多做一次无用的面板局部初始化。 */
bool epd_clock_refresh_try(const epd_model_t *epd, const gui_data_t *data,
                           bool prepare_only);

#endif
