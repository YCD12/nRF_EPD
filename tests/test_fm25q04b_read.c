#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fm25q04b.h"
#include "nrf_drv_spi.h"

static unsigned reads;
void app_feed_wdt(void) {}
ret_code_t nrf_drv_spi_init(const nrf_drv_spi_t* spi, const nrf_drv_spi_config_t* config, void* fn, void* ctx)
{
    (void)spi; (void)fn; (void)ctx;
    assert(config->orc == 0xFFU);
    return NRF_SUCCESS;
}
void nrf_drv_spi_uninit(const nrf_drv_spi_t* spi) { (void)spi; }
ret_code_t nrf_drv_spi_transfer(const nrf_drv_spi_t* spi, const uint8_t* tx, uint8_t tx_len, uint8_t* rx, uint8_t rx_len)
{
    (void)spi;
    if (rx_len) memset(rx, 0, rx_len);
    if (tx[0] == 3U) {
        assert(tx_len == 4U && rx_len >= 5U);
        uint32_t address = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3];
        for (unsigned i = 4U; i < rx_len; i++) rx[i] = (uint8_t)(address + i - 4U);
        reads++;
    }
    return NRF_SUCCESS;
}
int main(void)
{
    assert(fm25q04b_init() == NRF_SUCCESS);
    uint8_t bytes[503];
    assert(fm25q04b_read(0x12F8U, bytes, sizeof(bytes)) == NRF_SUCCESS);
    assert(reads == 3U); /* 251 + 251 + 1, including page/address crossings */
    for (unsigned i = 0U; i < sizeof(bytes); i++) assert(bytes[i] == (uint8_t)(0x12F8U + i));
    assert(fm25q04b_read(FM25Q04B_SIZE_BYTES - 2U, bytes, 3U) == NRF_ERROR_INVALID_ADDR);
    assert(fm25q04b_read(FM25Q04B_SIZE_BYTES, NULL, 0U) == NRF_SUCCESS);
    assert(reads == 3U);
    puts("PASS: SPI header-only TX, 251-byte RX chunks and address bounds");
    return 0;
}
