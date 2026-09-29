#ifndef FONT_STORE_H
#define FONT_STORE_H

#include <stdbool.h>
#include "sdk_errors.h"
#include "u8g2_font.h"

#define FONT_STORE_SLOT_COUNT 3U

typedef enum {
    FONT_STORE_SLOT_WQY9 = 0,
    FONT_STORE_SLOT_WQY12,
    FONT_STORE_SLOT_HELVB18,
} font_store_slot_t;

ret_code_t font_store_init(void);
/* Main only, while rendering is suspended. Discards physical font addresses. */
void font_store_invalidate(void);
bool font_store_ready(void);

const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot);

#if defined(FONT_STORE_HOST_TEST)
/* Test-only runtime reset; Flash contents are deliberately preserved. */
void font_store_host_reset(void);
#endif

#endif
