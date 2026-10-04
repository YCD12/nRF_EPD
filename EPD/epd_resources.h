#ifndef EPD_RESOURCES_H
#define EPD_RESOURCES_H

#include "resource_store.h"

/* Display resource lifecycle, main context only. No BLE or clock ownership. */
void epd_resources_invalidate(void);
void epd_resources_invalidate_partitions(uint16_t changed);
rs_result_t epd_resources_apply(void);
rs_result_t epd_resources_reload(void);
bool epd_resources_ready(void);

#endif
