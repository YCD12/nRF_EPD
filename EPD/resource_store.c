#include "resource_store.h"
#include "resource_layout.h"
#include "fm25q04b.h"
#include <string.h>

/* Only metadata is cached; payloads remain on SPI Flash. */
static uint16_t m_count;
static struct {
    rs_partition_t partition;
    uint32_t length;
    uint16_t id;
    bool valid;
} m_read_cache;
static struct {
    rs_partition_t partition;
    uint32_t length, crc, revision, received, erased_end;
    uint16_t active;
    bool failed;
} m_update;

uint32_t resource_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void resource_put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4U; ++i) p[i] = (uint8_t)(v >> (8U * i));
}

uint32_t resource_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8U; ++i)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc;
}

static rs_result_t read_flash(uint32_t address, void *data, size_t length)
{
    return fm25q04b_read(address, data, length) == NRF_SUCCESS ? RS_OK : RS_IO;
}

static rs_result_t crc_flash(uint32_t address, uint32_t length, uint32_t expected)
{
    uint8_t bytes[64];
    uint32_t crc = UINT32_MAX;
    while (length) {
        size_t n = length > sizeof(bytes) ? sizeof(bytes) : length;
        if (read_flash(address, bytes, n) != RS_OK) return RS_IO;
        crc = resource_crc32(crc, bytes, n);
        address += n;
        length -= n;
    }
    return (crc ^ UINT32_MAX) == expected ? RS_OK : RS_CRC;
}

static rs_result_t entry_read(unsigned index, rs_partition_t *p)
{
    uint8_t b[RS_ENTRY_SIZE];
    rs_result_t r = read_flash(RS_HEADER_SIZE + index * RS_ENTRY_SIZE, b, sizeof(b));
    if (r != RS_OK) return r;
    p->id = b[0] | ((uint16_t)b[1] << 8);
    p->type = b[2] | ((uint16_t)b[3] << 8);
    p->offset = resource_get_u32(b + 4);
    p->size = resource_get_u32(b + 8);
    p->schema = b[12] | ((uint16_t)b[13] << 8);
    p->flags = b[14] | ((uint16_t)b[15] << 8);
    return RS_OK;
}

static rs_result_t table_validate(uint32_t length)
{
    if (!length || length % RS_ENTRY_SIZE || length / RS_ENTRY_SIZE > RS_MAX_PARTITIONS)
        return RS_INVALID;
    bool locator = false;
    for (unsigned i = 0; i < length / RS_ENTRY_SIZE; ++i) {
        rs_partition_t p, q;
        if (entry_read(i, &p) != RS_OK) return RS_IO;
        if (!p.id || !p.type || p.schema != RS_ABI || p.flags || !p.size ||
            p.offset % RS_SECTOR_SIZE || p.size % RS_SECTOR_SIZE ||
            p.offset >= RS_CAPACITY || p.size > RS_CAPACITY - p.offset)
            return RS_INVALID;
        if (p.id == RS_PART_PARTITION_TABLE) {
            if (p.type != RS_PART_PARTITION_TABLE || p.offset || p.size != RS_SECTOR_SIZE)
                return RS_INVALID;
            locator = true;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (entry_read(j, &q) != RS_OK) return RS_IO;
            if (p.id == q.id || (p.offset < q.offset + q.size && q.offset < p.offset + p.size))
                return RS_INVALID;
        }
    }
    return locator ? RS_OK : RS_INVALID;
}

static rs_result_t header_read(const rs_partition_t *p, uint8_t *b)
{
    if (read_flash(p->offset, b, RS_HEADER_SIZE) != RS_OK) return RS_IO;
    if (resource_get_u32(b) != RS_MAGIC || resource_get_u32(b + 28) != RS_COMMIT)
        return RS_NOT_FOUND;
    if (resource_get_u32(b + 4) != ((uint32_t)RS_ABI << 16 | p->id) ||
        resource_get_u32(b + 20) != RS_CAPACITY)
        return RS_INCOMPATIBLE;
    if ((resource_crc32(UINT32_MAX, b, 24) ^ UINT32_MAX) != resource_get_u32(b + 24))
        return RS_CRC;
    uint32_t n = resource_get_u32(b + 12);
    if (!n || n > p->size - RS_HEADER_SIZE) return RS_INVALID;
    return RS_OK;
}

static rs_result_t package_header(uint16_t id, rs_partition_t *partition, uint8_t *header)
{
    rs_result_t r = resource_store_find(id, partition);
    return r == RS_OK ? header_read(partition, header) : r;
}

rs_result_t resource_store_mount(void)
{
    uint8_t b[RS_HEADER_SIZE];
    rs_partition_t table = {1, 1, RS_ABI, 0, 0, RS_SECTOR_SIZE};
    if (m_update.active) return RS_BUSY;
    m_count = 0;
    m_read_cache.valid = false;
    rs_result_t r = header_read(&table, b);
    if (r == RS_OK) r = crc_flash(RS_HEADER_SIZE, resource_get_u32(b + 12), resource_get_u32(b + 16));
    if (r == RS_OK) r = table_validate(resource_get_u32(b + 12));
    if (r == RS_OK) m_count = resource_get_u32(b + 12) / RS_ENTRY_SIZE;
    return r;
}

rs_result_t resource_store_find(uint16_t id, rs_partition_t *p)
{
    if (!p) return RS_INVALID;
    for (unsigned i = 0; i < m_count; ++i) {
        if (entry_read(i, p) != RS_OK) return RS_IO;
        if (p->id == id) return RS_OK;
    }
    return RS_NOT_FOUND;
}

rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    rs_partition_t p;
    uint8_t header[RS_HEADER_SIZE];
    if (!length) return RS_INVALID;
    *length = 0;
    /* Never retain a previously verified view when this validation fails. */
    m_read_cache.valid = false;
    if (m_update.active == id) return RS_BUSY;
    rs_result_t r = package_header(id, &p, header);
    if (r == RS_OK) {
        uint32_t payload_length = resource_get_u32(header + 12);
        r = crc_flash(p.offset + RS_HEADER_SIZE, payload_length, resource_get_u32(header + 16));
        if (r == RS_OK) {
            m_read_cache.partition = p;
            m_read_cache.length = payload_length;
            m_read_cache.id = id;
            m_read_cache.valid = true;
            *length = payload_length;
        }
    }
    return r;
}

rs_result_t resource_store_info(uint16_t id, rs_package_info_t *info)
{
    rs_partition_t p;
    uint8_t b[RS_HEADER_SIZE];
    if (!info) return RS_INVALID;
    memset(info, 0, sizeof(*info));
    if (m_update.active == id) return RS_BUSY;
    rs_result_t r = package_header(id, &p, b);
    if (r == RS_OK) {
        info->revision = resource_get_u32(b + 8);
        info->length = resource_get_u32(b + 12);
        info->crc32 = resource_get_u32(b + 16);
    }
    return r;
}

rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    rs_partition_t p;
    uint8_t b[RS_HEADER_SIZE];
    if ((!data && length) || m_update.active == id) return RS_INVALID;
    if (m_read_cache.valid && m_read_cache.id == id) {
        if (offset > m_read_cache.length || length > m_read_cache.length - offset)
            return RS_INVALID;
        return read_flash(m_read_cache.partition.offset + RS_HEADER_SIZE + offset,
                          data, length);
    }
    rs_result_t r = package_header(id, &p, b);
    if (r != RS_OK) return r;
    uint32_t n = resource_get_u32(b + 12);
    if (offset > n || length > n - offset) return RS_INVALID;
    /* Consumers must open (full CRC) once before reading a package. */
    return read_flash(p.offset + RS_HEADER_SIZE + offset, data, length);
}

static rs_result_t fail_io(void)
{
    m_update.failed = true;
    return RS_IO;
}

rs_result_t resource_store_begin(uint16_t id, uint32_t length, uint32_t crc, uint32_t revision)
{
    rs_partition_t p;
    uint8_t invalid[4] = {0}, check[4];
    if (m_update.active) return RS_BUSY;
    if (id == RS_PART_PARTITION_TABLE) {
        p = (rs_partition_t){1, 1, RS_ABI, 0, 0, RS_SECTOR_SIZE};
        if (!length || length % RS_ENTRY_SIZE || length / RS_ENTRY_SIZE > RS_MAX_PARTITIONS)
            return RS_INVALID;
    } else {
        /* Revalidate the table immediately before any destructive operation. */
        rs_result_t r = resource_store_mount();
        if (r == RS_OK) r = resource_store_find(id, &p);
        if (r != RS_OK) return r;
        if (p.type == RS_PART_RESERVED || p.id == RS_PART_RESOURCE_UPDATE_STATUS)
            return RS_WRITE_PROTECTED;
    }
    if (!length || length > p.size - RS_HEADER_SIZE || p.schema != RS_ABI) return RS_INVALID;
    memset(&m_update, 0, sizeof(m_update));
    m_update.partition = p;
    m_update.length = length;
    m_update.crc = crc;
    m_update.revision = revision;
    m_update.active = id; /* Own the target even if a subsequent write fails. */
    if (id == RS_PART_PARTITION_TABLE) m_count = 0;
    /* Invalidate BEFORE erasing anything. Readback makes ambiguous errors fail closed. */
    if (fm25q04b_write(p.offset + 28, invalid, 4) != NRF_SUCCESS ||
        read_flash(p.offset + 28, check, 4) != RS_OK || memcmp(invalid, check, 4) ||
        fm25q04b_erase_sector(p.offset) != NRF_SUCCESS) return fail_io();
    m_update.erased_end = p.offset + RS_SECTOR_SIZE;
    return RS_OK;
}

rs_result_t resource_store_write(uint32_t offset, const uint8_t *data, size_t length)
{
    if (!m_update.active || m_update.failed || !data || !length ||
        offset > m_update.length || length > m_update.length - offset) return RS_INVALID;
    uint32_t address = m_update.partition.offset + RS_HEADER_SIZE + offset;
    if (offset != m_update.received) {
        /* A lost ACK may cause a retransmit. Accept only identical durable bytes. */
        uint8_t check[20];
        if (offset >= m_update.received || length > m_update.received - offset || length > sizeof(check))
            return RS_INVALID;
        if (read_flash(address, check, length) != RS_OK) return fail_io();
        return memcmp(data, check, length) ? RS_INVALID : RS_OK;
    }
    while (address + length > m_update.erased_end) {
        if (fm25q04b_erase_sector(m_update.erased_end) != NRF_SUCCESS) return fail_io();
        m_update.erased_end += RS_SECTOR_SIZE;
    }
    if (fm25q04b_write(address, data, length) != NRF_SUCCESS) return fail_io();
    m_update.received += length;
    return RS_OK;
}

rs_result_t resource_store_commit(void)
{
    uint8_t b[RS_HEADER_SIZE], check[RS_HEADER_SIZE];
    if (!m_update.active || m_update.failed || m_update.received != m_update.length) return RS_INVALID;
    uint32_t base = m_update.partition.offset;
    rs_result_t r = crc_flash(base + RS_HEADER_SIZE, m_update.length, m_update.crc);
    if (r == RS_OK && m_update.active == RS_PART_PARTITION_TABLE) r = table_validate(m_update.length);
    if (r != RS_OK) return r;
    resource_put_u32(b, RS_MAGIC);
    resource_put_u32(b + 4, ((uint32_t)RS_ABI << 16) | m_update.active);
    resource_put_u32(b + 8, m_update.revision);
    resource_put_u32(b + 12, m_update.length);
    resource_put_u32(b + 16, m_update.crc);
    resource_put_u32(b + 20, RS_CAPACITY);
    resource_put_u32(b + 24, resource_crc32(UINT32_MAX, b, 24) ^ UINT32_MAX);
    resource_put_u32(b + 28, UINT32_MAX);
    if (fm25q04b_write(base, b, sizeof(b)) != NRF_SUCCESS ||
        read_flash(base, check, sizeof(check)) != RS_OK || memcmp(b, check, sizeof(b))) return fail_io();
    resource_put_u32(b + 28, RS_COMMIT);
    if (fm25q04b_write(base + 28, b + 28, 4) != NRF_SUCCESS ||
        read_flash(base, check, sizeof(check)) != RS_OK || memcmp(b, check, sizeof(b))) return fail_io();
    bool table = m_update.active == RS_PART_PARTITION_TABLE;
    resource_store_abort();
    return table ? resource_store_mount() : RS_OK;
}

void resource_store_abort(void)
{
    memset(&m_update, 0, sizeof(m_update));
    m_read_cache.valid = false;
}
uint16_t resource_store_active(void) { return m_update.active; }
uint32_t resource_store_received(void) { return m_update.received; }
