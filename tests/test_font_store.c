#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "resource_store.h"

#define MOCK_FLASH_SIZE FM25Q04B_SIZE_BYTES

static uint8_t mock_flash[MOCK_FLASH_SIZE];
static bool use_resource_layout;
static uint32_t resource_payload_length;

rs_result_t resource_store_mount(void) { return use_resource_layout ? RS_OK : RS_NOT_FOUND; }
rs_result_t resource_store_find(uint16_t id, rs_partition_t *p)
{
    if (!use_resource_layout || id != 7U || p == NULL) return RS_NOT_FOUND;
    *p = (rs_partition_t){7U, 7U, 1U, 0U, 0x18000U, 0x40000U};
    return RS_OK;
}
rs_result_t resource_store_info(uint16_t id, rs_package_info_t *p)
{
    if (!use_resource_layout || id != 7U || p == NULL) return RS_NOT_FOUND;
    *p = (rs_package_info_t){42U, resource_payload_length, 0U};
    return RS_OK;
}
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    if (!use_resource_layout || id != 7U || length == NULL) return RS_NOT_FOUND;
    *length = resource_payload_length;
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

int main(void)
{
    uint8_t font[96];
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
    assert(font_store_init() == NRF_SUCCESS);
    assert_source(FONT_STORE_SLOT_WQY9, 0x18000U + RS_HEADER_SIZE + 56U, sizeof(font));
    assert(font_store_get_source(FONT_STORE_SLOT_WQY12) == NULL);
    payload[16U] ^= 1U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_get_source(FONT_STORE_SLOT_WQY9) == NULL);
    install_pack(font, sizeof(font));
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_source(FONT_STORE_SLOT_WQY9, 0x18000U + RS_HEADER_SIZE + 56U, sizeof(font));
    puts("PASS: ERS font pack, missing/corrupt pack recovery and legacy rejection");
    return 0;
}
