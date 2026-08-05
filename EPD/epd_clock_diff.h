#ifndef EPD_CLOCK_DIFF_H
#define EPD_CLOCK_DIFF_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"
#include "GUI.h"

void epd_clock_diff_seed(display_mode_t mode, uint32_t timestamp);
void epd_clock_diff_invalidate(void);
bool epd_clock_diff_try_partial(epd_model_t *epd, uint32_t timestamp,
                                display_mode_t mode);

#endif
