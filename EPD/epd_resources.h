#ifndef EPD_RESOURCES_H
#define EPD_RESOURCES_H

#include "resource_store.h"

/* Display resource lifecycle, main context only. No BLE or clock ownership. */
void epd_resources_invalidate(void);
rs_result_t epd_resources_reload(void);
bool epd_resources_ready(void);

#endif
