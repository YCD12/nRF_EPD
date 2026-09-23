#include "resource_protocol.h"
#include "resource_layout.h"
#include "epd_protocol.h"
#include "fm25q04b.h"
#include <string.h>

static bool m_entered;

void resource_protocol_reset(void)
{
    m_entered = false;
    resource_store_abort();
}

bool resource_protocol_entered(void) { return m_entered; }

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
        a[4] = RESOURCE_STORE_WRITABLE;
        a[5] = RS_ABI;
        a[6] = (uint8_t)resource_store_active();
        a[7] = m_entered;
        resource_put_u32(a + 8, resource_store_received());
        resource_put_u32(a + 12, RS_CAPACITY);
        reply_length = 16;
        break;
    case 1: /* Raw read is bounded, read-only, also supports old-layout export. */
        if (n != 8 || q[7] == 0 || q[7] > 16) break;
        if (resource_get_u32(q + 3) > RS_CAPACITY || q[7] > RS_CAPACITY - resource_get_u32(q + 3)) break;
        r = fm25q04b_read(resource_get_u32(q + 3), a + 4, q[7]) == NRF_SUCCESS ? RS_OK : RS_IO;
        if (r == RS_OK) reply_length += q[7];
        break;
#if RESOURCE_STORE_WRITABLE
    case 6: /* Enter an exclusive resource-write session; no erase yet. */
        if (n != 7 || memcmp(q + 3, "MIG1", 4)) break;
        m_entered = true;
        r = RS_OK;
        break;
    case 2:
        if (!m_entered) { r = RS_READ_ONLY; break; }
        if (n == 16) r = resource_store_begin(q[3], resource_get_u32(q + 4),
                                              resource_get_u32(q + 8), resource_get_u32(q + 12));
        break;
    case 3:
        if (!m_entered) { r = RS_READ_ONLY; break; }
        if (n > 7) r = resource_store_write(resource_get_u32(q + 3), q + 7, n - 7);
        break;
    case 4:
        if (!m_entered) { r = RS_READ_ONLY; break; }
        if (n == 3) r = resource_store_commit();
        break;
    case 5:
        if (n == 3) { resource_store_abort(); r = RS_OK; }
        break;
    case 7: /* Leave the write session and resume normal display work. */
        if (n == 3) { resource_store_abort(); m_entered = false; r = RS_OK; }
        break;
#else
    case 2: case 3: case 4: case 5: case 6: case 7:
        r = RS_READ_ONLY;
        break;
#endif
    default: break;
    }
done:
    a[3] = (uint8_t)r;
    return reply_length;
}
