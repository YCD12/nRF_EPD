#include "font_store.h"

#include <string.h>

#include "main.h"
#include "font_geometry.h"
#include "resource_store.h"
#include "resource_layout.h"

#define FONT_STORE_MAGIC              0x33545346UL /* "FST3" */
#define FONT_STORE_VERSION            3U
#define FONT_STORE_COMMIT             0xA55AA55AUL
#define FONT_STORE_LEGACY_MAGIC       0x32545346UL
#define FONT_STORE_NO_MANIFEST        UINT32_MAX
#define FONT_STORE_LEGACY_CAPACITY    (FM25Q04B_SIZE_BYTES - FONT_STORE_DATA_BASE)
#define FONT_STORE_MANIFEST_SIZE      60U
#define FONT_STORE_RECORD_SIZE        12U
#define FONT_STORE_MANIFEST_CRC_OFFSET 48U
#define FONT_STORE_MANIFEST_COMMIT_OFFSET 52U
#define FONT_STORE_MANIFEST_STATE_OFFSET 56U
#define FONT_STORE_MANIFEST_STATE_READY  0x5AA55AA5UL
#define FONT_PACK_MAGIC              0x314B5046UL /* FPK1 */
#define FONT_PACK_VERSION            1U
#define FONT_PACK_HEADER_SIZE        8U
#define FONT_PACK_RECORD_SIZE        16U
#define FONT_PACK_DIRECTORY_SIZE     (FONT_PACK_HEADER_SIZE + FONT_STORE_SLOT_COUNT * FONT_PACK_RECORD_SIZE)
typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} font_record_t;

#define FONT_VALIDATE_CACHE_SIZE 251U

typedef struct {
    uint8_t bytes[23];
} font_header_t;

typedef struct {
    uint32_t address;
    uint32_t length;
    uint32_t cache_base;
    uint16_t cache_length;
    bool cache_valid;
    uint8_t cache[FONT_VALIDATE_CACHE_SIZE];
} font_flash_reader_t;

static bool validate_font_header(font_store_slot_t slot, uint32_t address, uint32_t length);
static bool validate_font_structure(font_store_slot_t slot, uint32_t address, uint32_t length);
static bool validate_font_records(uint32_t address, uint32_t length,
                                  uint16_t start_upper, uint16_t start_lower,
                                  uint16_t start_unicode);
static uint8_t m_ready;
static uint32_t m_generation;
static uint32_t m_revision = 1U;
static font_record_t m_records[FONT_STORE_SLOT_COUNT];

static uint32_t m_manifest_address = FONT_STORE_NO_MANIFEST;
static uint32_t m_data_base = FONT_STORE_DATA_BASE;
static fm25q04b_region_t m_regions[FONT_STORE_SLOT_COUNT];
static u8g2_font_source_t m_sources[FONT_STORE_SLOT_COUNT];

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t font_store_crc32_update(uint32_t crc, const uint8_t* data, size_t length)
{
    size_t i;

    for (i = 0U; i < length; i++) {
        uint8_t bit;
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t font_store_crc32(const uint8_t* data, size_t length)
{
    if (data == NULL && length != 0U) return 0U;
    return font_store_crc32_update(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
}

static bool font_store_crc32_flash(uint32_t address, uint32_t length,
                                   uint32_t* out_crc)
{
    uint8_t buf[251];
    uint32_t crc = 0xFFFFFFFFUL;

    if (out_crc == NULL) {
        return false;
    }

    while (length > 0U) {
        size_t chunk = (length > sizeof(buf)) ? sizeof(buf) : length;

        if (fm25q04b_read(address, buf, chunk) != NRF_SUCCESS) {
            return false;
        }
        crc = font_store_crc32_update(crc, buf, chunk);
        address += (uint32_t)chunk;
        length -= (uint32_t)chunk;
        app_feed_wdt();
    }
    *out_crc = crc ^ 0xFFFFFFFFUL;
    return true;
}

static bool slot_valid(font_store_slot_t slot)
{
    return slot < FONT_STORE_SLOT_COUNT;
}

static uint32_t record_address(const font_record_t* record)
{
    return m_data_base + record->offset;
}

static bool record_valid(const font_record_t* record, uint32_t capacity)
{
    if (record == NULL) {
        return false;
    }
    if (record->length == 0U) {
        return record->offset == 0U && record->crc32 == 0U;
    }
    if (record->offset >= capacity ||
        record->length > capacity - record->offset) {
        return false;
    }
    return true;
}

static bool records_non_overlapping(const font_record_t* records)
{
    uint8_t i;
    uint8_t j;

    for (i = 0U; i < FONT_STORE_SLOT_COUNT; i++) {
        if (records[i].length == 0U) {
            continue;
        }
        for (j = (uint8_t)(i + 1U); j < FONT_STORE_SLOT_COUNT; j++) {
            uint32_t i_end;
            uint32_t j_end;

            if (records[j].length == 0U) {
                continue;
            }
            i_end = records[i].offset + records[i].length;
            j_end = records[j].offset + records[j].length;
            if (records[i].offset < j_end && records[j].offset < i_end) {
                return false;
            }
        }
    }
    return true;
}

static void font_store_update_sources(void)
{
    uint8_t slot;

    for (slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        m_regions[slot].base_address = m_data_base + m_records[slot].offset;
        m_regions[slot].length = m_records[slot].length;
        m_sources[slot].data = NULL;
        m_sources[slot].length = m_records[slot].length;
        m_sources[slot].read = fm25q04b_read_region;
        m_sources[slot].context = &m_regions[slot];
        m_sources[slot].revision = m_revision;
    }
}

static bool resource_font_pack_load(void)
{
    uint8_t raw[FONT_PACK_DIRECTORY_SIZE];
    rs_partition_t partition;
    rs_package_info_t package;
    uint32_t checked_length;

    if (resource_store_find(RS_PART_FONTS, &partition) != RS_OK ||
        resource_store_info(RS_PART_FONTS, &package) != RS_OK ||
        resource_store_open(RS_PART_FONTS, &checked_length) != RS_OK ||
        checked_length != package.length || package.length < sizeof(raw) ||
        resource_store_read(RS_PART_FONTS, 0U, raw, sizeof(raw)) != RS_OK ||
        get_u32(raw) != FONT_PACK_MAGIC || get_u16(&raw[4]) != FONT_PACK_VERSION ||
        get_u16(&raw[6]) != FONT_STORE_SLOT_COUNT) return false;

    memset(m_records, 0, sizeof(m_records));
    for (uint8_t slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        uint32_t base = FONT_PACK_HEADER_SIZE + slot * FONT_PACK_RECORD_SIZE;
        font_record_t *record = &m_records[slot];
        record->offset = get_u32(&raw[base]);
        record->length = get_u32(&raw[base + 4U]);
        record->crc32 = get_u32(&raw[base + 8U]);
        if (get_u32(&raw[base + 12U]) != 0U ||
            (record->length == 0U && (record->offset != 0U || record->crc32 != 0U)) ||
            (record->length != 0U &&
             (record->offset < sizeof(raw) || record->offset >= package.length ||
              record->length > package.length - record->offset))) {
            memset(m_records, 0, sizeof(m_records));
            return false;
        }
    }
    if (!records_non_overlapping(m_records)) {
        memset(m_records, 0, sizeof(m_records));
        return false;
    }
    m_data_base = partition.offset + RS_HEADER_SIZE;
    for (uint8_t slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        uint32_t crc;
        font_record_t *record = &m_records[slot];
        if (record->length == 0U) continue;
        if (!font_store_crc32_flash(record_address(record), record->length, &crc) ||
            crc != record->crc32 ||
            !validate_font_structure((font_store_slot_t)slot,
                                     record_address(record), record->length)) {
            memset(m_records, 0, sizeof(m_records));
            return false;
        }
    }
    m_revision = package.revision;
    return true;
}

static bool manifest_read_at(uint32_t address, font_record_t* records,
                             uint32_t* generation, bool legacy)
{
    uint8_t raw[FONT_STORE_MANIFEST_SIZE];
    uint32_t state;
    if (fm25q04b_read(address, raw, sizeof(raw)) != NRF_SUCCESS) return false;
    state = get_u32(&raw[FONT_STORE_MANIFEST_STATE_OFFSET]);
    if (get_u32(raw) != (legacy ? FONT_STORE_LEGACY_MAGIC : FONT_STORE_MAGIC) ||
        get_u16(&raw[4]) != (legacy ? 2U : FONT_STORE_VERSION) ||
        raw[6] != FONT_STORE_SLOT_COUNT ||
        get_u32(&raw[FONT_STORE_MANIFEST_COMMIT_OFFSET]) != FONT_STORE_COMMIT ||
        (state != FONT_STORE_MANIFEST_STATE_READY && !(legacy && state == UINT32_MAX)) ||
        font_store_crc32(raw, FONT_STORE_MANIFEST_CRC_OFFSET) !=
            get_u32(&raw[FONT_STORE_MANIFEST_CRC_OFFSET])) return false;

    for (uint8_t slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        uint32_t base = 12U + slot * FONT_STORE_RECORD_SIZE;
        records[slot].offset = get_u32(&raw[base]);
        records[slot].length = get_u32(&raw[base + 4U]);
        records[slot].crc32 = get_u32(&raw[base + 8U]);
        if (!record_valid(&records[slot], legacy ? FONT_STORE_LEGACY_CAPACITY :
                                                 FONT_STORE_DATA_CAPACITY)) return false;
    }
    if (!records_non_overlapping(records)) return false;
    *generation = get_u32(&raw[8]);
    return true;
}

static void manifest_load(void)
{
    font_record_t candidate[FONT_STORE_SLOT_COUNT];
    uint32_t generation;
    uint32_t address = FONT_STORE_PRIMARY_ADDR;
    m_manifest_address = FONT_STORE_NO_MANIFEST;
    /* The first migration appends in the old directory's unused bytes.
     * Scan the fixed 4 KiB area so torn migration attempts can be skipped
     * without ever erasing the still-published FST2 directory. */
    for (;;) {
        if (manifest_read_at(address, candidate, &generation, false) &&
            (m_manifest_address == FONT_STORE_NO_MANIFEST ||
             (int32_t)(generation - m_generation) > 0)) {
            memcpy(m_records, candidate, sizeof(m_records));
            m_generation = generation;
            m_manifest_address = address;
        }
        if (address == FONT_STORE_SECONDARY_ADDR) break;
        address += 64U;
        if (address >= FM25Q04B_SECTOR_SIZE) address = FONT_STORE_SECONDARY_ADDR;
    }
    if (m_manifest_address == FONT_STORE_NO_MANIFEST &&
        manifest_read_at(0U, candidate, &generation, true)) {
        memcpy(m_records, candidate, sizeof(m_records));
        m_generation = generation;
        m_manifest_address = 0U;
    }
    /* Check headers only on FST3 boot. Payloads were read back at COMMIT;
     * the runtime parser remains bounded even if storage later degrades.
     * FST2 is checked fully because old writers had weaker validation. */
    for (uint8_t slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
        font_record_t* record = &m_records[slot];
        uint32_t crc;
        if (record->length == 0U) continue;
        if (!validate_font_header((font_store_slot_t)slot, record_address(record), record->length) ||
            (m_manifest_address == 0U &&
             (!font_store_crc32_flash(record_address(record), record->length, &crc) ||
              crc != record->crc32 ||
              !validate_font_structure((font_store_slot_t)slot, record_address(record), record->length)))) {
            memset(record, 0, sizeof(*record));
        }
    }
}

static uint16_t header_word(const font_header_t* header, unsigned offset)
{
    return (uint16_t)((uint16_t)header->bytes[offset] << 8) | header->bytes[offset + 1U];
}

static bool read_font_header(uint32_t address, uint32_t length, font_header_t* header)
{
    return header != NULL && length >= sizeof(header->bytes) &&
           fm25q04b_read(address, header->bytes, sizeof(header->bytes)) == NRF_SUCCESS;
}

static bool font_header_matches_slot(font_store_slot_t slot, uint32_t length,
                                     const font_header_t* header)
{
    if (!slot_valid(slot) || header == NULL || header->bytes[0] == 0U ||
        header->bytes[0] == 0xFFU) return false;
    for (unsigned i = 2U; i <= 8U; i++) {
        if (header->bytes[i] == 0U || header->bytes[i] > 8U) return false;
    }
    /* The first eight geometry fields use the same byte order as u8g2. */
    if (memcmp(header->bytes + 9U, &font_slot_geometry[slot], 8U) != 0) return false;
    for (unsigned i = 17U; i <= 21U; i += 2U) {
        if ((uint32_t)header_word(header, i) + 23U >= length) return false;
    }
    return true;
}

static bool validate_font_header(font_store_slot_t slot, uint32_t address, uint32_t length)
{
    font_header_t header;

    return read_font_header(address, length, &header) &&
           font_header_matches_slot(slot, length, &header);
}

static bool validate_font_structure(font_store_slot_t slot, uint32_t address, uint32_t length)
{
    font_header_t header;

    if (!read_font_header(address, length, &header) ||
        !font_header_matches_slot(slot, length, &header)) {
        return false;
    }
    return validate_font_records(address, length, header_word(&header, 17U),
                                 header_word(&header, 19U), header_word(&header, 21U));
}

static bool font_reader_byte(font_flash_reader_t* reader, uint32_t offset,
                             uint8_t* value)
{
    uint32_t base;
    size_t read_length;

    if (reader == NULL || value == NULL || offset >= reader->length) {
        return false;
    }
    if (!reader->cache_valid || offset < reader->cache_base ||
        offset >= reader->cache_base + reader->cache_length) {
        base = (offset / FONT_VALIDATE_CACHE_SIZE) * FONT_VALIDATE_CACHE_SIZE;
        read_length = FONT_VALIDATE_CACHE_SIZE;
        if (read_length > (size_t)(reader->length - base)) {
            read_length = (size_t)(reader->length - base);
        }
        if (fm25q04b_read(reader->address + base, reader->cache, read_length) != NRF_SUCCESS) {
            reader->cache_valid = false;
            return false;
        }
        reader->cache_base = base;
        reader->cache_length = (uint16_t)read_length;
        reader->cache_valid = true;
    }
    *value = reader->cache[offset - reader->cache_base];
    return true;
}

static bool font_reader_u16(font_flash_reader_t* reader, uint32_t offset,
                            uint16_t* value)
{
    uint8_t high;
    uint8_t low;

    if (!font_reader_byte(reader, offset, &high) ||
        !font_reader_byte(reader, offset + 1U, &low) || value == NULL) {
        return false;
    }
    *value = (uint16_t)(((uint16_t)high << 8) | low);
    return true;
}

static bool validate_ascii_records(font_flash_reader_t* reader, uint32_t offset)
{
    uint32_t record_count = 0U;

    while (record_count++ < reader->length) {
        uint8_t glyph;
        uint8_t size;

        if (!font_reader_byte(reader, offset, &glyph) ||
            !font_reader_byte(reader, offset + 1U, &size)) {
            return false;
        }
        (void)glyph;
        if (size == 0U) {
            return true;
        }
        if (size < 2U || size > reader->length - offset) {
            return false;
        }
        offset += size;
    }
    return false;
}

static bool validate_unicode_records(font_flash_reader_t* reader, uint32_t offset)
{
    uint32_t record_count = 0U;

    while (record_count++ < reader->length) {
        uint16_t glyph;
        uint8_t size;

        if (!font_reader_u16(reader, offset, &glyph)) {
            return false;
        }
        if (glyph == 0U) {
            return true;
        }
        if (!font_reader_byte(reader, offset + 2U, &size) ||
            size < 3U || size > reader->length - offset) {
            return false;
        }
        offset += size;
    }
    return false;
}

static bool validate_font_records(uint32_t address, uint32_t length,
                                  uint16_t start_upper, uint16_t start_lower,
                                  uint16_t start_unicode)
{
    font_flash_reader_t reader;
    uint32_t unicode_table;
    uint16_t lookup_offset;
    uint16_t first_codepoint;

    memset(&reader, 0, sizeof(reader));
    reader.address = address;
    reader.length = length;

    if ((uint32_t)23U + start_upper >= length ||
        (uint32_t)23U + start_lower >= length ||
        (uint32_t)23U + start_unicode >= length ||
        !validate_ascii_records(&reader, 23U) ||
        !validate_ascii_records(&reader, 23U + start_upper) ||
        !validate_ascii_records(&reader, 23U + start_lower)) {
        return false;
    }

    unicode_table = 23U + start_unicode;
    if (!font_reader_u16(&reader, unicode_table, &lookup_offset) ||
        !font_reader_u16(&reader, unicode_table + 2U, &first_codepoint) ||
        (uint32_t)unicode_table + lookup_offset >= length) {
        return false;
    }

    /* The three generated slot resources use one Unicode block. Reject
     * unsupported multi-block input explicitly rather than validating only
     * the first block while the renderer can reach later blocks. */
    if (first_codepoint == 0U) return true;
    return first_codepoint == 0xFFFFU && lookup_offset == 4U &&
           validate_unicode_records(&reader, unicode_table + lookup_offset);
}

ret_code_t font_store_init(void)
{
    fm25q04b_jedec_id_t id;

    if (m_ready) {
        return NRF_SUCCESS;
    }
    if (fm25q04b_init() != NRF_SUCCESS || fm25q04b_probe(&id) != NRF_SUCCESS) {
        return NRF_ERROR_NOT_FOUND;
    }
    memset(m_records, 0, sizeof(m_records));
    if (resource_store_mount() == RS_OK) {
        (void)resource_font_pack_load();
        m_ready = 1U;
        font_store_update_sources();
        return NRF_SUCCESS;
    }
    uint8_t prefix[4];
    if (fm25q04b_read(0, prefix, sizeof(prefix)) != NRF_SUCCESS ||
        (get_u32(prefix) != UINT32_MAX && get_u32(prefix) != FONT_STORE_LEGACY_MAGIC))
        return NRF_ERROR_NOT_FOUND;
    m_data_base = FONT_STORE_DATA_BASE;
    manifest_load();
    m_ready = 1U;
    font_store_update_sources();
    return NRF_SUCCESS;
}

bool font_store_ready(void)
{
    return m_ready != 0U;
}

const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot)
{
    if (!m_ready || !slot_valid(slot) || m_records[slot].length == 0U) {
        return NULL;
    }
    return &m_sources[slot];
}

#if defined(FONT_STORE_HOST_TEST)
void font_store_host_reset(void)
{
    m_ready = 0U;
    m_generation = 0U;
    m_revision = 1U;
    m_manifest_address = FONT_STORE_NO_MANIFEST;
    m_data_base = FONT_STORE_DATA_BASE;
    memset(m_records, 0, sizeof(m_records));
    memset(m_regions, 0, sizeof(m_regions));
    memset(m_sources, 0, sizeof(m_sources));
}
#endif
