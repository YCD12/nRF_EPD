#ifndef RESOURCE_STORE_H
#define RESOURCE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maintenance builds provide the new writer before resource consumers migrate.
 * They deliberately disable the legacy font allocator. Never auto-format. */
#ifndef RESOURCE_STORE_MAINTENANCE
#define RESOURCE_STORE_MAINTENANCE 0
#endif

#define RS_HEADER_SIZE 32U
#define RS_ENTRY_SIZE 16U
#define RS_MAX_PARTITIONS 32U
#define RS_MAGIC 0x31535245UL /* ERS1, explicitly little endian */
#define RS_COMMIT 0xA55AC33CUL
#define RS_ABI 1U

typedef enum {
    RS_OK, RS_INVALID, RS_BUSY, RS_CRC, RS_NOT_FOUND, RS_IO,
    RS_INCOMPATIBLE, RS_READ_ONLY
} rs_result_t;

typedef struct {
    uint16_t id, type, schema, flags;
    uint32_t offset, size;
} rs_partition_t;

typedef struct {
    uint32_t revision, length, crc32;
} rs_package_info_t;

/* All store APIs run in main context. Mount/read never write or format Flash. */
rs_result_t resource_store_mount(void);
rs_result_t resource_store_find(uint16_t id, rs_partition_t *partition);
rs_result_t resource_store_info(uint16_t id, rs_package_info_t *info);
rs_result_t resource_store_open(uint16_t id, uint32_t *length);
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length);
/* Table (ID 1) is repairable at the fixed locator even when mount fails.
 * Table updates require callers to enter exclusive maintenance mode first. */
rs_result_t resource_store_begin(uint16_t id, uint32_t length, uint32_t crc, uint32_t revision);
rs_result_t resource_store_write(uint32_t offset, const uint8_t *data, size_t length);
rs_result_t resource_store_commit(void);
void resource_store_abort(void);
uint16_t resource_store_active(void);
uint32_t resource_store_received(void);
uint32_t resource_crc32(uint32_t crc, const uint8_t *data, size_t length);
uint32_t resource_get_u32(const uint8_t *p);
void resource_put_u32(uint8_t *p, uint32_t value);

#endif
