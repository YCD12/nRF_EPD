#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "resource_store.h"
#include "font_geometry.h"

#define MOCK_FLASH_SIZE FM25Q04B_SIZE_BYTES

static uint8_t mock_flash[MOCK_FLASH_SIZE];
static bool use_resource_layout;
static uint32_t resource_payload_length;
static int read_fail = -1;
static unsigned watchdog_feeds;

rs_result_t resource_store_mount(void) { return use_resource_layout ? RS_OK : RS_NOT_FOUND; }
rs_result_t resource_store_find(uint16_t id, rs_partition_t *p)
{
    if (!use_resource_layout || id != 7U || p == NULL) return RS_NOT_FOUND;
    *p = (rs_partition_t){7U, 7U, 1U, 0U, 0x18000U, 0x40000U};
    return RS_OK;
}
rs_result_t resource_store_open_info(uint16_t id, rs_package_info_t *p)
{
    if (!use_resource_layout || id != 7U || p == NULL) return RS_NOT_FOUND;
    *p = (rs_package_info_t){42U, resource_payload_length, 0U};
    return RS_OK;
}
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    if (!use_resource_layout || id != 7U || data == NULL ||
        offset > resource_payload_length || length > resource_payload_length - offset)
        return RS_NOT_FOUND;
    memcpy(data, &mock_flash[0x18000U + RS_HEADER_SIZE + offset], length);
    return RS_OK;
}

void app_feed_wdt(void)
{
    ++watchdog_feeds;
}

ret_code_t fm25q04b_init(void)
{
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t* id)
{
    if (id != NULL) {
        id->manufacturer_id = 0xEFU;
        id->memory_type = 0x40U;
        id->capacity = 0x18U;
    }
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_read(uint32_t address, void* data, size_t length)
{
    if (data == NULL && length != 0U) {
        return NRF_ERROR_NULL;
    }
    if (address > MOCK_FLASH_SIZE || length > MOCK_FLASH_SIZE - address) {
        return NRF_ERROR_INVALID_ADDR;
    }
    if (read_fail == 0) return NRF_ERROR_INTERNAL;
    if (read_fail > 0) --read_fail;
    memcpy(data, &mock_flash[address], length);
    return NRF_SUCCESS;
}

int fm25q04b_read_region(const void* context, uint32_t offset,
                         uint8_t* data, size_t length)
{
    const fm25q04b_region_t* region = (const fm25q04b_region_t*)context;

    if (region == NULL || (data == NULL && length != 0U) ||
        offset > region->length || length > region->length - offset) {
        return (int)NRF_ERROR_INVALID_ADDR;
    }
    return (int)fm25q04b_read(region->base_address + offset, data, length);
}

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_u32_le(uint8_t* p, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; i++) p[i] = (uint8_t)(value >> (i * 8U));
}

static void make_font(uint8_t* data, size_t length, font_store_slot_t slot)
{
    static const uint8_t widths[FONT_STORE_SLOT_COUNT] = {12U, 16U, 13U};
    static const uint8_t heights[FONT_STORE_SLOT_COUNT] = {12U, 16U, 22U};
    static const int8_t y_offsets[FONT_STORE_SLOT_COUNT] = {-2, -2, -3};
    static const int8_t ascents[FONT_STORE_SLOT_COUNT] = {8, 11, 19};
    static const int8_t descents[FONT_STORE_SLOT_COUNT] = {-2, -3, -5};
    static const int8_t ascent_para[FONT_STORE_SLOT_COUNT] = {10, 12, 19};
    static const int8_t descent_para[FONT_STORE_SLOT_COUNT] = {-1, -4, -5};

    assert(length >= 64U);
    memset(data, 0, length);
    data[0] = 1U;
    data[2] = 1U;
    data[3] = 1U;
    data[4] = 1U;
    data[5] = 1U;
    data[6] = 1U;
    data[7] = 1U;
    data[8] = 1U;
    data[9] = widths[slot];
    data[10] = heights[slot];
    data[12] = (uint8_t)y_offsets[slot];
    data[13] = (uint8_t)ascents[slot];
    data[14] = (uint8_t)descents[slot];
    data[15] = (uint8_t)ascent_para[slot];
    data[16] = (uint8_t)descent_para[slot];
    write_u16_be(&data[17], 0U);
    write_u16_be(&data[19], 0U);
    write_u16_be(&data[21], 0U);
    data[32] = (uint8_t)(0x30U + (uint8_t)slot);
}


static void assert_source(font_store_slot_t slot, uint32_t address, uint32_t length)
{
    const u8g2_font_source_t* source = font_store_get_source(slot);
    assert(source != NULL && source->length == length);
    const fm25q04b_region_t* region = source->context;
    assert(region->base_address == address);
    uint8_t value;
    assert(source->read(source->context, 32U, &value, 1U) == NRF_SUCCESS);
    assert(value == (uint8_t)(0x30U + slot));
}

static uint32_t font_crc32(const uint8_t* data, size_t length)
{
    return resource_crc32(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
}

static uint8_t* install_pack(const uint8_t* font, size_t length)
{
    uint8_t* payload = &mock_flash[0x18000U + RS_HEADER_SIZE];
    resource_payload_length = 56U + (uint32_t)length;
    memset(payload, 0, 56U);
    write_u32_le(payload, 0x314B5046UL);
    payload[4] = 1U;
    payload[6] = FONT_STORE_SLOT_COUNT;
    write_u32_le(payload + 8U, 56U);
    write_u32_le(payload + 12U, (uint32_t)length);
    write_u32_le(payload + 16U, font_crc32(font, length));
    memcpy(payload + 56U, font, length);
    return payload;
}

static uint8_t* install_todo_pack(const uint8_t* font, size_t length)
{
    uint8_t* payload = &mock_flash[0x18000U + RS_HEADER_SIZE];
    memset(payload, 0, 72U);
    write_u32_le(payload, 0x314B5046UL); payload[4] = 1U; payload[6] = 4U;
    write_u32_le(payload + 56U, 72U);
    write_u32_le(payload + 60U, (uint32_t)length);
    write_u32_le(payload + 64U, font_crc32(font, length));
    write_u32_le(payload + 68U, 0x12345678UL);
    memcpy(payload + 72U, font, length);
    resource_payload_length = 72U + (uint32_t)length;
    return payload;
}

static void assert_font_structure(const uint8_t *font, size_t length, bool valid)
{
    install_pack(font, length); /* Recompute CRC so structural validation is exercised. */
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert((font_store_get_source(FONT_STORE_SLOT_WQY9) != NULL) == valid);
}

static void test_glyph_record_boundaries(void)
{
    uint8_t font[300];
    for (unsigned field = 17U; field <= 21U; field += 2U) {
        make_font(font, 64U, FONT_STORE_SLOT_WQY9);
        write_u16_be(font + field, 41U); /* Offset points exactly past payload. */
        assert_font_structure(font, 64U, false);
    }

    make_font(font, 64U, FONT_STORE_SLOT_WQY9);
    write_u16_be(font + 17U, 40U); /* ASCII glyph without its size byte. */
    font[63] = 'A';
    assert_font_structure(font, 64U, false);

    make_font(font, 64U, FONT_STORE_SLOT_WQY9);
    font[23] = 'A'; font[24] = 1U; /* A record must include its own header. */
    assert_font_structure(font, 64U, false);

    make_font(font, sizeof(font), FONT_STORE_SLOT_WQY9);
    write_u16_be(font + 21U, 27U); /* Unicode lookup at byte 50. */
    write_u16_be(font + 50U, 4U);
    write_u16_be(font + 52U, 0xFFFFU);
    write_u16_be(font + 54U, 0x4E00U);
    font[56] = 3U; /* One minimal record, followed by a two-byte terminator. */
    assert_font_structure(font, 64U, true);
    font[56] = 2U;
    assert_font_structure(font, 64U, false);
    font[56] = 11U; /* Record extends beyond the payload. */
    assert_font_structure(font, 64U, false);
    font[56] = 9U; /* Next terminator has only one byte in the payload. */
    assert_font_structure(font, 64U, false);

    font[56] = 3U;
    write_u16_be(font + 17U, 227U);
    font[250] = 'A'; font[251] = 3U; /* Header crosses the 251-byte read cache. */
    assert_font_structure(font, sizeof(font), true);
}

int main(void)
{
    uint8_t font[300];
    make_font(font, sizeof(font), FONT_STORE_SLOT_WQY9);
    assert(font_crc32((const uint8_t*)"123456789", 9U) == 0xCBF43926UL);
    memset(mock_flash, 0xFF, sizeof(mock_flash));
    for (uint8_t version = 2U; version <= 3U; version++) {
        write_u32_le(mock_flash, version == 2U ? 0x32545346UL : 0x33545346UL);
        font_store_host_reset();
        assert(font_store_init() == NRF_ERROR_NOT_FOUND && !font_store_ready());
        assert(font_store_get_source(FONT_STORE_SLOT_WQY9) == NULL);
    }

    use_resource_layout = true;
    resource_payload_length = 0U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS && font_store_ready());
    assert(font_store_get_source(FONT_STORE_SLOT_WQY9) == NULL);

    uint8_t* payload = install_pack(font, sizeof(font));
    font_store_host_reset();
    watchdog_feeds = 0;
    assert(font_store_init() == NRF_SUCCESS);
    assert(watchdog_feeds >= 2); /* Both CRC chunks feed the watchdog. */
    assert_source(FONT_STORE_SLOT_WQY9, 0x18000U + RS_HEADER_SIZE + 56U, sizeof(font));
    assert(font_store_get_source(FONT_STORE_SLOT_WQY12) == NULL);
    payload[16U] ^= 1U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_get_source(FONT_STORE_SLOT_WQY9) == NULL);
    install_pack(font, sizeof(font));
    read_fail = 1; /* First CRC chunk succeeds; the second fails. */
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_get_source(FONT_STORE_SLOT_WQY9) == NULL);
    read_fail = -1;
    install_pack(font, sizeof(font));
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_source(FONT_STORE_SLOT_WQY9, 0x18000U + RS_HEADER_SIZE + 56U, sizeof(font));
    /* Optional fourth directory record has independent family identity and
     * either supported body size. Legacy three-record packages remain valid. */
    for (unsigned size = 0U; size < 2U; ++size) {
        make_font(font, sizeof(font), (font_store_slot_t)size);
        payload = &mock_flash[0x18000U + RS_HEADER_SIZE];
        memset(payload, 0, 72U);
        write_u32_le(payload, 0x314B5046UL); payload[4] = 1U; payload[6] = 4U;
        write_u32_le(payload + 56U, 72U);
        write_u32_le(payload + 60U, sizeof(font));
        write_u32_le(payload + 64U, font_crc32(font, sizeof(font)));
        write_u32_le(payload + 68U, 0x12345678UL);
        memcpy(payload + 72U, font, sizeof(font));
        resource_payload_length = 72U + sizeof(font);
        font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
        assert(font_store_get_source(FONT_STORE_SLOT_TODO) != NULL);
        assert(font_store_todo_font_size() == (size == 0U ? 9U : 12U));
        assert(font_store_todo_font_key() == 0x12345678UL);
        assert(memcmp(font_store_todo_font_metrics(), &font_slot_geometry[size],
                      sizeof(u8g2_font_placeholder_t)) == 0);
        write_u32_le(payload + 68U, 0U);
        font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
        assert(font_store_get_source(FONT_STORE_SLOT_TODO) == NULL);
        assert(font_store_todo_font_key() == 0U && font_store_todo_font_size() == 0U);
        assert(font_store_todo_font_metrics() == NULL);
    }
    for (unsigned i = 0U; i < TODO_FONT_SIZE_COUNT; ++i) {
        make_font(font, sizeof(font), FONT_STORE_SLOT_WQY9);
        memcpy(font + 9U, &todo_font_geometry[i], 8U);
        payload = install_todo_pack(font, sizeof(font));
        font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
        assert(font_store_todo_font_size() == todo_font_sizes[i]);
        assert(font_store_todo_font_key() == 0x12345678UL);
        assert(memcmp(font_store_todo_font_metrics(), &todo_font_geometry[i],
                      sizeof(u8g2_font_placeholder_t)) == 0);
        assert(font_store_todo_font_metrics()->x_offset < 0);
        payload[72U + 9U] = 65U; /* Geometry is bounded even with a valid CRC. */
        write_u32_le(payload + 64U, font_crc32(payload + 72U, sizeof(font)));
        font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
        assert(font_store_get_source(FONT_STORE_SLOT_TODO) == NULL);
        assert(font_store_todo_font_metrics() == NULL);
        install_pack(font, sizeof(font)); /* Base font geometry is now external too. */
        font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
        assert(font_store_get_source(FONT_STORE_SLOT_WQY9) != NULL);
    }
    payload = install_todo_pack(font, sizeof(font));
    make_font(font, sizeof(font), FONT_STORE_SLOT_HELVB18);
    memcpy(payload + 72U, font, sizeof(font));
    write_u32_le(payload + 68U, 1U);
    write_u32_le(payload + 64U, font_crc32(font, sizeof(font)));
    font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_get_source(FONT_STORE_SLOT_TODO) != NULL);
    assert(font_store_todo_font_size()==0); /* Available to UI, not a matching TODO font. */
    install_pack(font, sizeof(font));
    font_store_host_reset(); assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_todo_font_key() == 0U);
    test_glyph_record_boundaries();
    puts("PASS: ERS font packs, geometry/glyph bounds, cache boundaries and recovery");
    return 0;
}
