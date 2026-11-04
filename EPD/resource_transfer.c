#include "resource_transfer.h"
#include "resource_protocol.h"
#include "resource_store.h"
#include "app_util_platform.h"
#include "sdk_errors.h"
#include <string.h>

/* Retransmitted BLE writes are recognized by length + CRC32 instead of a full
 * copy of the previous request. The packet buffers dominate this module's RAM;
 * a 244 B duplicate cache was over-provisioned for a dedupe check (a 2^-32
 * collision would only resend the previous reply instead of re-executing). */
static struct {
    uint8_t request[RESOURCE_PACKET_MAX], reply[RESOURCE_PACKET_MAX];
    uint8_t length, previous_length, reply_length;
    uint32_t previous_crc;
    uint32_t generation, request_generation, last_activity;
    volatile bool pending, running, reply_pending;
} m;

bool resource_transfer_running(void) { return m.running; }
bool resource_transfer_reply_pending(void) { return m.reply_pending; }

void resource_transfer_receive(const uint8_t *data, uint16_t length, uint32_t generation)
{
    if (!data || length < 3 || length > RESOURCE_PACKET_MAX) return;
    CRITICAL_REGION_ENTER();
    if (!m.pending && !m.running && !m.reply_pending) {
        memcpy(m.request, data, length);
        m.length = length;
        m.request_generation = generation;
        m.pending = true;
    }
    CRITICAL_REGION_EXIT();
}

void resource_transfer_process(void *context, uint32_t generation, bool connected,
                               bool idle, uint32_t now,
                               uint32_t (*send)(void *, uint32_t, uint8_t *, uint16_t))
{
    /* Generation is supplied by the BLE service; an old reply is never sent
     * on a new connection. Mutations already executing may finish, but retry
     * after reconnect starts a fresh transaction and validates persisted data. */
    if (m.generation != generation || (!connected &&
        (m.pending || m.reply_pending || m.previous_length || resource_protocol_entered()))) {
        CRITICAL_REGION_ENTER();
        m.running = true;
        m.generation = generation;
        if (!connected || m.request_generation != generation) m.pending = false;
        m.reply_pending = false;
        m.previous_length = 0;
        CRITICAL_REGION_EXIT();
        resource_protocol_reset();
        m.running = false;
    }
    if (!connected) return;
    if (resource_protocol_entered() && (uint32_t)(now - m.last_activity) >= 60U) {
        CRITICAL_REGION_ENTER();
        m.running = true;
        m.previous_length = 0;
        m.reply_pending = false;
        m.pending = false; /* A request from the expired session cannot execute. */
        CRITICAL_REGION_EXIT();
        resource_protocol_reset();
        m.running = false;
    }
    if (m.pending && idle) {
        CRITICAL_REGION_ENTER();
        m.running = true;
        m.pending = false;
        CRITICAL_REGION_EXIT();
        const uint32_t request_crc = resource_crc32(0U, m.request, m.length);
        if (m.previous_length != m.length || m.previous_crc != request_crc) {
            m.reply_length = resource_protocol_execute(m.request, m.length, m.reply);
            m.previous_crc = request_crc;
            m.previous_length = m.length;
        }
        m.last_activity = now;
        m.reply_pending = true;
        m.running = false;
    }
    if (m.reply_pending) {
        /* Keep the reply on queue-full and transient failures. A disconnect
         * clears it in the next main turn; RX never performs SPI or HVX. */
        uint32_t result = send(context, generation, m.reply, m.reply_length);
        if (result == NRF_SUCCESS) {
            m.reply_pending = false;
        } else if (result != NRF_ERROR_RESOURCES && result != NRF_ERROR_BUSY) {
            /* Terminal delivery failure: notifications are disabled, the
             * connection changed, or the length is invalid. Dropping the reply
             * lets the display worker run again; the peer's next request is
             * re-executed/deduplicated and answered if it can receive. */
            m.reply_pending = false;
        }
    }
}
