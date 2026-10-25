#include "resource_layout.h"
#include "resource_store.h"
#include "stubs/ui_default_payload.h"
#include "ui_resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t payload[4096];
static uint32_t payload_length;
static bool mount_ok = true;
static void put16(uint8_t *p, uint16_t n) {
    p[0] = n;
    p[1] = n >> 8;
}
rs_result_t resource_store_mount(void) { return mount_ok ? RS_OK : RS_NOT_FOUND; }
rs_result_t resource_store_open_info(uint16_t id, rs_package_info_t *info) {
    if (id != 6)
        return RS_NOT_FOUND;
    info->length = payload_length;
    info->revision = 7;
    return RS_OK;
}
rs_result_t resource_store_read(uint16_t id, uint32_t at, void *out, size_t n) {
    if (id != 6 || at > payload_length || n > payload_length - at)
        return RS_INVALID;
    memcpy(out, payload + at, n);
    return RS_OK;
}
uint32_t resource_get_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void reset(void) {
    memcpy(payload, ui_default_payload, sizeof(ui_default_payload));
    payload_length = sizeof(ui_default_payload);
    ui_resources_host_reset();
}
static void test_conditional_clocks_require_full_refresh(void) {
    const uint8_t flags[] = {UI_FLAG_HIDDEN, UI_FLAG_SYNC_ONLY,
                            UI_FLAG_JIEQI_AHEAD, UI_FLAG_JIEQI_TODAY,
                            UI_FLAG_SYNC_ONLY | UI_FLAG_JIEQI_AHEAD};
    for (unsigned full = 0; full < 2; full++) {
        for (unsigned f = 0; f < sizeof(flags); f++) {
            reset();
            for (unsigned i = 0; i < payload[6]; i++) {
                uint8_t *record = payload + 16U + 32U * i;
                if (record[1] == (full ? UI_COMP_CLOCK_TIME : UI_COMP_SPLIT_TIME))
                    record[3] = flags[f];
            }
            assert(ui_resources_init());
            clock_refresh_geom_t geom;
            assert(ui_resources_clock_geom(full, 800, 480, &geom));
            /* A hidden old face cannot be regenerated from its timestamp. */
            assert(!geom.partial_allowed);
        }
    }
    reset();
    clock_refresh_geom_t geom;
    assert(ui_resources_clock_geom(true, 800, 480, &geom) && geom.partial_allowed);
    assert(ui_resources_clock_geom(false, 800, 480, &geom) && geom.partial_allowed);
}
int main(void) {
    test_conditional_clocks_require_full_refresh();
    _Static_assert(sizeof(ui_component_t) == 32, "UIR2 wire layout");
    reset();
    assert(ui_resources_init());
    assert(ui_resources_count() == 46);
    assert(ui_resources_revision() == 7);
    assert(ui_resource_component(UI_COMP_TODO_SPLIT)->h == 384);
    assert(ui_resources_todo_capacity(58, true) == 6);
    const ui_component_t *temp = ui_resource_at(22);
    assert(temp->x + temp->w == 775);
    char text[256];
    assert(ui_resource_text(temp, 0, text, sizeof(text)));
    assert(text[0] == 1 && text[1] == UI_TOKEN_TEMPERATURE);
    clock_refresh_geom_t g;
    assert(ui_resources_clock_geom(false, 800, 480, &g));
    assert(g.resource_geometry && g.face_x == 56 && g.face_y == 13);
    assert(ui_resources_clock_geom(true, 800, 480, &g));
    assert(g.face_x == 6 && g.face_y == 2);
    reset();
    payload[16+19*32+3]=1;payload[16+20*32+3]=1;
    assert(ui_resources_init()&&ui_resources_todo_capacity(58,true)==8);
    reset();
    payload[3] = '1';
    assert(!ui_resources_init());
    reset();
    put16(payload + 16 + 8, 799);
    assert(!ui_resources_init());
    reset();
    payload[16 + 20] = 3;
    assert(!ui_resources_init());
    reset();
    payload[16 + 1] = payload[48 + 1];
    assert(!ui_resources_init());
    reset();
    put16(payload + 16 + 24, 0);
    assert(!ui_resources_init());
    reset();
    payload[16 + 26]++;
    assert(!ui_resources_init());
    reset();
    payload[16 + 27]++;
    assert(!ui_resources_init());
    reset();
    payload_length--;
    assert(!ui_resources_init());
    reset();
    payload[16 + 7] = 3;
    assert(!ui_resources_init());
    reset();
    put16(payload + 16 + 11 * 32 + 8, 1);
    assert(!ui_resources_init());
    reset();
    mount_ok = false;
    assert(!ui_resources_init());
    mount_ok = true;
    reset();
    assert(ui_resources_init());
    puts("UIR2: source fixture, templates, bounds, clock geometry, capacity and malformed packages "
         "OK");
}
