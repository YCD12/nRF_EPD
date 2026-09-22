#include "EPD_config.h"
#include "persistent_record.h"
#include "resource_layout.h"
#include "fm25q04b.h"
#include "app_util_platform.h"
#include "main.h"
#include <string.h>

#define EPD_CONFIG_RECORD_MAGIC 0x31435045UL /* EPC1 */

/* BLE handlers may replace the requested value while main is writing. The
 * snapshot remains immutable for the complete erase/program/commit cycle. */
static epd_config_t m_requested, m_snapshot, m_committed;
static volatile bool m_pending, m_busy, m_clear, m_clear_done, m_result_pending;
static bool m_committed_valid;
static uint8_t m_attempts, m_result;
static uint32_t m_due, m_revision, m_data_crc;

uint32_t epd_config_init(epd_config_t *cfg)
{
    (void)cfg;
    m_pending = m_busy = m_clear = m_clear_done = m_result_pending = false;
    m_committed_valid = false;
    m_attempts = 0U;
    m_due = 0U;
    /* Resource damage must not prevent BLE recovery from starting. */
    (void)fm25q04b_init();
    return 0U;
}

void epd_config_read(epd_config_t *cfg)
{
    persistent_record_info_t info;
    memset(cfg, 0xFF, sizeof(*cfg));
    rs_result_t r = persistent_record_load(RS_PART_EPD_CONFIG, EPD_CONFIG_RECORD_MAGIC,
                                           cfg, sizeof(*cfg), &info);
    if (r == RS_OK && info.length == sizeof(*cfg)) {
        m_committed = *cfg;
        m_committed_valid = true;
        m_revision = info.revision;
        m_data_crc = info.data_crc;
    } else {
        memset(cfg, 0xFF, sizeof(*cfg));
        m_committed_valid = false;
        m_revision = 0U;
        m_data_crc = 0U;
    }
}

void epd_config_write(epd_config_t *cfg)
{
    if (cfg == NULL) return;
    CRITICAL_REGION_ENTER();
    if (!m_clear) {
        m_requested = *cfg;
        m_pending = true;
    }
    CRITICAL_REGION_EXIT();
}

void epd_config_clear(epd_config_t *cfg)
{
    (void)cfg;
    CRITICAL_REGION_ENTER();
    m_clear = true;
    m_clear_done = false;
    m_pending = false;
    m_attempts = 0U;
    CRITICAL_REGION_EXIT();
}

bool epd_config_clear_complete(void) { return m_clear_done; }

bool epd_config_busy(void) { return m_busy || m_pending || m_clear; }

bool epd_config_is_saved(const epd_config_t *cfg)
{
    return !epd_config_busy() && m_committed_valid && cfg != NULL &&
           memcmp(cfg, &m_committed, sizeof(*cfg)) == 0;
}

static void publish_result(rs_result_t result, const persistent_record_info_t *info)
{
    m_result = (uint8_t)result;
    if (info != NULL) {
        m_revision = info->revision;
        m_data_crc = info->data_crc;
    }
    m_result_pending = true;
}

bool epd_config_take_save_result(uint8_t *result, uint32_t *revision, uint32_t *data_crc)
{
    if (!m_result_pending || result == NULL || revision == NULL || data_crc == NULL) return false;
    CRITICAL_REGION_ENTER();
    *result = m_result;
    *revision = m_revision;
    *data_crc = m_data_crc;
    m_result_pending = false;
    CRITICAL_REGION_EXIT();
    return true;
}

void epd_config_process(void)
{
    bool clearing, start = false;
    uint32_t now = app_uptime_seconds();
    if (m_busy || m_clear_done || (int32_t)(now - m_due) < 0) return;

    CRITICAL_REGION_ENTER();
    clearing = m_clear;
    if (clearing || m_pending) {
        if (!clearing) m_snapshot = m_requested;
        m_pending = false;
        m_busy = true;
        start = true;
    }
    CRITICAL_REGION_EXIT();
    if (!start) return;

    if (!clearing && m_committed_valid &&
        memcmp(&m_snapshot, &m_committed, sizeof(m_snapshot)) == 0) {
        persistent_record_info_t same = {m_revision, m_data_crc, sizeof(m_snapshot)};
        m_busy = false;
        m_attempts = 0U;
        publish_result(RS_OK, &same);
        return;
    }

    persistent_record_info_t info;
    uint16_t attempted_length = clearing ? 0U : sizeof(m_snapshot);
    uint32_t attempted_crc = persistent_record_data_crc(clearing ? NULL : &m_snapshot,
                                                        attempted_length);
    rs_result_t r = persistent_record_save(RS_PART_EPD_CONFIG, EPD_CONFIG_RECORD_MAGIC,
                                           clearing ? NULL : &m_snapshot,
                                           attempted_length, clearing, &info);
    m_busy = false;
    if (r == RS_OK) {
        m_attempts = 0U;
        if (clearing) {
            m_clear_done = true;
            m_clear = false;
            m_committed_valid = false;
        } else {
            m_committed = m_snapshot;
            m_committed_valid = true;
        }
        publish_result(r, &info);
        return;
    }
    /* A failed single-copy write may already have erased the old record. */
    if (r != RS_BUSY) m_committed_valid = false;
    if (r == RS_BUSY || ++m_attempts <= 3U) {
        m_due = now + 1U;
        if (!clearing) m_pending = true;
        return;
    }
    m_attempts = 0U;
    if (clearing) m_clear = false;
    persistent_record_info_t failed = {0U, attempted_crc, attempted_length};
    publish_result(r, &failed);
}

bool epd_config_empty(epd_config_t *cfg)
{
    for (size_t i = 0; i < sizeof(*cfg); i++)
        if (((uint8_t *)cfg)[i] != 0xFFU) return false;
    return true;
}
