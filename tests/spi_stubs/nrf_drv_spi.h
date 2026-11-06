#include "sdk_errors.h"
#include <stdint.h>
#include <stddef.h>
typedef unsigned nrf_drv_spi_t;
typedef struct {
    unsigned ss_pin, miso_pin, sck_pin, mosi_pin, frequency, mode, bit_order;
    uint8_t orc;
} nrf_drv_spi_config_t;
#define NRF_DRV_SPI_INSTANCE(n) (n)
#define NRF_DRV_SPI_DEFAULT_CONFIG { .orc = 0xFFU }
#define NRF_DRV_SPI_FREQ_8M 8000000U
#define NRF_DRV_SPI_MODE_0 0U
#define NRF_DRV_SPI_BIT_ORDER_MSB_FIRST 0U
ret_code_t nrf_drv_spi_init(const nrf_drv_spi_t*, const nrf_drv_spi_config_t*, void*, void*);
void nrf_drv_spi_uninit(const nrf_drv_spi_t*);
ret_code_t nrf_drv_spi_transfer(const nrf_drv_spi_t*, const uint8_t*, uint8_t, uint8_t*, uint8_t);
