#include "resource_transfer.h"
#include "resource_protocol.h"
#include "app_util_platform.h"
#include "sdk_errors.h"
#include <string.h>

static struct {
    uint8_t request[RESOURCE_PACKET_MAX], previous[RESOURCE_PACKET_MAX], reply[RESOURCE_PACKET_MAX];
    uint8_t length, previous_length, reply_length;
    uint32_t generation, request_generation, last_activity;
    volatile bool pending, running, reply_pending;
} m;

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
    if (m.generation != generation || !connected) {
        CRITICAL_REGION_ENTER();
        m.generation = generation;
        if (!connected || m.request_generation != generation) m.pending = false;
        m.reply_pending = false;
        m.previous_length = 0;
        CRITICAL_REGION_EXIT();
        resource_protocol_reset();
    }
    if (!connected) return;
    if (resource_store_active() && (uint32_t)(now - m.last_activity) >= 60U) {
        resource_protocol_reset();
        m.previous_length = 0;
        m.reply_pending = false;
    }
    if (m.pending && idle) {
        CRITICAL_REGION_ENTER();
        m.running = true;
        m.pending = false;
        CRITICAL_REGION_EXIT();
        if (m.previous_length != m.length || memcmp(m.previous, m.request, m.length)) {
            m.reply_length = resource_protocol_execute(m.request, m.length, m.reply);
            memcpy(m.previous, m.request, m.length);
            m.previous_length = m.length;
        }
        m.last_activity = now;
        m.reply_pending = true;
        m.running = false;
    }
    if (m.reply_pending) {
        /* Keep the reply on queue-full and transient failures. A disconnect
         * clears it in the next main turn; RX never performs SPI or HVX. */
        if (send(context, generation, m.reply, m.reply_length) == NRF_SUCCESS) m.reply_pending = false;
    }
}
