#include "persistent_record.h"
#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

uint32_t persistent_record_data_crc(const void *data, uint16_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    if (length != 0U && data == NULL) return 0U;
    return resource_crc32(crc, bytes, length) ^ UINT32_MAX;
}

static rs_result_t decode_header(const uint8_t header[PERSISTENT_RECORD_HEADER_SIZE],
                                 uint32_t magic, uint32_t package_length,
                                 uint16_t capacity, bool *tombstone,
                                 persistent_record_info_t *info)
{
    uint16_t length = get_u16(header + 8);
    uint16_t flags = get_u16(header + 10);
    if (resource_get_u32(header) != magic || get_u16(header + 4) != PERSISTENT_RECORD_SCHEMA ||
        get_u16(header + 6) != PERSISTENT_RECORD_HEADER_SIZE ||
        (flags & (uint16_t)~PERSISTENT_RECORD_FLAG_TOMBSTONE) != 0U ||
        package_length != PERSISTENT_RECORD_HEADER_SIZE + length || length > capacity ||
        ((flags & PERSISTENT_RECORD_FLAG_TOMBSTONE) != 0U && length != 0U))
        return RS_INCOMPATIBLE;
    info->length = length;
    info->data_crc = resource_get_u32(header + 12);
    *tombstone = (flags & PERSISTENT_RECORD_FLAG_TOMBSTONE) != 0U;
    return RS_OK;
}

rs_result_t persistent_record_load(uint16_t partition_id, uint32_t record_magic,
                                   void *data, uint16_t capacity,
                                   persistent_record_info_t *info)
{
    uint8_t header[PERSISTENT_RECORD_HEADER_SIZE];
    rs_package_info_t package;
    bool tombstone = false;
    if (info == NULL || (capacity != 0U && data == NULL)) return RS_INVALID;
    memset(info, 0, sizeof(*info));
    rs_result_t r = resource_store_mount();
    if (r == RS_OK) r = resource_store_open_info(partition_id, &package);
    if (r == RS_OK && package.length < sizeof(header)) r = RS_INCOMPATIBLE;
    if (r == RS_OK) r = resource_store_read(partition_id, 0, header, sizeof(header));
    if (r == RS_OK) {
        info->revision = package.revision;
        r = decode_header(header, record_magic, package.length, capacity, &tombstone, info);
    }
    if (r == RS_OK && tombstone) return RS_NOT_FOUND;
    if (r == RS_OK && info->length != 0U)
        r = resource_store_read(partition_id, sizeof(header), data, info->length);
    if (r == RS_OK && persistent_record_data_crc(data, info->length) != info->data_crc)
        r = RS_CRC;
    return r;
}

rs_result_t persistent_record_save(uint16_t partition_id, uint32_t record_magic,
                                   const void *data, uint16_t length, bool tombstone,
                                   persistent_record_info_t *info)
{
    uint8_t header[PERSISTENT_RECORD_HEADER_SIZE];
    rs_package_info_t old;
    uint32_t revision = 1U;
    uint32_t crc;
    rs_result_t r;
    if (info == NULL || (length != 0U && data == NULL) || (tombstone && length != 0U))
        return RS_INVALID;
    memset(info, 0, sizeof(*info));
    r = resource_store_mount();
    if (r != RS_OK) return r;
    r = resource_store_info(partition_id, &old);
    if (r == RS_OK) revision = old.revision == UINT32_MAX ? 1U : old.revision + 1U;
    else if (r != RS_NOT_FOUND && r != RS_CRC && r != RS_INCOMPATIBLE) return r;

    resource_put_u32(header, record_magic);
    put_u16(header + 4, PERSISTENT_RECORD_SCHEMA);
    put_u16(header + 6, PERSISTENT_RECORD_HEADER_SIZE);
    put_u16(header + 8, length);
    put_u16(header + 10, tombstone ? PERSISTENT_RECORD_FLAG_TOMBSTONE : 0U);
    info->data_crc = persistent_record_data_crc(data, length);
    resource_put_u32(header + 12, info->data_crc);
    crc = resource_crc32(UINT32_MAX, header, sizeof(header));
    crc = resource_crc32(crc, data, length) ^ UINT32_MAX;

    r = resource_store_begin(partition_id, sizeof(header) + length, crc, revision);
    if (r == RS_OK) r = resource_store_write(0, header, sizeof(header));
    if (r == RS_OK && length != 0U) r = resource_store_write(sizeof(header), data, length);
    if (r == RS_OK) r = resource_store_commit();
    if (r != RS_OK) resource_store_abort();
    else {
        info->revision = revision;
        info->length = length;
    }
    return r;
}
