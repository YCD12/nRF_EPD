#ifndef UC8179_H
#define UC8179_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_driver.h"

bool UC8179_HadBusyTimeout(void);
void UC8179_ClearBusyTimeout(void);

void UC8179_Init_Partial(epd_model_t *epd);
bool UC8179_Refresh_Partial(epd_model_t *epd, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h);
bool UC8179_Partial_Diff_Push(epd_model_t *epd, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const uint8_t *old_bm, const uint8_t *new_bm);

#endif /* UC8179_H */
