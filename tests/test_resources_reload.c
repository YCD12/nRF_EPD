#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "epd_resources.h"
#include "font_store.h"
#include "fm25q04b.h"
#include "calendar_resources.h"
#include "ui_resources.h"

static uint8_t flash[FM25Q04B_SIZE_BYTES], fonts[262144];
static bool flash_initialized;
static unsigned font_reads;
void app_feed_wdt(void) {}
ret_code_t fm25q04b_init(void) { flash_initialized = true; return NRF_SUCCESS; }
ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t *id) { memset(id, 0, sizeof(*id)); return NRF_SUCCESS; }
ret_code_t fm25q04b_read(uint32_t at, void *p, size_t n)
{
    if (!flash_initialized) return NRF_ERROR_INVALID_STATE;
    assert(at <= sizeof(flash) && n <= sizeof(flash) - at);
    if (at >= 0x18000 && at < 0x58000) ++font_reads;
    memcpy(p, flash + at, n); return NRF_SUCCESS;
}
ret_code_t fm25q04b_write(uint32_t at, const void *p, size_t n)
{
    assert(at <= sizeof(flash) && n <= sizeof(flash) - at);
    for (size_t i = 0; i < n; ++i) flash[at + i] &= ((const uint8_t *)p)[i];
    return NRF_SUCCESS;
}
ret_code_t fm25q04b_erase_sector(uint32_t at)
{
    assert(at % 4096 == 0 && at < sizeof(flash));
    memset(flash + at, 255, 4096); return NRF_SUCCESS;
}
int fm25q04b_read_region(const void *ctx, uint32_t at, uint8_t *p, size_t n)
{
    const fm25q04b_region_t *r = ctx;
    assert(at <= r->length && n <= r->length - at);
    return fm25q04b_read(r->base_address + at, p, n);
}
static void replace(unsigned id, const uint8_t *p, uint32_t n)
{
    epd_resources_invalidate_partitions(1U << id);
    assert(!epd_resources_ready());
    assert((font_store_get_source(0) != NULL) == (id != 7));
    assert(resource_store_begin(id, n, resource_crc32(UINT32_MAX, p, n) ^ UINT32_MAX, 2) == RS_OK);
    assert(resource_store_write(0, p, n) == RS_OK);
    assert(resource_store_commit() == RS_OK);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(fread(flash, 1, sizeof(flash), f) == sizeof(flash)); fclose(f);
    /* The boot mount used to run before ble_epd_init() initialized SPI. */
    assert(epd_resources_reload() == RS_IO && !epd_resources_ready());
    assert(fm25q04b_init() == NRF_SUCCESS);
    assert(epd_resources_reload() == RS_OK && epd_resources_ready());
    uint8_t ui[396];
    assert(resource_store_read(6, 0, ui, sizeof(ui)) == RS_OK);
    ui[16 + 8] = 12; /* Move calendar header from x=10 to x=12. */
    replace(6, ui, sizeof(ui));
    unsigned before = font_reads;
    assert(epd_resources_apply() == RS_OK && font_reads == before);
    assert(ui_resource_component(1)->x == 12 && ui_resources_revision() == 2);
    ui[16 + 8] = 255; ui[16 + 9] = 127; /* Valid CRC, invalid geometry. */
    replace(6, ui, sizeof(ui));
    assert(epd_resources_apply() == RS_INVALID && !epd_resources_ready() && font_reads == before);
    ui[16 + 8] = 10; ui[16 + 9] = 0;
    replace(6, ui, sizeof(ui));
    assert(epd_resources_apply() == RS_OK && font_reads == before);

    uint32_t n;
    assert(resource_store_open(7, &n) == RS_OK && n + 16 < sizeof(fonts));
    assert(resource_store_read(7, 0, fonts, n) == RS_OK);
    /* Relocate all three fonts inside FPK1. No cached physical address may survive. */
    memmove(fonts + 72, fonts + 56, n - 56);
    memset(fonts + 56, 0, 16);
    for (unsigned i = 0; i < 3; ++i)
        resource_put_u32(fonts + 8 + i * 16, resource_get_u32(fonts + 8 + i * 16) + 16);
    replace(7, fonts, n + 16);
    before = font_reads;
    assert(epd_resources_apply() == RS_OK && font_reads > before);
    for (unsigned i = 0; i < 3; ++i) {
        const u8g2_font_source_t *source = font_store_get_source(i);
        uint8_t header[23];
        assert(source && source->read(source->context, 0, header, sizeof(header)) == 0);
        assert(!memcmp(header, fonts + resource_get_u32(fonts + 8 + i * 16), sizeof(header)));
    }
    epd_resources_invalidate_partitions(1U << 1);
    assert(!font_store_get_source(0) && !epd_resources_ready());
    before = font_reads;
    assert(epd_resources_apply() == RS_OK && font_reads > before);
    puts("PASS: selective UI reload, semantic rejection, repair, table invalidation and relocated font sources");
}
