#define main original_store_tests_main
#include "test_font_store.c"
#undef main

static uint8_t snapshot[MOCK_FLASH_SIZE];
static uint8_t old_font[96], new_font[96];

static void reset_blank(void)
{
    power_cut_after = -1;
    memset(mock_flash, 0xFF, sizeof(mock_flash));
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    make_font(old_font, sizeof(old_font), FONT_STORE_SLOT_WQY9);
    memcpy(new_font, old_font, sizeof(new_font));
    new_font[80] ^= 0x55U;
}

static void assert_old_or_new(void)
{
    font_store_info_t info;
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert(font_store_get_info(FONT_STORE_SLOT_WQY9, &info));
    assert(info.present && info.length == sizeof(old_font));
    assert(info.crc32 == font_store_crc32(old_font, sizeof(old_font)) ||
           info.crc32 == font_store_crc32(new_font, sizeof(new_font)));
    uint8_t bytes[96];
    const u8g2_font_source_t* source = font_store_get_source(FONT_STORE_SLOT_WQY9);
    assert(source && source->read(source->context, 0U, bytes, sizeof(bytes)) == 0);
    assert(font_store_crc32(bytes, sizeof(bytes)) == info.crc32);
}

static void exercise_power_cuts(void)
{
    memcpy(snapshot, mock_flash, sizeof(snapshot));
    unsigned cuts = 0U;
    /* Cut every mutation, including every prefix of the 60-byte directory
     * and four-byte commit marker. Repeat for both directory directions. */
    for (volatile unsigned mutation = 0; mutation < 12U; mutation++) {
        for (volatile unsigned prefix = 0; prefix <= 96U; prefix++) {
            memcpy(mock_flash, snapshot, sizeof(snapshot));
            font_store_host_reset();
            assert(font_store_init() == NRF_SUCCESS);
            power_cut_bytes = prefix;
            power_cut_after = (int)mutation;
            if (setjmp(power_cut_jump) == 0) {
                upload_pack(new_font, sizeof(new_font), NULL, 0U, NULL, 0U);
            } else {
                cuts++;
            }
            power_cut_after = -1;
            assert_old_or_new();
        }
    }
    assert(cuts != 0U);
    memcpy(mock_flash, snapshot, sizeof(snapshot));
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
}

static void put_le32(uint8_t* p, uint32_t value)
{
    for (unsigned i = 0; i < 4U; i++) p[i] = value >> (8U * i);
}

static void install_legacy(uint32_t offset)
{
    uint8_t* raw = mock_flash;
    memset(raw, 0, 60U);
    put_le32(raw, 0x32545346UL);
    raw[4] = 2U;
    raw[6] = 3U;
    put_le32(raw + 8, 7U);
    put_le32(raw + 12, offset);
    put_le32(raw + 16, sizeof(old_font));
    put_le32(raw + 20, font_store_crc32(old_font, sizeof(old_font)));
    put_le32(raw + 48, font_store_crc32(raw, 48U));
    put_le32(raw + 52, 0xA55AA55AUL);
    put_le32(raw + 56, 0x5AA55AA5UL);
    memcpy(mock_flash + FONT_STORE_DATA_BASE + offset, old_font, sizeof(old_font));
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
}

static void test_bad_font_rejected(void)
{
    uint8_t font[96];
    reset_blank();
    memcpy(font, old_font, sizeof(font));
    write_u16_be(font + 21, 2U);
    write_u16_be(font + 25, 8U);
    write_u16_be(font + 27, 0x0100U);
    write_u16_be(font + 29, 6U);
    write_u16_be(font + 31, 0xFFFFU);
    write_u16_be(font + 33, 0x0100U);
    font[35] = 4U;
    write_u16_be(font + 39, 0x0200U);
    font_store_pack_slot_t slots[3] = {{sizeof(font), font_store_crc32(font, sizeof(font))}, {0}, {0}};
    assert(font_store_begin_pack(sizeof(font), slots[0].crc32, slots) == FONT_STORE_OK);
    assert(font_store_write_pack_chunk(0U, font, sizeof(font)) == FONT_STORE_OK);
    assert(font_store_commit_pack() == FONT_STORE_INVALID);
    assert(font_store_abort_update() == FONT_STORE_OK);
}

static void test_full_capacity(void)
{
    static uint8_t large[FONT_STORE_DATA_CAPACITY];
    reset_blank();
    make_font(large, sizeof(large), FONT_STORE_SLOT_WQY9);
    font_store_pack_slot_t slots[3] = {{sizeof(large), font_store_crc32(large, sizeof(large))}, {0}, {0}};
    assert(font_store_begin_pack(sizeof(large), slots[0].crc32, slots) == FONT_STORE_OK);
    for (size_t offset = 0; offset < sizeof(large); offset += 251U) {
        size_t length = sizeof(large) - offset;
        if (length > 251U) length = 251U;
        assert(font_store_write_pack_chunk(offset, large + offset, length) == FONT_STORE_OK);
    }
    assert(font_store_commit_pack() == FONT_STORE_OK);
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_present(FONT_STORE_SLOT_WQY9, sizeof(large), FONT_STORE_DATA_BASE);
    for (unsigned i = 0; i < FM25Q04B_SECTOR_SIZE; i++) assert(mock_flash[FONT_STORE_SECONDARY_ADDR + i] == 0xFFU);
    slots[0].length = sizeof(old_font);
    assert(font_store_begin_pack(sizeof(old_font), 0U, slots) == FONT_STORE_NO_SPACE);
    upload_pack(NULL, 0U, NULL, 0U, NULL, 0U);
    font_store_host_reset();
    assert(font_store_init() == NRF_SUCCESS);
    assert_missing(FONT_STORE_SLOT_WQY9);
}

int main(void)
{
    reset_blank();
    upload_pack(old_font, sizeof(old_font), NULL, 0U, NULL, 0U);
    exercise_power_cuts();
    upload_pack(old_font, sizeof(old_font), NULL, 0U, NULL, 0U);
    exercise_power_cuts();
    /* Legacy package in the newly reserved last sector: migrate safely
     * without erasing that data while the legacy directory is active. */
    reset_blank();
    install_legacy(FONT_STORE_DATA_CAPACITY);
    exercise_power_cuts();
    /* An interrupted initial migration leaves a nonblank record. Retry
     * using a later record without erasing the old directory. */
    memset(mock_flash + FONT_STORE_PRIMARY_ADDR, 0x12, 60U);
    upload_pack(new_font, sizeof(new_font), NULL, 0U, NULL, 0U);
    assert_old_or_new();
    /* Tail exhaustion cycles into free prefix sectors. */
    reset_blank();
    for (unsigned i = 0; i < 140U; i++) upload_pack(old_font, sizeof(old_font), NULL, 0U, NULL, 0U);
    const fm25q04b_region_t* region = font_store_get_source(FONT_STORE_SLOT_WQY9)->context;
    assert(region->base_address < 0x20000UL);
    assert_old_or_new();
    /* Erase accepted by NOR but completion times out: old package survives. */
    font_store_pack_slot_t slots[3] = {{sizeof(new_font), font_store_crc32(new_font, sizeof(new_font))}, {0}, {0}};
    assert(font_store_begin_pack(sizeof(new_font), slots[0].crc32, slots) == FONT_STORE_OK);
    fail_next_data_erase = true;
    assert(font_store_write_pack_chunk(0U, new_font, sizeof(new_font)) == FONT_STORE_IO);
    assert(font_store_abort_update() == FONT_STORE_OK);
    assert_old_or_new();
    test_bad_font_rejected();
    test_full_capacity();
    puts("PASS: NOR power cuts, FST2 migration, cyclic reuse and parser validation");
    return 0;
}
