#include "EPD_config.h"
#include "storage_retry.h"
#include "app_util_platform.h"
#include "fds.h"
#include "main.h"
#include "nrf_soc.h"
#include <string.h>

#define CONFIG_FILE_ID 0x0000
#define CONFIG_REC_KEY 0x0001
#define CONFIG_WORDS ((sizeof(epd_config_t) + 3U) / 4U)

/* Requested values may change in BLE IRQs. The aligned snapshot remains
 * immutable until the matching FDS completion has been consumed in main. */
static uint32_t m_requested[CONFIG_WORDS];
static uint32_t m_snapshot[CONFIG_WORDS];
static volatile bool m_pending, m_busy, m_done, m_clear, m_clear_done;
static volatile bool m_deleting, m_clear_failed;
static volatile uint32_t m_result;
static volatile bool m_initialized;
static volatile uint32_t m_init_result;
static uint8_t m_attempts;
static uint32_t m_due;

static void fds_evt_handler(fds_evt_t const *evt)
{
    if (evt->id == FDS_EVT_INIT) {
        m_init_result = evt->result;
        m_initialized = true;
    } else if (m_busy &&
               ((!m_deleting && (evt->id == FDS_EVT_WRITE || evt->id == FDS_EVT_UPDATE) &&
                 evt->write.file_id == CONFIG_FILE_ID && evt->write.record_key == CONFIG_REC_KEY) ||
                (m_deleting && evt->id == FDS_EVT_DEL_RECORD &&
                 evt->del.file_id == CONFIG_FILE_ID && evt->del.record_key == CONFIG_REC_KEY))) {
        m_result = evt->result;
        m_done = true;
    }
}

uint32_t epd_config_init(epd_config_t *cfg)
{
    (void)cfg;
    uint32_t err = epd_storage_init();
    if (err != 0U) return err;
    err = fds_register(fds_evt_handler);
    if (err != 0U) return err;
    m_initialized = false;
    err = fds_init();
    if (err != 0U) return err;
    /* Main-context startup, before advertising. SoftDevice IRQs stay enabled
     * so fresh-install and recovery writes can complete. */
    app_feed_wdt();
    while (!m_initialized) {
        (void)sd_app_evt_wait();
    }
    return m_init_result;
}

void epd_config_read(epd_config_t *cfg)
{
    fds_record_desc_t desc;
    fds_find_token_t token = {0};
    fds_flash_record_t record;
    memset(cfg, 0xFF, sizeof(*cfg));
    if (fds_record_find(CONFIG_FILE_ID, CONFIG_REC_KEY, &desc, &token) != 0U ||
        fds_record_open(&desc, &record) != 0U) return;
    uint32_t length = record.p_header->length_words * 4U;
    if (length > sizeof(*cfg)) length = sizeof(*cfg);
    memcpy(cfg, record.p_data, length);
    (void)fds_record_close(&desc);
}

void epd_config_write(epd_config_t *cfg)
{
    CRITICAL_REGION_ENTER();
    if (!m_clear) {
        memset(m_requested, 0, sizeof(m_requested));
        memcpy(m_requested, cfg, sizeof(*cfg));
        m_pending = true;
    }
    CRITICAL_REGION_EXIT();
}

void epd_config_clear(epd_config_t *cfg)
{
    (void)cfg;
    CRITICAL_REGION_ENTER();
    m_clear = true;
    m_clear_failed = false;
    m_pending = false;
    CRITICAL_REGION_EXIT();
}

bool epd_config_clear_complete(void) { return m_clear_done; }

void epd_config_process(void)
{
    uint32_t now = app_uptime_seconds();
    if (m_done) {
        uint32_t result = m_result;
        m_done = false;
        m_busy = false;
        if (result == 0U || (m_clear && !m_deleting)) {
            m_attempts = 0U;
        } else if (epd_storage_retry(result, &m_attempts, &m_due)) {
            if (!m_clear) m_pending = true;
        } else if (m_clear) {
            m_clear_failed = true; /* Never reset after a failed deletion. */
        }
    }
    if (m_busy || epd_storage_gc_pending() || m_clear_done ||
        (int32_t)(now - m_due) < 0 || (m_clear && m_clear_failed)) return;

    bool start = false;
    CRITICAL_REGION_ENTER();
    if (m_clear || m_pending) {
        m_deleting = m_clear;
        memcpy(m_snapshot, m_requested, sizeof(m_snapshot));
        m_pending = false;
        m_busy = true;
        start = true;
    }
    CRITICAL_REGION_EXIT();
    if (!start) return;

    fds_record_desc_t desc;
    fds_find_token_t token = {0};
    uint32_t err = fds_record_find(CONFIG_FILE_ID, CONFIG_REC_KEY, &desc, &token);
    if (m_deleting) {
        if (err == FDS_ERR_NOT_FOUND) {
            m_busy = false;
            m_clear_done = true;
            return;
        }
        if (err == 0U) err = fds_record_delete(&desc);
    } else {
        fds_record_t record = {.file_id = CONFIG_FILE_ID, .key = CONFIG_REC_KEY,
            .data = {.p_data = m_snapshot, .length_words = CONFIG_WORDS}};
        if (err == 0U) err = fds_record_update(&desc, &record);
        else if (err == FDS_ERR_NOT_FOUND) err = fds_record_write(&desc, &record);
    }
    if (err != 0U) {
        m_result = err;
        m_done = true;
    }
}

bool epd_config_empty(epd_config_t *cfg)
{
    for (size_t i = 0; i < sizeof(*cfg); i++)
        if (((uint8_t *)cfg)[i] != 0xFFU) return false;
    return true;
}
