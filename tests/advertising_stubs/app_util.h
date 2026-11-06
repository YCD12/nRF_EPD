#ifndef TEST_ADVERTISING_APP_UTIL_H
#define TEST_ADVERTISING_APP_UTIL_H
#include <stdint.h>
typedef uint8_t uint16_le_t[2];
typedef struct { uint16_t size; uint8_t *p_data; } uint8_array_t;
static inline uint8_t uint16_encode(uint16_t value, uint8_t *out)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    return 2;
}
static inline uint16_t uint16_decode(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}
#endif
