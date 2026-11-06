#ifndef TEST_NRF_GPIO_H
#define TEST_NRF_GPIO_H

#include <stdint.h>

static inline void nrf_gpio_pin_write(uint32_t pin, uint32_t value)
{
    (void)pin;
    (void)value;
}

static inline uint32_t nrf_gpio_pin_read(uint32_t pin)
{
    (void)pin;
    return 0U;
}

#endif
