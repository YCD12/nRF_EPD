#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "fm25q04b.h"
#include "font_store.h"
#include "resource_store.h"

#define MOCK_FLASH_SIZE FM25Q04B_SIZE_BYTES

static uint8_t mock_flash[MOCK_FLASH_SIZE];
static bool fail_next_manifest_commit;
static bool fail_next_data_write;
static bool fail_next_data_erase;
static bool corrupt_next_font_read;
static unsigned erase_count;
static unsigned flash_read_count;
static int power_cut_after = -1;
static size_t power_cut_bytes;
static jmp_buf power_cut_jump;
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
    flash_read_count++;
    memcpy(data, &mock_flash[address], length);
    if (corrupt_next_font_read && address >= FONT_STORE_DATA_BASE && length >= 64U) {
        ((uint8_t*)data)[0] ^= 0x01U;
        corrupt_next_font_read = false;
    }
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

ret_code_t fm25q04b_write(uint32_t address, const void* data, size_t length)
{
    if (data == NULL && length != 0U) {
        return NRF_ERROR_NULL;
    }
    if (address > MOCK_FLASH_SIZE || length > MOCK_FLASH_SIZE - address) {
        return NRF_ERROR_INVALID_ADDR;
    }
    if (fail_next_manifest_commit && length == 4U &&
        ((address < FONT_STORE_DATA_BASE && address % 64U == 52U) ||
         address == FONT_STORE_SECONDARY_ADDR + 52U)) {
        fail_next_manifest_commit = false;
        return NRF_ERROR_INTERNAL;
    }
    if (fail_next_data_write && address >= FONT_STORE_DATA_BASE &&
        address < MOCK_FLASH_SIZE) {
        fail_next_data_write = false;
        return NRF_ERROR_INTERNAL;
    }
    bool cut = power_cut_after == 0;
    if (power_cut_after >= 0) power_cut_after--;
    size_t programmed = cut && power_cut_bytes < length ? power_cut_bytes : length;
    for (size_t i = 0; i < programmed; i++) {
        mock_flash[address + i] &= ((const uint8_t*)data)[i];
    }
    if (cut) longjmp(power_cut_jump, 1);
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_erase_sector(uint32_t address)
{
    address &= ~(FM25Q04B_SECTOR_SIZE - 1UL);
    if (address >= MOCK_FLASH_SIZE ||
        FM25Q04B_SECTOR_SIZE > MOCK_FLASH_SIZE - address) {
        return NRF_ERROR_INVALID_ADDR;
    }
    memset(&mock_flash[address], 0xFF, FM25Q04B_SECTOR_SIZE);
    erase_count++;
    if (power_cut_after == 0) longjmp(power_cut_jump, 1);
    if (power_cut_after > 0) power_cut_after--;
    if (fail_next_data_erase && address >= FONT_STORE_DATA_BASE) {
        fail_next_data_erase = false;
        return NRF_ERROR_TIMEOUT;
    }
    return NRF_SUCCESS;
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

static void upload_pack(const uint8_t* wqy9, size_t wqy9_length,
                        const uint8_t* wqy12, size_t wqy12_length,
                        const uint8_t* helvb18, size_t helvb18_length)
{
    const uint8_t* data[FONT_STORE_SLOT_COUNT] = {wqy9, wqy12, helvb18};
    const size_t lengths[FONT_STORE_SLOT_COUNT] = {
        wqy9_length, wqy12_length, helvb18_length,
    };
    font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT];
    uint8_t packed[sizeof((uint8_t[96]){0}) * FONT_STORE_SLOT_COUNT];
    size_t total_length = 0U;
    size_t offset = 0U;
    uint8_t slot;

    for (slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        slots[slot].length = (uint32_t)lengths[slot];
        slots[slot].crc32 = lengths[slot] == 0U ? 0U :
                            font_store_crc32(data[slot], lengths[slot]);
        total_length += lengths[slot];
        if (lengths[slot] != 0U) {
            assert(total_length <= sizeof(packed));
            memcpy(&packed[offset], data[slot], lengths[slot]);
            offset += lengths[slot];
        }
    }

    assert(font_store_begin_pack((uint32_t)total_length,
                                 font_store_crc32(packed, total_length),
                                 slots) == FONT_STORE_OK);
    for (offset = 0U; offset < total_length; offset += 19U) {
        size_t chunk = (total_length - offset > 19U) ? 19U : total_length - offset;
        assert(font_store_write_pack_chunk((uint32_t)offset, &packed[offset], chunk) ==
               FONT_STORE_OK);
    }
    if (total_length == 0U) {
        assert(font_store_write_pack_chunk(0U, NULL, 0U) == FONT_STORE_OK);
    }
    assert(font_store_commit_pack() == FONT_STORE_OK);
}

static void assert_present(font_store_slot_t slot, size_t length, uint32_t address)
{
    font_store_info_t info;
    const u8g2_font_source_t* source;
    const fm25q04b_region_t* region;

    assert(font_store_get_info(slot, &info));
    assert(info.present == 1U);
    assert(info.length == length);
    assert(info.crc32 != 0U);
    source = font_store_get_source(slot);
    assert(source != NULL);
    region = (const fm25q04b_region_t*)source->context;
    assert(region != NULL && region->base_address == address);
}

static void assert_missing(font_store_slot_t slot)
{
    font_store_info_t info;

    assert(font_store_get_info(slot, &info));
    assert(info.present == 0U);
    assert(info.length == 0U);
    assert(info.crc32 == 0U);
    assert(font_store_get_source(slot) == NULL);
}

int main(void)
{
    uint8_t wqy9[96];
    uint8_t wqy12[96];
    uint8_t helvb18[96];
    uint8_t readback[8];
    uint8_t expected_wqy12[8];
    const u8g2_font_source_t* source;
    const fm25q04b_region_t* region;
    unsigned erase_count_before;

    memset(mock_flash, 0xFF, sizeof(mock_flash));
    font_store_host_reset();
    assert(font_store_crc32((const uint8_t*)"123456789", 9U) == 0xCBF43926UL);
    assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_ready());
    assert_missing(FONT_STORE_SLOT_WQY9);
    assert_missing(FONT_STORE_SLOT_WQY12);
    assert_missing(FONT_STORE_SLOT_HELVB18);

    make_font(wqy9, sizeof(wqy9), FONT_STORE_SLOT_WQY9);
    make_font(wqy12, sizeof(wqy12), FONT_STORE_SLOT_WQY12);
    make_font(helvb18, sizeof(helvb18), FONT_STORE_SLOT_HELVB18);

    erase_count_before = erase_count;
    upload_pack(wqy9, sizeof(wqy9), wqy12, sizeof(wqy12), helvb18, sizeof(helvb18));
    assert(erase_count == erase_count_before + 2U); /* data sector + Manifest */
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE);
    assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 96U);
    assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 192U);

    /* READY boot scans the bounded directory area and one fixed 23-byte
     * header per present slot. It must not scan the complete font records. */
    flash_read_count = 0U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert(flash_read_count == 68U); /* layout guard + directory scan + three headers */

    source = font_store_get_source(FONT_STORE_SLOT_WQY12);
    assert(source != NULL);
    region = (const fm25q04b_region_t*)source->context;
    assert(region != NULL && region->base_address == FONT_STORE_DATA_BASE + 96U);
    assert(source->read(source->context, 32U, readback, sizeof(readback)) == 0);
    memcpy(expected_wqy12, &wqy12[32], sizeof(expected_wqy12));
    assert(memcmp(readback, expected_wqy12, sizeof(readback)) == 0);

    /* Omitting WQY12 appends a new package at the next erased sector.  The
     * published records remain compact within the package, but their base
     * address is no longer forced back to the beginning of the data area. */
    upload_pack(wqy9, sizeof(wqy9), NULL, 0U, helvb18, sizeof(helvb18));
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x1000U);
    assert_missing(FONT_STORE_SLOT_WQY12);
    assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x1000U + 96U);

    /* A reset after the first data write leaves the previous READY manifest
     * intact in append mode.  The next boot therefore keeps the old package
     * visible while the partial new package remains unpublished. */
    upload_pack(wqy9, sizeof(wqy9), wqy12, sizeof(wqy12), helvb18, sizeof(helvb18));
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {sizeof(wqy9), font_store_crc32(wqy9, sizeof(wqy9))},
            {sizeof(wqy12), font_store_crc32(wqy12, sizeof(wqy12))},
            {sizeof(helvb18), font_store_crc32(helvb18, sizeof(helvb18))},
        };
        uint8_t packed[288];
        memcpy(&packed[0], wqy9, sizeof(wqy9));
        memcpy(&packed[96], wqy12, sizeof(wqy12));
        memcpy(&packed[192], helvb18, sizeof(helvb18));
        assert(font_store_begin_pack(sizeof(packed), font_store_crc32(packed, sizeof(packed)),
                                     slots) == FONT_STORE_OK);
        assert(font_store_write_pack_chunk(0U, packed, 19U) == FONT_STORE_OK);
    }
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x2000U);
    assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 0x2000U + 96U);
    assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x2000U + 192U);
    upload_pack(wqy9, sizeof(wqy9), wqy12, sizeof(wqy12), helvb18, sizeof(helvb18));

    /* A readback fault during commit is rejected before publishing the pack. */
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {sizeof(wqy9), font_store_crc32(wqy9, sizeof(wqy9))},
            {sizeof(wqy12), font_store_crc32(wqy12, sizeof(wqy12))},
            {sizeof(helvb18), font_store_crc32(helvb18, sizeof(helvb18))},
        };
        uint8_t packed[288];
        memcpy(&packed[0], wqy9, sizeof(wqy9));
        memcpy(&packed[96], wqy12, sizeof(wqy12));
        memcpy(&packed[192], helvb18, sizeof(helvb18));
        assert(font_store_begin_pack(sizeof(packed), font_store_crc32(packed, sizeof(packed)),
                                     slots) == FONT_STORE_OK);
        assert(font_store_write_pack_chunk(0U, packed, sizeof(packed)) == FONT_STORE_OK);
        corrupt_next_font_read = true;
        assert(font_store_commit_pack() == FONT_STORE_CRC);
        assert(font_store_abort_update() == FONT_STORE_OK);
        assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x3000U);
        assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 0x3000U + 96U);
        assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x3000U + 192U);
    }
    upload_pack(wqy9, sizeof(wqy9), wqy12, sizeof(wqy12), helvb18, sizeof(helvb18));

    /* Full-package deletion removes only WQY12 and compacts HELVB18 inside
     * the newly appended package. */
    upload_pack(wqy9, sizeof(wqy9), NULL, 0U, helvb18, sizeof(helvb18));
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x5000U);
    assert_missing(FONT_STORE_SLOT_WQY12);
    assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x5000U + 96U);

    /* If data programming fails after an erase, abort preserves the old
     * package because append mode never touches its sectors. */
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {sizeof(wqy9), font_store_crc32(wqy9, sizeof(wqy9))},
            {sizeof(wqy12), font_store_crc32(wqy12, sizeof(wqy12))},
            {sizeof(helvb18), font_store_crc32(helvb18, sizeof(helvb18))},
        };
        assert(font_store_begin_pack(0U, 0U, slots) == FONT_STORE_INVALID);
        fail_next_data_write = true;
        {
            uint8_t packed[288];
            memcpy(&packed[0], wqy9, sizeof(wqy9));
            memcpy(&packed[96], wqy12, sizeof(wqy12));
            memcpy(&packed[192], helvb18, sizeof(helvb18));
            assert(font_store_begin_pack(sizeof(packed), font_store_crc32(packed, sizeof(packed)),
                                         slots) == FONT_STORE_OK);
            assert(font_store_write_pack_chunk(0U, packed, 19U) == FONT_STORE_IO);
        }
        assert(font_store_abort_update() == FONT_STORE_OK);
        assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x5000U);
        assert_missing(FONT_STORE_SLOT_WQY12);
        assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x5000U + 96U);
    }
    upload_pack(wqy9, sizeof(wqy9), wqy12, sizeof(wqy12), helvb18, sizeof(helvb18));

    /* BEGIN followed by ABORT without data preserves the committed package. */
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {sizeof(wqy9), font_store_crc32(wqy9, sizeof(wqy9))},
            {sizeof(wqy12), font_store_crc32(wqy12, sizeof(wqy12))},
            {sizeof(helvb18), font_store_crc32(helvb18, sizeof(helvb18))},
        };
        assert(font_store_begin_pack(288U, 0U, slots) == FONT_STORE_OK);
        assert(font_store_abort_update() == FONT_STORE_OK);
        assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x6000U);
        assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 0x6000U + 96U);
        assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x6000U + 192U);

        /* The explicit abort also restores the persistent ready marker; a
         * reboot before the next upload must keep the old package visible. */
        font_store_host_reset();
        assert(font_store_init() == NRF_SUCCESS);
        assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x6000U);
        assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 0x6000U + 96U);
        assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x6000U + 192U);
    }

    /* A Manifest commit failure must restore the previous manifest when the
     * old data package was preserved by append mode. */
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {sizeof(wqy9), font_store_crc32(wqy9, sizeof(wqy9))},
            {sizeof(wqy12), font_store_crc32(wqy12, sizeof(wqy12))},
            {sizeof(helvb18), font_store_crc32(helvb18, sizeof(helvb18))},
        };
        uint8_t packed[288];
        memcpy(&packed[0], wqy9, sizeof(wqy9));
        memcpy(&packed[96], wqy12, sizeof(wqy12));
        memcpy(&packed[192], helvb18, sizeof(helvb18));
        fail_next_manifest_commit = true;
        assert(font_store_begin_pack(sizeof(packed), font_store_crc32(packed, sizeof(packed)),
                                     slots) == FONT_STORE_OK);
        assert(font_store_write_pack_chunk(0U, packed, sizeof(packed)) == FONT_STORE_OK);
        assert(font_store_commit_pack() == FONT_STORE_IO);
    }
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x6000U);
    assert_present(FONT_STORE_SLOT_WQY12, sizeof(wqy12), FONT_STORE_DATA_BASE + 0x6000U + 96U);
    assert_present(FONT_STORE_SLOT_HELVB18, sizeof(helvb18), FONT_STORE_DATA_BASE + 0x6000U + 192U);

    /* No staging space: BEGIN rejects without destroying the old package. */
    {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT] = {
            {FONT_STORE_DATA_CAPACITY, 0U}, {0U, 0U}, {0U, 0U},
        };
        assert(font_store_begin_pack(FONT_STORE_DATA_CAPACITY, 0U, slots) == FONT_STORE_NO_SPACE);
        font_store_host_reset();
        assert(font_store_init() == NRF_SUCCESS);
        assert_present(FONT_STORE_SLOT_WQY9, sizeof(wqy9), FONT_STORE_DATA_BASE + 0x6000U);
    }

    /* A committed ERS font partition supersedes FST2/FST3. Its directory and
     * every u8g2 stream are checked before any slot becomes visible. */
    memset(mock_flash, 0xFF, sizeof(mock_flash));
    use_resource_layout = true;
    resource_payload_length = 56U + 3U * 96U;
    uint8_t *payload = &mock_flash[0x18000U + RS_HEADER_SIZE];
    memset(payload, 0, 56U);
    write_u32_le(payload, 0x314B5046UL);
    payload[4] = 1U;
    payload[6] = 3U;
    const uint8_t *font_data[] = {wqy9, wqy12, helvb18};
    for (unsigned slot = 0U; slot < 3U; slot++) {
        uint32_t record = 8U + slot * 16U;
        uint32_t offset = 56U + slot * 96U;
        write_u32_le(payload + record, offset);
        write_u32_le(payload + record + 4U, 96U);
        write_u32_le(payload + record + 8U, font_store_crc32(font_data[slot], 96U));
        memcpy(payload + offset, font_data[slot], 96U);
    }
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_present(FONT_STORE_SLOT_WQY9, 96U, 0x18000U + RS_HEADER_SIZE + 56U);
    assert_present(FONT_STORE_SLOT_WQY12, 96U, 0x18000U + RS_HEADER_SIZE + 152U);
    font_store_info_t resource_info;
    assert(font_store_get_info(FONT_STORE_SLOT_HELVB18, &resource_info));
    assert(resource_info.revision == 42U);
    font_store_pack_slot_t read_only_slots[3] = {{96U, 0U}, {96U, 0U}, {96U, 0U}};
    assert(font_store_begin_pack(288U, 0U, read_only_slots) == FONT_STORE_INVALID);
    payload[8U + 8U] ^= 1U;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_missing(FONT_STORE_SLOT_WQY9);
    assert_missing(FONT_STORE_SLOT_WQY12);
    assert_missing(FONT_STORE_SLOT_HELVB18);

    puts("PASS: dynamic font package transaction and power-loss behavior");
    return 0;
}
