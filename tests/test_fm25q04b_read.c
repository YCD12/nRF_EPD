#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fm25q04b.h"
#include "nrfx_spim.h"
#include <stdbool.h>

static unsigned reads, programs, feeds, busy_reads;
static uint32_t waited;
static bool enabled, stuck;
static uint8_t programmed[1024];
void app_feed_wdt(void) { feeds++; }
void test_delay_us(uint32_t n) { waited += n; }
ret_code_t nrfx_spim_init(const nrfx_spim_t* spi, const nrfx_spim_config_t* config, void* fn, void* ctx)
{
    (void)spi; (void)fn; (void)ctx;
    assert(config->orc == 0xFFU);
    assert(*spi == 1 && config->frequency == NRF_SPIM_FREQ_8M);
    return NRF_SUCCESS;
}
void nrfx_spim_uninit(const nrfx_spim_t* spi) { (void)spi; }
ret_code_t nrfx_spim_xfer(const nrfx_spim_t* spi, const nrfx_spim_xfer_desc_t* x, uint32_t flags)
{
    (void)spi; assert(flags == 0);
    const uint8_t *tx = x->p_tx_buffer;
    uint8_t *rx = x->p_rx_buffer;
    size_t tx_len = x->tx_length, rx_len = x->rx_length;
    if (rx_len) memset(rx, 0, rx_len);
    if (tx[0] == 3U) {
        assert(tx_len == 4U && rx_len >= 5U);
        uint32_t address = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3];
        for (unsigned i = 4U; i < rx_len; i++) rx[i] = (uint8_t)(address + i - 4U);
        reads++;
    }
    if (tx[0] == 6U) enabled = true;
    if (tx[0] == 5U) {
        rx[1] = enabled ? 2U : 0U;
        if (stuck || busy_reads) { rx[1] |= 1U; if (busy_reads) busy_reads--; }
    }
    if (tx[0] == 2U) {
        uint32_t address = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3];
        assert(enabled && tx_len > 4 && tx_len <= 260 && (address % 256) + tx_len - 4 <= 256);
        assert(address + tx_len - 4 <= sizeof(programmed));
        memcpy(programmed + address, tx + 4, tx_len - 4);
        programs++; enabled = false; busy_reads = 2;
    }
    return NRF_SUCCESS;
}
int main(void)
{
    assert(fm25q04b_init() == NRF_SUCCESS);
    uint8_t bytes[520];
    assert(fm25q04b_read(0x12F8U, bytes, sizeof(bytes)) == NRF_SUCCESS);
    assert(reads == 3U); /* 256 + 256 + 8, including page/address crossings */
    for (unsigned i = 0U; i < sizeof(bytes); i++) assert(bytes[i] == (uint8_t)(0x12F8U + i));
    assert(fm25q04b_read(FM25Q04B_SIZE_BYTES - 2U, bytes, 3U) == NRF_ERROR_INVALID_ADDR);
    assert(fm25q04b_read(FM25Q04B_SIZE_BYTES, NULL, 0U) == NRF_SUCCESS);
    assert(reads == 3U);
    waited = 0;
    assert(fm25q04b_write(0, bytes, 256) == NRF_SUCCESS);
    assert(programs == 1 && !memcmp(programmed, bytes, 256));
    assert(waited == 150); /* 50 + 100 us rather than two 1 ms polls. */
    assert(fm25q04b_write(250, bytes, sizeof(bytes)) == NRF_SUCCESS);
    assert(programs == 5 && !memcmp(programmed + 250, bytes, sizeof(bytes)));
    waited = feeds = 0; stuck = true;
    assert(fm25q04b_write(0, bytes, 1) == NRF_ERROR_TIMEOUT);
    assert(waited == 1000000 && feeds > 0);
    puts("PASS: full 260-byte SPIM page transactions, boundary splits, polling and timeout");
    return 0;
}
