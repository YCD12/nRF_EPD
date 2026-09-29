#include "resource_protocol.h"
#include "resource_layout.h"
#include "epd_protocol.h"
#include "fm25q04b.h"
#include <string.h>

static volatile bool m_entered;
static uint16_t m_changed;
static resource_event_handler_t m_handler;
static void *m_context;

void resource_protocol_set_handler(resource_event_handler_t handler, void *context)
{
    m_handler = handler;
    m_context = context;
}

static rs_result_t activate(bool force)
{
    rs_result_t result = RS_OK;
    if (m_handler && (m_changed || force))
        result = m_handler(m_context, RESOURCE_ACTIVATE, m_changed);
    if (result == RS_OK) m_changed = 0U;
    return result;
}

void resource_protocol_reset(void)
{
    bool was_entered = m_entered;
    m_entered = true; /* Keep ISR-owned display commands out until reload ends. */
    resource_store_abort();
    if (was_entered) (void)activate(false);
    m_entered = false;
}

bool resource_protocol_entered(void) { return m_entered; }

static rs_result_t enter(void)
{
    bool previous = m_entered;
    m_entered = true;
    /* Recheck after blocking IRQ mutations: work may have arrived since the
     * main loop's idle check. Failed acquisition keeps the previous session. */
    rs_result_t result = m_handler ? m_handler(m_context, RESOURCE_ENTER, 0U) : RS_OK;
    if (result != RS_OK) m_entered = previous;
    return result;
}

uint8_t resource_protocol_execute(const uint8_t *q, uint8_t n, uint8_t *a)
{
    rs_result_t r = RS_INVALID;
    uint8_t reply_length = 4;
    memset(a, 0, RESOURCE_PACKET_MAX);
    a[0] = EPD_RSP_RESOURCE;
    a[1] = n > 1 ? q[1] : 0xFF;
    a[2] = n > 2 ? q[2] : 0;
    if (n < 3 || n > RESOURCE_PACKET_MAX || q[0] != EPD_CMD_RESOURCE) goto done;
    fm25q04b_jedec_id_t jedec;
    if (fm25q04b_init() != NRF_SUCCESS || fm25q04b_probe(&jedec) != NRF_SUCCESS) {
        r = RS_IO;
        goto done;
    }
    switch (q[1]) {
    case 0: /* Status: mount result, writer capability, ABI, target, received. */
        if (n != 3) break;
        r = resource_store_mount();
        a[4] = RESOURCE_CAP_WRITE | (m_handler ? RESOURCE_CAP_ACTIVATE | RESOURCE_CAP_OFFLINE : 0U);
        a[5] = RS_ABI;
        a[6] = (uint8_t)resource_store_active();
        a[7] = m_entered;
        resource_put_u32(a + 8, resource_store_received());
        resource_put_u32(a + 12, RS_CAPACITY);
        reply_length = 16;
        break;
    case 1: /* Raw read is bounded and also supports old-layout export. */
        if (n != 8 || q[7] == 0 || q[7] > 16) break;
        if (resource_get_u32(q + 3) > RS_CAPACITY || q[7] > RS_CAPACITY - resource_get_u32(q + 3)) break;
        r = fm25q04b_read(resource_get_u32(q + 3), a + 4, q[7]) == NRF_SUCCESS ? RS_OK : RS_IO;
        if (r == RS_OK) reply_length += q[7];
        break;
    case 6: /* Enter an exclusive resource-write session; no erase yet. */
        if (n != 7 || memcmp(q + 3, "MIG1", 4)) break;
        r = enter();
        break;
    case 2:
        if (!m_entered) { r = RS_WRITE_PROTECTED; break; }
        if (n == 16) {
            uint16_t changed = q[3] < 16U ? (uint16_t)(1U << q[3]) : 1U;
            if (resource_store_active()) { r = RS_BUSY; break; }
            if (m_handler) {
                r = m_handler(m_context, RESOURCE_INVALIDATE, changed);
                if (r != RS_OK) break;
            }
            m_changed |= changed;
            r = resource_store_begin(q[3], resource_get_u32(q + 4),
                                     resource_get_u32(q + 8), resource_get_u32(q + 12));
        }
        break;
    case 3:
        if (!m_entered) { r = RS_WRITE_PROTECTED; break; }
        if (n > 7) r = resource_store_write(resource_get_u32(q + 3), q + 7, n - 7);
        break;
    case 4:
        if (!m_entered) { r = RS_WRITE_PROTECTED; break; }
        if (n == 3) r = resource_store_commit();
        break;
    case 5:
        if (n == 3) { resource_store_abort(); r = RS_OK; }
        break;
    case 7: /* Leave/abort, then reload consumers before rendering can resume. */
        if (n == 3) {
            m_entered = true;
            resource_store_abort();
            r = activate(false);
            m_entered = false;
        }
        break;
    case 8: /* Validate and disconnect; preserve time and the advertising policy. */
        if (n != 3 || !m_handler) break;
        if (resource_store_active()) { r = RS_BUSY; break; }
        r = enter();
        if (r != RS_OK) break;
        r = activate(true);
        if (r == RS_OK) r = m_handler(m_context, RESOURCE_OFFLINE, 0U);
        m_entered = false;
        break;
    default: break;
    }
done:
    a[3] = (uint8_t)r;
    return reply_length;
}
