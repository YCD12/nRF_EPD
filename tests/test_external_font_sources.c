#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "fonts.h"
#include "font_geometry.h"
#include "resource_layout.h"
#include "resource_store.h"

#define TEST_FONT_DATA_BASE (RS_OFFSET_FONTS + RS_HEADER_SIZE + 56U)

static fm25q04b_region_t regions[FONT_STORE_MAX_SLOTS] = {
    {TEST_FONT_DATA_BASE, 4315UL},
    {TEST_FONT_DATA_BASE + 4315UL, 4830UL},
    {TEST_FONT_DATA_BASE + 4315UL + 4830UL, 389UL},
    {TEST_FONT_DATA_BASE + 4315UL + 4830UL + 389UL, 4830UL},
};
static u8g2_font_source_t sources[FONT_STORE_MAX_SLOTS] = {
    {NULL, 4315UL, fm25q04b_read_region, &regions[0], 1U},
    {NULL, 4830UL, fm25q04b_read_region, &regions[1], 1U},
    {NULL, 389UL, fm25q04b_read_region, &regions[2], 1U},
    {NULL, 4830UL, fm25q04b_read_region, &regions[3], 1U},
};

static bool ready;
static uint8_t todo_font_size;
static bool todo_font_extended;
ret_code_t font_store_init(void) { ready = true; return NRF_SUCCESS; }
bool font_store_ready(void) { return ready; }
const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot) {
    return ready && slot < FONT_STORE_MAX_SLOTS &&
        (slot != FONT_STORE_SLOT_TODO || todo_font_size != 0U) ? &sources[slot] : NULL;
}
const u8g2_font_placeholder_t *font_store_font_metrics(font_store_slot_t slot) {
    return slot<3?&font_slot_geometry[slot]:NULL;
}
uint8_t font_store_todo_font_size(void) { return todo_font_size; }
uint32_t font_store_todo_font_key(void) { return todo_font_size != 0U ? 0x12345678UL : 0U; }
const u8g2_font_placeholder_t *font_store_todo_font_metrics(void) {
    if (!ready || todo_font_size == 0U) return NULL;
    if (todo_font_extended) return todo_font_metrics_for_size(todo_font_size);
    return &font_slot_geometry[todo_font_size == 9U ? 0U : 1U];
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

    assert_source(GUI_FONT_WQY9, TEST_FONT_DATA_BASE, 4315UL);
    assert_source(GUI_FONT_WQY12, TEST_FONT_DATA_BASE + 4315UL, 4830UL);
    assert_source(GUI_FONT_HELVB18, TEST_FONT_DATA_BASE + 4315UL + 4830UL, 389UL);
    assert(GUI_GetExternalFont(GUI_FONT_TODO) == NULL);
    assert(GUI_GetFontMetrics(GUI_FONT_TODO) == NULL);
    assert(!GUI_TodoFontMatches(12U, 0x12345678UL));
    for (unsigned size = 9; size <= 12; size += 3) {
        todo_font_size = (uint8_t)size;
        assert_source(GUI_FONT_TODO, TEST_FONT_DATA_BASE + 4315UL + 4830UL + 389UL, 4830UL);
        assert(memcmp(GUI_GetFontMetrics(GUI_FONT_TODO), &font_slot_geometry[size == 9U ? 0U : 1U],
                      sizeof(gui_font_metrics_t)) == 0);
        assert(GUI_TodoFontMatches((uint8_t)size, 0x12345678UL));
        assert(!GUI_TodoFontMatches(size == 9U ? 12U : 9U, 0x12345678UL));
        assert(!GUI_TodoFontMatches((uint8_t)size, 0x12345679UL));
    }
    todo_font_extended = true;
    for (unsigned i = 0U; i < TODO_FONT_SIZE_COUNT; ++i) {
        todo_font_size = todo_font_sizes[i];
        assert(GUI_GetFontMetrics(GUI_FONT_TODO) == &todo_font_geometry[i]);
        assert(GUI_TodoFontMatches(todo_font_size, 0x12345678UL));
        assert(!GUI_TodoFontMatches(todo_font_size, 0x12345679UL));
    }
    assert(GUI_GetFontMetrics((gui_font_id_t)4) == NULL);

    assert(GUI_ExternalFontsInit() == 1U);
    puts("PASS: external font source mapping and legacy/extended TODO metrics");
    return 0;
}
