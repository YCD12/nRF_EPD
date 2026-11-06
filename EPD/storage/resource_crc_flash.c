#include "resource_store.h"
#include "fm25q04b.h"
#include "main.h"

rs_result_t resource_crc32_flash(uint32_t address, uint32_t length, uint32_t expected)
{
    uint8_t bytes[FM25Q04B_PAGE_SIZE];
    uint32_t crc = UINT32_MAX;
    if (address > FM25Q04B_SIZE_BYTES || length > FM25Q04B_SIZE_BYTES - address)
        return RS_INVALID;
    while (length) {
        size_t n = length > sizeof(bytes) ? sizeof(bytes) : length;
        if (fm25q04b_read(address, bytes, n) != NRF_SUCCESS) return RS_IO;
        crc = resource_crc32(crc, bytes, n);
        address += n;
        length -= n;
        app_feed_wdt();
    }
    return (crc ^ UINT32_MAX) == expected ? RS_OK : RS_CRC;
}
