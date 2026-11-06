#ifndef PERSISTENT_RECORD_H
#define PERSISTENT_RECORD_H

#include "resource_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PERSISTENT_RECORD_HEADER_SIZE 16U
#define PERSISTENT_RECORD_SCHEMA 1U
#define PERSISTENT_RECORD_FLAG_TOMBSTONE 0x0001U

typedef struct {
    uint32_t revision;
    uint32_t data_crc;
    uint16_t length;
} persistent_record_info_t;

/* The ERS header supplies atomic commit and package CRC. REC1 adds a typed,
 * fixed-width payload header and a business-data CRC used by save ACKs. */
rs_result_t persistent_record_load(uint16_t partition_id, uint32_t record_magic,
                                   void *data, uint16_t capacity,
                                   persistent_record_info_t *info);
rs_result_t persistent_record_save(uint16_t partition_id, uint32_t record_magic,
                                   const void *data, uint16_t length, bool tombstone,
                                   persistent_record_info_t *info);
uint32_t persistent_record_data_crc(const void *data, uint16_t length);

#endif
