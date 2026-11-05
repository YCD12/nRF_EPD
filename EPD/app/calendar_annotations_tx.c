#include "calendar_annotations_tx.h"

#include "calendar_annotations.h"

#if CALENDAR_ANNOTATIONS_USE_NRF_CRITICAL
#include "app_util_platform.h"
#else
#define CRITICAL_REGION_ENTER() {
#define CRITICAL_REGION_EXIT() }
#endif

void calendar_annotations_tx_start(calendar_annotations_tx_t *tx)
{
    if (tx == NULL) return;
    CRITICAL_REGION_ENTER();
    tx->generation++;
    tx->offset = 0U;
    tx->active = true;
    tx->waiting_tx = false;
    CRITICAL_REGION_EXIT();
}

void calendar_annotations_tx_cancel(calendar_annotations_tx_t *tx)
{
    if (tx == NULL) return;
    CRITICAL_REGION_ENTER();
    tx->generation++;
    tx->offset = 0U;
    tx->active = false;
    tx->waiting_tx = false;
    CRITICAL_REGION_EXIT();
}

void calendar_annotations_tx_wake(calendar_annotations_tx_t *tx)
{
    if (tx != NULL && tx->active) tx->waiting_tx = false;
}

void calendar_annotations_tx_process(calendar_annotations_tx_t *tx,
                                     uint16_t max_packet_len,
                                     calendar_annotations_tx_send_fn send,
                                     void *context)
{
    uint8_t packet[244];
    uint16_t length = 0U;
    uint32_t generation = 0U;
    calendar_annotations_tx_result_t result;

    if (tx == NULL || send == NULL) return;
    if (max_packet_len > sizeof(packet)) max_packet_len = sizeof(packet);
    CRITICAL_REGION_ENTER();
    if (tx->active && !tx->waiting_tx) {
        generation = tx->generation;
        length = calendar_annotations_notify_chunk(tx->offset, packet, max_packet_len);
        if (length <= CALENDAR_ANNO_NOTIFY_HDR) calendar_annotations_tx_cancel(tx);
        else tx->waiting_tx = true; /* Register before HVX can complete. */
    }
    CRITICAL_REGION_EXIT();
    if (length <= CALENDAR_ANNO_NOTIFY_HDR) return;

    result = send(context, packet, length);
    CRITICAL_REGION_ENTER();
    if (tx->generation == generation && tx->active) {
        if (result == CALENDAR_ANNO_TX_SENT) {
            tx->offset = (uint16_t)(tx->offset + length - CALENDAR_ANNO_NOTIFY_HDR);
            if (tx->offset >= calendar_annotations_blob(NULL)) {
                calendar_annotations_tx_cancel(tx);
            }
        } else if (result != CALENDAR_ANNO_TX_BLOCKED) {
            calendar_annotations_tx_cancel(tx);
        }
    }
    CRITICAL_REGION_EXIT();
}
