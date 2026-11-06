#include "epd_rle.h"

#include <stddef.h>

bool epd_rle_decompress(const uint8_t *src, uint16_t src_len, uint8_t *dst,
                        uint16_t dst_capacity, uint16_t *out_len)
{
    uint16_t si = 0U;
    uint16_t di = 0U;

    if (dst == NULL || out_len == NULL) return false;
    if (src_len > 0U && src == NULL) return false;

    while (si < src_len) {
        uint8_t control = src[si++];

        if ((control & 0x80U) != 0U) {
            /* repeat run: control 0x80|(count-3)，后跟 value */
            uint16_t count = (uint16_t)((control & 0x7FU) + 3U);
            if (si >= src_len) return false;
            if ((uint32_t)di + count > dst_capacity) return false;
            uint8_t value = src[si++];
            for (uint16_t i = 0U; i < count; i++) dst[di++] = value;
        } else {
            /* literal run: control=(count-1)，后跟 count 字节原文 */
            uint16_t count = (uint16_t)control + 1U;
            if ((uint32_t)si + count > src_len) return false;
            if ((uint32_t)di + count > dst_capacity) return false;
            for (uint16_t i = 0U; i < count; i++) dst[di++] = src[si++];
        }
    }

    *out_len = di;
    return true;
}
