#ifndef FONT_STORE_H
#define FONT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fm25q04b.h"
#include "sdk_errors.h"
#include "u8g2_font.h"

#define FONT_STORE_SLOT_COUNT 3U
#define FONT_STORE_DATA_BASE 0x01000UL
/* FST3 primary records live in sector zero; secondary lives in the last
 * sector. Data base stays unchanged so existing FST2 packages remain readable. */
#define FONT_STORE_PRIMARY_ADDR 0x00040UL
#define FONT_STORE_SECONDARY_ADDR (FM25Q04B_SIZE_BYTES - FM25Q04B_SECTOR_SIZE)
#define FONT_STORE_DATA_CAPACITY (FONT_STORE_SECONDARY_ADDR - FONT_STORE_DATA_BASE)

typedef enum {
    FONT_STORE_SLOT_WQY9 = 0,
    FONT_STORE_SLOT_WQY12,
    FONT_STORE_SLOT_HELVB18,
} font_store_slot_t;

typedef enum {
    FONT_STORE_OK = 0,
    FONT_STORE_INVALID = 1,
    FONT_STORE_BUSY = 2,
    FONT_STORE_CRC = 3,
    FONT_STORE_NOT_FOUND = 4,
    FONT_STORE_IO = 5,
    FONT_STORE_NO_SPACE = 6,
} font_store_result_t;

typedef struct {
    uint8_t present;
    uint32_t length;
    uint32_t crc32;
    uint32_t revision;
} font_store_info_t;

typedef struct {
    uint32_t length;
    uint32_t crc32;
} font_store_pack_slot_t;

ret_code_t font_store_init(void);
bool font_store_ready(void);
bool font_store_update_active(void);

const u8g2_font_source_t* font_store_get_source(font_store_slot_t slot);
bool font_store_get_info(font_store_slot_t slot, font_store_info_t* info);

/* A transaction contains all three fonts concatenated in slot order.  A zero
 * length means that the corresponding slot is intentionally absent. */
font_store_result_t font_store_begin_pack(uint32_t total_length,
                                          uint32_t pack_crc32,
                                          const font_store_pack_slot_t* slots);
font_store_result_t font_store_write_pack_chunk(uint32_t offset,
                                                const uint8_t* data,
                                                size_t length);
font_store_result_t font_store_commit_pack(void);
font_store_result_t font_store_abort_update(void);

/* CRC32/IEEE, reflected polynomial, init/xorout 0xFFFFFFFF. */
uint32_t font_store_crc32(const uint8_t* data, size_t length);

#if defined(FONT_STORE_HOST_TEST)
/* Test-only runtime reset; Flash contents are deliberately preserved. */
void font_store_host_reset(void);
#endif

#endif
