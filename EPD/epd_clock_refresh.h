#ifndef EPD_CLOCK_REFRESH_H
#define EPD_CLOCK_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"
#include "GUI.h"

void epd_clock_refresh_invalidate(void);
void epd_clock_refresh_mark_valid(uint32_t timestamp);
bool epd_clock_refresh_is_valid(void);
bool epd_clock_refresh_try(epd_model_t *epd, gui_data_t *data);

#endif
