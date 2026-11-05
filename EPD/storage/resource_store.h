#ifndef RESOURCE_STORE_H
#define RESOURCE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS_HEADER_SIZE 32U
#define RS_ENTRY_SIZE 16U
#define RS_MAX_PARTITIONS 32U
#define RS_MAGIC 0x31535245UL /* ERS1, explicitly little endian */
#define RS_COMMIT 0xA55AC33CUL
#define RS_ABI 1U

typedef enum {
    RS_OK, RS_INVALID, RS_BUSY, RS_CRC, RS_NOT_FOUND, RS_IO,
    RS_INCOMPATIBLE, RS_WRITE_PROTECTED
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
/* Verify the complete payload and return metadata from the same header read.
 * With non-NULL info, failures clear it and invalidate the previous read cache. */
rs_result_t resource_store_open_info(uint16_t id, rs_package_info_t *info);
rs_result_t resource_store_open(uint16_t id, uint32_t *length);
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length);
/* Main-context rendering transaction: retry an unavailable table once per frame.
 * Failed payload reads, transient mount/open failures or active updates prevent
 * frame commit. Missing packages permit NO DATA. */
void resource_store_render_begin(void);
bool resource_store_render_valid(void);
/* Table (ID 1) is repairable at the fixed locator even when mount fails.
 * Updates require callers to enter an exclusive resource-write session first. */
rs_result_t resource_store_begin(uint16_t id, uint32_t length, uint32_t crc, uint32_t revision);
/* Erase one aligned 4 KiB sector and verify it. ID 0 addresses the whole chip;
 * other IDs use a relative offset in the freshly validated partition. */
rs_result_t resource_store_erase(uint16_t id, uint32_t offset);
rs_result_t resource_store_write(uint32_t offset, const uint8_t *data, size_t length);
/* Buffered DATA reports accepted bytes separately; full/final pages are durable. */
rs_result_t resource_store_buffered_write(uint32_t offset, const uint8_t *data, size_t length);
rs_result_t resource_store_commit(void);
void resource_store_abort(void);
uint16_t resource_store_active(void);
uint32_t resource_store_received(void);
uint32_t resource_store_accepted(void);
uint32_t resource_crc32(uint32_t crc, const uint8_t *data, size_t length);
/* Validate a physical Flash range using a bounded page-sized scratch buffer. */
rs_result_t resource_crc32_flash(uint32_t address, uint32_t length, uint32_t expected);
uint32_t resource_get_u32(const uint8_t *p);
void resource_put_u32(uint8_t *p, uint32_t value);

#endif
