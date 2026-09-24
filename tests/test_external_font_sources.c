#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "fonts.h"

static fm25q04b_region_t regions[FONT_STORE_SLOT_COUNT] = {
    {FONT_STORE_DATA_BASE, 4315UL},
    {FONT_STORE_DATA_BASE + 4315UL, 4830UL},
    {FONT_STORE_DATA_BASE + 4315UL + 4830UL, 389UL}
};
static u8g2_font_source_t sources[FONT_STORE_SLOT_COUNT] = {
    {NULL, 4315UL, fm25q04b_read_region, &regions[0], 1U},
    {NULL, 4830UL, fm25q04b_read_region, &regions[1], 1U},
    {NULL, 389UL, fm25q04b_read_region, &regions[2], 1U},
};

ret_code_t font_store_init(void) { return NRF_SUCCESS; }
bool font_store_ready(void) { return true; }
const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot) {
    return slot < FONT_STORE_SLOT_COUNT ? &sources[slot] : NULL;
}
int fm25q04b_read_region(const void* context, uint32_t offset,
                         uint8_t* data, size_t length) {
    (void)context;
    (void)offset;
    (void)data;
    (void)length;
    return 0;
}
static void assert_source(gui_font_id_t id, uint32_t address, uint32_t length) {
    const u8g2_font_source_t* source = GUI_GetExternalFont(id);
    const fm25q04b_region_t* region;

    assert(source != NULL);
    assert(source->data == NULL);
    assert(source->length == length);
    assert(source->read == fm25q04b_read_region);

    region = (const fm25q04b_region_t*)source->context;
    assert(region != NULL);
    assert(region->base_address == address);
    assert(region->length == length);
}

int main(void) {
    assert(GUI_GetExternalFont(GUI_FONT_WQY9) == NULL);
    assert(GUI_ExternalFontsInit() == 1U);

    assert_source(GUI_FONT_WQY9, FONT_STORE_DATA_BASE, 4315UL);
    assert_source(GUI_FONT_WQY12, FONT_STORE_DATA_BASE + 4315UL, 4830UL);
    assert_source(GUI_FONT_HELVB18, FONT_STORE_DATA_BASE + 4315UL + 4830UL, 389UL);

    assert(GUI_ExternalFontsInit() == 1U);
    puts("PASS: external font source mapping");
    return 0;
}
