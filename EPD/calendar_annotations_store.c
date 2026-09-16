#include "calendar_annotations_store.h"
#include "calendar_annotations.h"
#include "storage_retry.h"
#include "app_util_platform.h"
#include "fds.h"
#include "main.h"
#include "nrf_log.h"
#include <string.h>

#define ANNO_FILE_ID 0x0001
#define ANNO_REC_KEY 0x0001
static volatile bool m_fds_busy, m_save_pending, m_pending_clear, m_done;
static volatile uint32_t m_result;
static uint8_t m_attempts;
static uint32_t m_due;
static calendar_annotations_store_applied_fn m_on_applied;

static void store_reload(void)
{
    fds_flash_record_t flash_record;
    fds_record_desc_t record_desc;
    fds_find_token_t ftok;
    uint32_t record_len;

    memset(&ftok, 0x00, sizeof(ftok));
    calendar_annotations_reset();
    if (fds_record_find(ANNO_FILE_ID, ANNO_REC_KEY, &record_desc, &ftok) != NRF_SUCCESS) {
        return;
    }
    if (fds_record_open(&record_desc, &flash_record) != NRF_SUCCESS) {
        NRF_LOG_ERROR("anno store: open failed\n");
        return;
    }
    record_len = flash_record.p_header->length_words * sizeof(uint32_t);
    if (record_len > CALENDAR_ANNOTATIONS_MAX_BYTES) {
        record_len = CALENDAR_ANNOTATIONS_MAX_BYTES;
    }
    (void)calendar_annotations_load(flash_record.p_data, (size_t)record_len);
    fds_record_close(&record_desc);
}

static void fds_evt_handler(fds_evt_t const *evt)
{
    /* Unrelated writes, deletes and GC must never release our live-data lock. */
    if (m_fds_busy && (evt->id == FDS_EVT_WRITE || evt->id == FDS_EVT_UPDATE) &&
        evt->write.file_id == ANNO_FILE_ID && evt->write.record_key == ANNO_REC_KEY) {
        m_result = evt->result;
        m_done = true;
    }
}

void calendar_annotations_store_init(void)
{
    (void)fds_register(fds_evt_handler);
    calendar_annotations_set_reload(store_reload);
    calendar_annotations_set_live_locked(false);
    store_reload();
}

void calendar_annotations_store_on_applied(calendar_annotations_store_applied_fn fn)
{
    m_on_applied = fn;
}

void calendar_annotations_store_save(void)
{
    CRITICAL_REGION_ENTER();
    /* Lock at acceptance, not later when main submits the FDS operation. */
    calendar_annotations_set_live_locked(true);
    m_save_pending = true;
    CRITICAL_REGION_EXIT();
}

bool calendar_annotations_store_clear(void)
{
    bool applied = false;
    CRITICAL_REGION_ENTER();
    if (m_fds_busy || m_save_pending) {
        m_pending_clear = true;
    } else {
        calendar_annotations_clear();
        calendar_annotations_set_live_locked(true);
        m_save_pending = true;
        applied = true;
    }
    CRITICAL_REGION_EXIT();
    return applied;
}

void calendar_annotations_store_process(void)
{
    bool applied = false;
    CRITICAL_REGION_ENTER();
    if (m_done) {
        m_done = false;
        m_fds_busy = false;
        if (m_result == 0U) m_attempts = 0;
        else m_save_pending = epd_storage_retry(m_result, &m_attempts, &m_due);
    }
    if (!m_fds_busy && m_pending_clear) {
        calendar_annotations_set_live_locked(false);
        calendar_annotations_clear();
        calendar_annotations_set_live_locked(true);
        m_pending_clear = false;
        m_save_pending = true;
        m_attempts = 0;
        applied = true;
    }
    if (!m_fds_busy && !m_save_pending) calendar_annotations_set_live_locked(false);
    CRITICAL_REGION_EXIT();
    if (applied && m_on_applied != NULL) m_on_applied();
    if (m_fds_busy || !m_save_pending || epd_storage_gc_pending() ||
        (int32_t)(app_uptime_seconds() - m_due) < 0) return;

    const uint8_t *blob;
    uint16_t length = calendar_annotations_blob(&blob);
    if (length == 0U || blob == NULL) return;
    fds_record_t record = {.file_id = ANNO_FILE_ID, .key = ANNO_REC_KEY,
        .data = {.p_data = blob, .length_words = (length + 3U) / 4U}};
    fds_record_desc_t desc;
    fds_find_token_t token = {0};
    m_fds_busy = true;
    m_save_pending = false;
    uint32_t err = fds_record_find(ANNO_FILE_ID, ANNO_REC_KEY, &desc, &token);
    if (err == 0U) err = fds_record_update(&desc, &record);
    else if (err == FDS_ERR_NOT_FOUND) err = fds_record_write(&desc, &record);
    if (err != 0U) {
        m_result = err;
        m_done = true;
    }
}

bool calendar_annotations_store_busy(void)
{
    return m_fds_busy || m_save_pending || m_pending_clear;
}
