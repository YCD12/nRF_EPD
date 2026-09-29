#include "calendar_annotations_store.h"
#include "calendar_annotations.h"
#include "persistent_record.h"
#include "resource_layout.h"
#include "app_util_platform.h"
#include "main.h"

#define ANNOTATIONS_RECORD_MAGIC 0x31434E41UL /* ANC1 */

static volatile bool m_busy, m_save_pending, m_pending_clear, m_result_pending;
static uint8_t m_attempts, m_result;
static uint16_t m_committed_length;
static uint32_t m_due, m_revision, m_data_crc, m_committed_crc;
static bool m_committed_valid;
static calendar_annotations_store_applied_fn m_on_applied;

bool calendar_annotations_store_valid(void) { return m_committed_valid; }

static bool store_reload(void)
{
    uint16_t capacity = 0U;
    uint8_t *buffer = calendar_annotations_load_buffer(&capacity);
    persistent_record_info_t info;
    if (buffer == NULL) return false;
    rs_result_t r = persistent_record_load(RS_PART_CALENDAR_ANNOTATIONS,
                                          ANNOTATIONS_RECORD_MAGIC, buffer, capacity, &info);
    if (r == RS_BUSY) return false;
    if (r == RS_OK &&
        calendar_annotations_load_commit(info.length)) {
        m_revision = info.revision;
        m_data_crc = m_committed_crc = info.data_crc;
        m_committed_length = info.length;
        m_committed_valid = true;
        return true;
    }
    calendar_annotations_reset();
    m_committed_valid = false;
    m_committed_length = 0U;
    m_committed_crc = 0U;
    return true;
}

void calendar_annotations_store_init(void)
{
    m_busy = m_save_pending = m_pending_clear = m_result_pending = false;
    m_attempts = 0U;
    m_due = 0U;
    calendar_annotations_set_reload(store_reload);
    calendar_annotations_set_live_locked(false);
    calendar_annotations_reset();
    store_reload();
}

void calendar_annotations_store_on_applied(calendar_annotations_store_applied_fn fn)
{
    m_on_applied = fn;
}

void calendar_annotations_store_save(void)
{
    CRITICAL_REGION_ENTER();
    calendar_annotations_set_live_locked(true);
    m_save_pending = true;
    CRITICAL_REGION_EXIT();
}

bool calendar_annotations_store_clear(void)
{
    bool applied = false;
    CRITICAL_REGION_ENTER();
    if (m_busy || m_save_pending || calendar_annotations_reload_pending()) {
        m_pending_clear = true;
    } else {
        calendar_annotations_clear();
        calendar_annotations_set_live_locked(true);
        m_save_pending = true;
        m_attempts = 0U;
        applied = true;
    }
    CRITICAL_REGION_EXIT();
    return applied;
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

bool calendar_annotations_store_take_save_result(uint8_t *result, uint32_t *revision,
                                                 uint32_t *data_crc)
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

void calendar_annotations_store_process(void)
{
    bool applied = false;
    uint32_t now = app_uptime_seconds();
    if (m_busy) return;
    if (calendar_annotations_process_reload() && m_on_applied != NULL) m_on_applied();
    if (calendar_annotations_reload_pending() || (int32_t)(now - m_due) < 0) return;

    CRITICAL_REGION_ENTER();
    if (!m_save_pending && m_pending_clear) {
        calendar_annotations_set_live_locked(false);
        calendar_annotations_clear();
        calendar_annotations_set_live_locked(true);
        m_pending_clear = false;
        m_save_pending = true;
        m_attempts = 0U;
        applied = true;
    }
    CRITICAL_REGION_EXIT();
    if (applied && m_on_applied != NULL) m_on_applied();
    if (!m_save_pending) {
        calendar_annotations_set_live_locked(false);
        return;
    }

    const uint8_t *blob;
    uint16_t length = calendar_annotations_blob(&blob);
    if (length == 0U || blob == NULL) return;
    uint32_t crc = persistent_record_data_crc(blob, length);
    if (m_committed_valid && m_committed_length == length && m_committed_crc == crc) {
        persistent_record_info_t same = {m_revision, crc, length};
        m_save_pending = false;
        m_attempts = 0U;
        calendar_annotations_set_live_locked(false);
        publish_result(RS_OK, &same);
        return;
    }

    m_busy = true;
    persistent_record_info_t info;
    rs_result_t r = persistent_record_save(RS_PART_CALENDAR_ANNOTATIONS,
                                           ANNOTATIONS_RECORD_MAGIC,
                                           blob, length, false, &info);
    m_busy = false;
    if (r == RS_OK) {
        m_save_pending = false;
        m_attempts = 0U;
        m_committed_valid = true;
        m_committed_length = length;
        m_committed_crc = info.data_crc;
        calendar_annotations_set_live_locked(false);
        publish_result(r, &info);
        return;
    }
    /* The old record is not guaranteed to survive a failed erase/program. */
    if (r != RS_BUSY) m_committed_valid = false;
    if (r == RS_BUSY || ++m_attempts <= 3U) {
        m_due = now + 1U;
        return;
    }
    m_save_pending = false;
    m_attempts = 0U;
    calendar_annotations_set_live_locked(false);
    persistent_record_info_t failed = {0U, crc, length};
    publish_result(r, &failed);
}

bool calendar_annotations_store_busy(void)
{
    return m_busy || m_save_pending || m_pending_clear || calendar_annotations_reload_pending();
}
