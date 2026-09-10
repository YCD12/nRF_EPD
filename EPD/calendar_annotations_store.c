#include "calendar_annotations_store.h"

#include <string.h>

#include "app_scheduler.h"
#include "calendar_annotations.h"
#include "fds.h"
#include "nordic_common.h"
#include "nrf_log.h"

#define ANNO_FILE_ID 0x0001
#define ANNO_REC_KEY 0x0001

#define ANNO_SAVE_RETRIES 3U

static bool m_fds_busy;
static bool m_save_pending;
static bool m_pending_clear;
static uint8_t m_save_retries;
static calendar_annotations_store_applied_fn m_on_applied;

static void try_save(void);

static void run_fds_gc(void* p_event_data, uint16_t event_size)
{
    (void)p_event_data;
    (void)event_size;
    (void)fds_gc();
}

static void schedule_gc(void)
{
    app_sched_event_put(NULL, 0, run_fds_gc);
}

static void notify_applied(void)
{
    if (m_on_applied != NULL) {
        m_on_applied();
    }
}

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

static void finish_or_retry(ret_code_t result)
{
    bool applied_clear = false;

    m_fds_busy = false;
    calendar_annotations_set_live_locked(false);

    if (result == FDS_ERR_NO_SPACE_IN_FLASH) {
        m_save_pending = true;
        schedule_gc();
        return;
    }
    if (result != NRF_SUCCESS) {
        NRF_LOG_ERROR("anno store: fds op failed, code=%d\n", result);
        if (m_save_retries >= ANNO_SAVE_RETRIES) {
            m_save_retries = 0;
            m_save_pending = false;
            m_pending_clear = false;
            return;
        }
        m_save_retries++;
        m_save_pending = true;
    } else {
        m_save_retries = 0;
    }

    if (m_pending_clear) {
        m_pending_clear = false;
        calendar_annotations_clear();
        m_save_pending = true;
        applied_clear = true;
    }

    if (m_save_pending) {
        m_save_pending = false;
        try_save();
    }
    if (applied_clear) {
        notify_applied();
    }
}

static void fds_evt_handler(fds_evt_t const* const p_evt)
{
    if (p_evt->id == FDS_EVT_WRITE || p_evt->id == FDS_EVT_UPDATE) {
        if (p_evt->write.file_id != ANNO_FILE_ID || p_evt->write.record_key != ANNO_REC_KEY) {
            if (!m_fds_busy && (m_save_pending || m_pending_clear)) {
                finish_or_retry(NRF_SUCCESS);
            }
            return;
        }
        finish_or_retry(p_evt->result);
        return;
    }
    if ((p_evt->id == FDS_EVT_GC || p_evt->id == FDS_EVT_DEL_RECORD ||
         p_evt->id == FDS_EVT_DEL_FILE) &&
        (m_save_pending || m_pending_clear)) {
        finish_or_retry(NRF_SUCCESS);
    }
}

static void try_save(void)
{
    fds_record_t record;
    fds_record_desc_t record_desc;
    fds_find_token_t ftok;
    const uint8_t* blob = NULL;
    uint16_t length = calendar_annotations_blob(&blob);
    ret_code_t ret;

    if (m_fds_busy) {
        m_save_pending = true;
        return;
    }
    if (blob == NULL || length == 0U) {
        return;
    }

    record.file_id = ANNO_FILE_ID;
    record.key = ANNO_REC_KEY;
    record.data.p_data = blob;
    record.data.length_words = BYTES_TO_WORDS(length);
    memset(&ftok, 0x00, sizeof(ftok));
    ret = fds_record_find(ANNO_FILE_ID, ANNO_REC_KEY, &record_desc, &ftok);
    if (ret == NRF_SUCCESS) {
        ret = fds_record_update(&record_desc, &record);
    } else {
        ret = fds_record_write(&record_desc, &record);
    }

    if (ret == NRF_SUCCESS) {
        m_fds_busy = true;
        calendar_annotations_set_live_locked(true);
        return;
    }

    NRF_LOG_ERROR("anno store: write failed, code=%d\n", ret);
    if (ret == FDS_ERR_NO_SPACE_IN_FLASH) {
        m_save_pending = true;
        schedule_gc();
        return;
    }
    if (ret == FDS_ERR_BUSY || ret == FDS_ERR_NO_SPACE_IN_QUEUES) {
        m_save_pending = true;
    }
}

void calendar_annotations_store_init(void)
{
    ret_code_t ret = fds_register(fds_evt_handler);

    if (ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("anno store: fds_register failed, code=%d\n", ret);
    }
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
    try_save();
}

bool calendar_annotations_store_clear(void)
{
    if (m_fds_busy) {
        m_pending_clear = true;
        return false;
    }
    calendar_annotations_clear();
    try_save();
    return true;
}

bool calendar_annotations_store_busy(void)
{
    return m_fds_busy || m_save_pending || m_pending_clear;
}
