#include <stdint.h>
static inline void nrf_delay_ms(uint32_t n) { (void)n; }
void test_delay_us(uint32_t n);
static inline void nrf_delay_us(uint32_t n) { test_delay_us(n); }
