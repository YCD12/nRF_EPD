#include "sdk_errors.h"
#include <stdint.h>
#include <stddef.h>

typedef unsigned nrfx_spim_t;
typedef struct {
    unsigned ss_pin, miso_pin, sck_pin, mosi_pin, frequency, mode, bit_order;
    uint8_t orc;
} nrfx_spim_config_t;
typedef struct {
    const uint8_t *p_tx_buffer;
    size_t tx_length;
    uint8_t *p_rx_buffer;
    size_t rx_length;
} nrfx_spim_xfer_desc_t;
#define NRFX_SPIM_INSTANCE(n) (n)
#define NRFX_SPIM_DEFAULT_CONFIG { .orc = 0xFFU }
#define NRF_SPIM_FREQ_8M 8000000U
#define NRF_SPIM_MODE_0 0U
#define NRF_SPIM_BIT_ORDER_MSB_FIRST 0U
#define NRFX_SPIM_XFER_TRX(tx, tn, rx, rn) {tx, tn, rx, rn}
ret_code_t nrfx_spim_init(const nrfx_spim_t*, const nrfx_spim_config_t*, void*, void*);
void nrfx_spim_uninit(const nrfx_spim_t*);
ret_code_t nrfx_spim_xfer(const nrfx_spim_t*, const nrfx_spim_xfer_desc_t*, uint32_t);
