#ifndef FONT_STORE_H
#define FONT_STORE_H

#include <stdbool.h>
#include "sdk_errors.h"
#include "u8g2_font.h"

#define FONT_STORE_SLOT_COUNT 3U
#define FONT_STORE_MAX_SLOTS 4U /* Three required fonts and an optional TODO font. */

typedef enum {
    FONT_STORE_SLOT_WQY9 = 0,
    FONT_STORE_SLOT_WQY12,
    FONT_STORE_SLOT_HELVB18,
    FONT_STORE_SLOT_TODO,
} font_store_slot_t;

ret_code_t font_store_init(void);
/* Explicit activation retry; successful complete font packs stay cached. */
ret_code_t font_store_retry_init(void);
/* Main only, while rendering is suspended. Discards physical font addresses. */
void font_store_invalidate(void);
bool font_store_ready(void);

const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot);
uint8_t font_store_todo_font_size(void);
uint32_t font_store_todo_font_key(void);
const u8g2_font_placeholder_t *font_store_todo_font_metrics(void);
const u8g2_font_placeholder_t *font_store_font_metrics(font_store_slot_t slot);

#if defined(FONT_STORE_HOST_TEST)
/* Test-only runtime reset; Flash contents are deliberately preserved. */
void font_store_host_reset(void);
#endif

#endif
