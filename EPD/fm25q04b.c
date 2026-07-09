#include "fm25q04b.h"

#include <stdbool.h>
#include <string.h>

#include "nrf_assert.h"
#include "nrf_delay.h"
#include "nrf_drv_spi.h"
#include "nrf_log.h"
#include "main.h"

#define FM25Q04B_SPI_INSTANCE         1
#define FM25Q04B_CS_PIN               12
#define FM25Q04B_MISO_PIN             13
#define FM25Q04B_SCK_PIN              14
#define FM25Q04B_MOSI_PIN             15

#define FM25Q04B_CMD_WRITE_ENABLE     0x06
#define FM25Q04B_CMD_WRITE_DISABLE    0x04
#define FM25Q04B_CMD_WRITE_STATUS_1   0x01
#define FM25Q04B_CMD_WRITE_STATUS_2   0x31
#define FM25Q04B_CMD_READ_STATUS_1    0x05
#define FM25Q04B_CMD_READ_STATUS_2    0x35
#define FM25Q04B_CMD_READ_DATA        0x03
#define FM25Q04B_CMD_PAGE_PROGRAM     0x02
#define FM25Q04B_CMD_SECTOR_ERASE_4K  0x20
#define FM25Q04B_CMD_READ_JEDEC_ID    0x9F
#define FM25Q04B_CMD_DEEP_POWER_DOWN  0xB9
#define FM25Q04B_CMD_RELEASE_POWERDN  0xAB

#define FM25Q04B_STATUS_WIP_MASK      0x01
#define FM25Q04B_STATUS_WEL_MASK      0x02
#define FM25Q04B_STATUS1_PROTECT_MASK 0xFC
#define FM25Q04B_STATUS2_PROTECT_MASK 0x40
#define FM25Q04B_STATUS2_SRP1_MASK    0x01

/*
 * Maximum data payload per SPI transfer. nrf_drv_spi_transfer takes uint8_t
 * lengths (max 255); the read/write header is always 4 bytes, so usable
 * payload = 255 - 4 = 251 bytes. Exceeding this silently truncates the length
 * to its low byte and returns NRF_SUCCESS with unread/unwritten data.
 * ASSERT in fm25q04b_read guards this; fm25q04b_write caps chunk to this value.
 */
#define FM25Q04B_SPI_PAYLOAD_MAX      128U
#define FM25Q04B_READ_CHUNK_MAX       FM25Q04B_SPI_PAYLOAD_MAX
#define FM25Q04B_ERASE_TIMEOUT_MS     5000U
#define FM25Q04B_PROGRAM_TIMEOUT_MS   1000U

static const nrf_drv_spi_t m_fm25q04b_spi = NRF_DRV_SPI_INSTANCE(FM25Q04B_SPI_INSTANCE);
static bool m_fm25q04b_initialized;
static bool m_fm25q04b_sleeping;

static ret_code_t fm25q04b_read_status(uint8_t* status);
static ret_code_t fm25q04b_wait_ready(uint32_t timeout_ms);
static ret_code_t fm25q04b_release_powerdown(void);

static bool fm25q04b_range_is_valid(uint32_t address, size_t length) {
    if (length == 0) {
        return true;
    }
    if (address >= FM25Q04B_SIZE_BYTES) {
        return false;
    }
    return length <= (size_t)(FM25Q04B_SIZE_BYTES - address);
}

static ret_code_t fm25q04b_transfer_raw(const uint8_t* tx_data,
                                        size_t tx_len,
                                        uint8_t* rx_data,
                                        size_t rx_len) {
    if (!m_fm25q04b_initialized) {
        return NRF_ERROR_INVALID_STATE;
    }
    return nrf_drv_spi_transfer(&m_fm25q04b_spi, tx_data, tx_len, rx_data, rx_len);
}

static ret_code_t fm25q04b_transfer(const uint8_t* tx_data,
                                    size_t tx_len,
                                    uint8_t* rx_data,
                                    size_t rx_len) {
    ret_code_t ret;

    if (!m_fm25q04b_initialized) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (m_fm25q04b_sleeping) {
        ret = fm25q04b_wake();
        if (ret != NRF_SUCCESS) {
            return ret;
        }
    }
    return fm25q04b_transfer_raw(tx_data, tx_len, rx_data, rx_len);
}

static void fm25q04b_encode_address(uint8_t* out, uint32_t address) {
    out[0] = (uint8_t)(address >> 16);
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)address;
}

static ret_code_t fm25q04b_write_enable(void) {
    uint8_t cmd = FM25Q04B_CMD_WRITE_ENABLE;
    uint8_t status = 0;
    ret_code_t ret = fm25q04b_transfer(&cmd, sizeof(cmd), NULL, 0);

    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_read_status(&status);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    return (status & FM25Q04B_STATUS_WEL_MASK) ? NRF_SUCCESS : NRF_ERROR_INTERNAL;
}

static void fm25q04b_write_disable(void) {
    uint8_t cmd = FM25Q04B_CMD_WRITE_DISABLE;
    (void)fm25q04b_transfer(&cmd, sizeof(cmd), NULL, 0);
}

static ret_code_t fm25q04b_read_status2(uint8_t* status) {
    uint8_t tx_data[2] = {FM25Q04B_CMD_READ_STATUS_2, 0xFF};
    uint8_t rx_data[2] = {0};
    ret_code_t ret;

    if (status == NULL) {
        return NRF_ERROR_NULL;
    }

    ret = fm25q04b_transfer(tx_data, sizeof(tx_data), rx_data, sizeof(rx_data));
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    *status = rx_data[1];
    return NRF_SUCCESS;
}

static ret_code_t fm25q04b_write_status(uint8_t cmd, uint8_t status) {
    uint8_t tx_data[2] = {cmd, status};
    ret_code_t ret;

    ret = fm25q04b_write_enable();
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_transfer(tx_data, sizeof(tx_data), NULL, 0);
    if (ret != NRF_SUCCESS) {
        fm25q04b_write_disable();
        return ret;
    }

    return fm25q04b_wait_ready(FM25Q04B_PROGRAM_TIMEOUT_MS);
}

static ret_code_t fm25q04b_clear_block_protection(void) {
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    ret_code_t ret = fm25q04b_read_status(&status1);

    if (ret != NRF_SUCCESS) {
        return ret;
    }
    ret = fm25q04b_read_status2(&status2);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    if ((status1 & FM25Q04B_STATUS1_PROTECT_MASK) == 0 &&
        (status2 & (FM25Q04B_STATUS2_PROTECT_MASK | FM25Q04B_STATUS2_SRP1_MASK)) == 0) {
        return NRF_SUCCESS;
    }

    NRF_LOG_INFO("FM25Q04B protected status: SR1=0x%02x SR2=0x%02x, clearing", status1, status2);
    ret = fm25q04b_write_status(FM25Q04B_CMD_WRITE_STATUS_1, 0x00);
    if (ret != NRF_SUCCESS) {
        return ret;
    }
    ret = fm25q04b_write_status(FM25Q04B_CMD_WRITE_STATUS_2, 0x00);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_read_status(&status1);
    if (ret != NRF_SUCCESS) {
        return ret;
    }
    ret = fm25q04b_read_status2(&status2);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    return ((status1 & FM25Q04B_STATUS1_PROTECT_MASK) ||
            (status2 & (FM25Q04B_STATUS2_PROTECT_MASK | FM25Q04B_STATUS2_SRP1_MASK))) ?
        NRF_ERROR_INVALID_STATE : NRF_SUCCESS;
}

static ret_code_t fm25q04b_page_program(uint32_t address, const uint8_t* data, size_t length) {
    uint8_t tx_data[4 + FM25Q04B_PAGE_SIZE];
    ret_code_t ret;

    if (length == 0) {
        return NRF_SUCCESS;
    }
    if (length > FM25Q04B_SPI_PAYLOAD_MAX ||
        length > FM25Q04B_PAGE_SIZE ||
        (address % FM25Q04B_PAGE_SIZE) + length > FM25Q04B_PAGE_SIZE) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    tx_data[0] = FM25Q04B_CMD_PAGE_PROGRAM;
    fm25q04b_encode_address(&tx_data[1], address);
    memcpy(&tx_data[4], data, length);

    ret = fm25q04b_write_enable();
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_transfer(tx_data, 4 + length, NULL, 0);
    if (ret != NRF_SUCCESS) {
        fm25q04b_write_disable();
        return ret;
    }

    return fm25q04b_wait_ready(FM25Q04B_PROGRAM_TIMEOUT_MS);
}

ret_code_t fm25q04b_init(void) {
    nrf_drv_spi_config_t spi_config = NRF_DRV_SPI_DEFAULT_CONFIG;
    ret_code_t ret;

    if (m_fm25q04b_initialized) {
        return NRF_SUCCESS;
    }

    spi_config.ss_pin = FM25Q04B_CS_PIN;
    spi_config.miso_pin = FM25Q04B_MISO_PIN;
    spi_config.sck_pin = FM25Q04B_SCK_PIN;
    spi_config.mosi_pin = FM25Q04B_MOSI_PIN;
    spi_config.frequency = NRF_DRV_SPI_FREQ_4M;
    spi_config.mode = NRF_DRV_SPI_MODE_0;
    spi_config.bit_order = NRF_DRV_SPI_BIT_ORDER_MSB_FIRST;

    ret = nrf_drv_spi_init(&m_fm25q04b_spi, &spi_config, NULL, NULL);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    m_fm25q04b_initialized = true;
    m_fm25q04b_sleeping = false;
    ret = fm25q04b_release_powerdown();
    if (ret != NRF_SUCCESS) {
        nrf_drv_spi_uninit(&m_fm25q04b_spi);
        m_fm25q04b_initialized = false;
        m_fm25q04b_sleeping = false;
        return ret;
    }

    ret = fm25q04b_clear_block_protection();
    if (ret != NRF_SUCCESS) {
        nrf_drv_spi_uninit(&m_fm25q04b_spi);
        m_fm25q04b_initialized = false;
        m_fm25q04b_sleeping = false;
        return ret;
    }

    return ret;
}

ret_code_t fm25q04b_wake(void) {
    if (!m_fm25q04b_initialized) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (!m_fm25q04b_sleeping) {
        return NRF_SUCCESS;
    }

    return fm25q04b_release_powerdown();
}

ret_code_t fm25q04b_sleep(void) {
    uint8_t cmd = FM25Q04B_CMD_DEEP_POWER_DOWN;
    ret_code_t ret;

    if (!m_fm25q04b_initialized) {
        return NRF_SUCCESS;
    }
    if (m_fm25q04b_sleeping) {
        return NRF_SUCCESS;
    }

    ret = fm25q04b_wait_ready(FM25Q04B_PROGRAM_TIMEOUT_MS);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_transfer_raw(&cmd, sizeof(cmd), NULL, 0);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    nrf_delay_us(5);
    m_fm25q04b_sleeping = true;
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t* id) {
    fm25q04b_jedec_id_t local_id;
    ret_code_t ret = fm25q04b_read_jedec_id(&local_id);

    if (ret != NRF_SUCCESS) {
        return ret;
    }
    if (id != NULL) {
        *id = local_id;
    }

    if ((local_id.manufacturer_id == 0x00 && local_id.memory_type == 0x00 && local_id.capacity == 0x00) ||
        (local_id.manufacturer_id == 0xFF && local_id.memory_type == 0xFF && local_id.capacity == 0xFF)) {
        ret = fm25q04b_release_powerdown();
        if (ret != NRF_SUCCESS) {
            return ret;
        }
        ret = fm25q04b_read_jedec_id(&local_id);
        if (ret != NRF_SUCCESS) {
            return ret;
        }
        if (id != NULL) {
            *id = local_id;
        }
    }

    if ((local_id.manufacturer_id == 0x00 && local_id.memory_type == 0x00 && local_id.capacity == 0x00) ||
        (local_id.manufacturer_id == 0xFF && local_id.memory_type == 0xFF && local_id.capacity == 0xFF)) {
        return NRF_ERROR_NOT_FOUND;
    }

    return NRF_SUCCESS;
}

static ret_code_t fm25q04b_release_powerdown(void) {
    uint8_t cmd = FM25Q04B_CMD_RELEASE_POWERDN;
    ret_code_t ret;

    if (!m_fm25q04b_initialized) {
        return NRF_ERROR_INVALID_STATE;
    }

    ret = fm25q04b_transfer_raw(&cmd, sizeof(cmd), NULL, 0);
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    nrf_delay_us(50);
    m_fm25q04b_sleeping = false;
    return NRF_SUCCESS;
}

ret_code_t fm25q04b_read_jedec_id(fm25q04b_jedec_id_t* id) {
    uint8_t tx_data[4] = {FM25Q04B_CMD_READ_JEDEC_ID, 0xFF, 0xFF, 0xFF};
    uint8_t rx_data[4] = {0};
    ret_code_t ret;

    if (id == NULL) {
        return NRF_ERROR_NULL;
    }

    ret = fm25q04b_transfer(tx_data, sizeof(tx_data), rx_data, sizeof(rx_data));
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    id->manufacturer_id = rx_data[1];
    id->memory_type = rx_data[2];
    id->capacity = rx_data[3];
    return NRF_SUCCESS;
}

static ret_code_t fm25q04b_read_status(uint8_t* status) {
    uint8_t tx_data[2] = {FM25Q04B_CMD_READ_STATUS_1, 0xFF};
    uint8_t rx_data[2] = {0};
    ret_code_t ret;

    if (status == NULL) {
        return NRF_ERROR_NULL;
    }

    ret = fm25q04b_transfer(tx_data, sizeof(tx_data), rx_data, sizeof(rx_data));
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    *status = rx_data[1];
    return NRF_SUCCESS;
}

static ret_code_t fm25q04b_wait_ready(uint32_t timeout_ms) {
    uint8_t status = 0xFF;
    ret_code_t ret;
    uint8_t feed_ticks = 0;

    while (timeout_ms-- > 0) {
        ret = fm25q04b_read_status(&status);
        if (ret != NRF_SUCCESS) {
            return ret;
        }
        if ((status & FM25Q04B_STATUS_WIP_MASK) == 0) {
            return NRF_SUCCESS;
        }
        if (++feed_ticks >= 16U) {
            app_feed_wdt();
            feed_ticks = 0;
        }
        nrf_delay_ms(1);
    }

    NRF_LOG_ERROR("FM25Q04B busy timeout, status=0x%02x", status);
    return NRF_ERROR_TIMEOUT;
}

ret_code_t fm25q04b_read(uint32_t address, void* data, size_t length) {
    uint8_t* out = (uint8_t*)data;
    ret_code_t ret;

    if (data == NULL && length != 0) {
        return NRF_ERROR_NULL;
    }
    if (!fm25q04b_range_is_valid(address, length)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    while (length > 0) {
        size_t chunk = (length > FM25Q04B_READ_CHUNK_MAX) ? FM25Q04B_READ_CHUNK_MAX : length;

        /* PRD §4.2: VLA boundary guard — prevents oversized stack allocation.
         * VLA adapts per-call: small reads (font glyphs) stay light,
         * bulk reads (manifests) get up to 256 bytes per transfer. */
        ASSERT(chunk <= FM25Q04B_READ_CHUNK_MAX);
        uint8_t tx_buf[4 + chunk];
        uint8_t rx_buf[4 + chunk];

        tx_buf[0] = FM25Q04B_CMD_READ_DATA;
        fm25q04b_encode_address(&tx_buf[1], address);
        memset(&tx_buf[4], 0xFF, chunk);

        ret = fm25q04b_transfer(tx_buf, 4 + chunk, rx_buf, 4 + chunk);
        if (ret != NRF_SUCCESS) {
            return ret;
        }

        memcpy(out, &rx_buf[4], chunk);
        out += chunk;
        address += (uint32_t)chunk;
        length -= chunk;
        app_feed_wdt();
    }

    return NRF_SUCCESS;
}

ret_code_t fm25q04b_write(uint32_t address, const void* data, size_t length) {
    const uint8_t* in = (const uint8_t*)data;
    ret_code_t ret;

    if (data == NULL && length != 0) {
        return NRF_ERROR_NULL;
    }
    if (!fm25q04b_range_is_valid(address, length)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    while (length > 0) {
        size_t page_left = FM25Q04B_PAGE_SIZE - (address % FM25Q04B_PAGE_SIZE);
        size_t chunk = (length < page_left) ? length : page_left;
        if (chunk > FM25Q04B_SPI_PAYLOAD_MAX) chunk = FM25Q04B_SPI_PAYLOAD_MAX;

        ret = fm25q04b_page_program(address, in, chunk);
        if (ret != NRF_SUCCESS) {
            return ret;
        }

        in += chunk;
        address += (uint32_t)chunk;
        length -= chunk;
    }

    return NRF_SUCCESS;
}

ret_code_t fm25q04b_erase_sector(uint32_t address) {
    uint8_t tx_data[4];
    ret_code_t ret;

    address &= ~(FM25Q04B_SECTOR_SIZE - 1UL);
    if (!fm25q04b_range_is_valid(address, FM25Q04B_SECTOR_SIZE)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    tx_data[0] = FM25Q04B_CMD_SECTOR_ERASE_4K;
    fm25q04b_encode_address(&tx_data[1], address);

    ret = fm25q04b_write_enable();
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = fm25q04b_transfer(tx_data, sizeof(tx_data), NULL, 0);
    if (ret != NRF_SUCCESS) {
        fm25q04b_write_disable();
        return ret;
    }

    return fm25q04b_wait_ready(FM25Q04B_ERASE_TIMEOUT_MS);
}

ret_code_t fm25q04b_erase_range(uint32_t address, size_t length) {
    uint32_t start;
    uint32_t end;
    ret_code_t ret;

    if (length == 0) {
        return NRF_SUCCESS;
    }
    if (!fm25q04b_range_is_valid(address, length)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    start = address & ~(FM25Q04B_SECTOR_SIZE - 1UL);
    end = (address + (uint32_t)length + FM25Q04B_SECTOR_SIZE - 1UL) & ~(FM25Q04B_SECTOR_SIZE - 1UL);

    for (uint32_t sector = start; sector < end; sector += FM25Q04B_SECTOR_SIZE) {
        ret = fm25q04b_erase_sector(sector);
        if (ret != NRF_SUCCESS) {
            return ret;
        }
    }

    return NRF_SUCCESS;
}
