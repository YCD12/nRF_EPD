#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "fonts.h"

static unsigned init_calls;
static unsigned probe_calls;

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
bool font_store_get_info(font_store_slot_t slot, font_store_info_t* info) {
    (void)slot; (void)info; return false;
}
font_store_result_t font_store_begin_pack(uint32_t total_length, uint32_t pack_crc32,
                                          const font_store_pack_slot_t* slots) {
    (void)total_length; (void)pack_crc32; (void)slots; return FONT_STORE_INVALID;
}
font_store_result_t font_store_write_pack_chunk(uint32_t offset, const uint8_t* data,
                                                size_t length) {
    (void)offset; (void)data; (void)length; return FONT_STORE_INVALID;
}
font_store_result_t font_store_commit_pack(void) { return FONT_STORE_INVALID; }
font_store_result_t font_store_abort_update(void) { return FONT_STORE_INVALID; }
uint32_t font_store_crc32(const uint8_t* data, size_t length) { (void)data; (void)length; return 0U; }

ret_code_t fm25q04b_init(void) {
    init_calls++;
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t* id) {
    probe_calls++;
    if (id != NULL) {
        id->manufacturer_id = 0xEF;
        id->memory_type = 0x40;
        id->capacity = 0x18;
    }
    return NRF_SUCCESS;
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
