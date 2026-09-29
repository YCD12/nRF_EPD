#include "resource_store.h"

uint32_t resource_crc32(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc;
}
