#include "storage_retry.h"

#include "fds.h"
#include "main.h"

static volatile bool m_gc_needed;
static volatile bool m_gc_busy;
static uint32_t m_gc_due;

static void storage_fds_evt_handler(fds_evt_t const *evt)
{
    if (evt->id == FDS_EVT_GC) m_gc_busy = false;
}

uint32_t epd_storage_init(void)
{
    m_gc_needed = false;
    m_gc_busy = false;
    m_gc_due = 0U;
    return fds_register(storage_fds_evt_handler);
}

bool epd_storage_gc_pending(void)
{
    return m_gc_needed || m_gc_busy;
}

bool epd_storage_retry(uint32_t result, uint8_t *attempts, uint32_t *due)
{
    if (result != FDS_ERR_BUSY && result != FDS_ERR_NO_SPACE_IN_QUEUES) {
        if (++*attempts > 3U) return false;
    }
    if (result == FDS_ERR_NO_SPACE_IN_FLASH && !m_gc_busy) m_gc_needed = true;
    *due = app_uptime_seconds() + 1U;
    return true;
}

void epd_storage_process(void)
{
    uint32_t now = app_uptime_seconds();
    if (!m_gc_needed || m_gc_busy || (int32_t)(now - m_gc_due) < 0) return;

    m_gc_busy = true;
    m_gc_needed = false;
    if (fds_gc() != 0U) {
        m_gc_busy = false;
        m_gc_needed = true;
        m_gc_due = now + 1U;
    }
}
