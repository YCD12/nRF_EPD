#ifndef FM25Q04B_H
#define FM25Q04B_H

#include <stddef.h>
#include <stdint.h>

#include "sdk_errors.h"

#define FM25Q04B_SIZE_BYTES        (512UL * 1024UL)
#define FM25Q04B_SECTOR_SIZE       4096UL
#define FM25Q04B_PAGE_SIZE         256UL

typedef struct {
    uint8_t manufacturer_id;
    uint8_t memory_type;
    uint8_t capacity;
} fm25q04b_jedec_id_t;

/* A bounded view of a resource stored in FM25Q04B.  The view is useful for
 * consumers such as u8g2 which should not need to know the physical Flash
 * address of a resource. */
typedef struct {
    uint32_t base_address;
    uint32_t length;
} fm25q04b_region_t;

ret_code_t fm25q04b_init(void);

ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t* id);
ret_code_t fm25q04b_read_jedec_id(fm25q04b_jedec_id_t* id);
ret_code_t fm25q04b_wake(void);
ret_code_t fm25q04b_sleep(void);

ret_code_t fm25q04b_read(uint32_t address, void* data, size_t length);
int fm25q04b_read_region(const void* context, uint32_t offset, uint8_t* data, size_t length);
ret_code_t fm25q04b_write(uint32_t address, const void* data, size_t length);
ret_code_t fm25q04b_erase_sector(uint32_t address);
ret_code_t fm25q04b_erase_range(uint32_t address, size_t length);

#endif
