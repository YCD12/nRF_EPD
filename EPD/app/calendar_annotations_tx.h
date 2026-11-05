#ifndef CALENDAR_ANNOTATIONS_TX_H
#define CALENDAR_ANNOTATIONS_TX_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CALENDAR_ANNO_TX_SENT = 0,
    CALENDAR_ANNO_TX_BLOCKED,
    CALENDAR_ANNO_TX_FAILED,
} calendar_annotations_tx_result_t;

typedef calendar_annotations_tx_result_t (*calendar_annotations_tx_send_fn)(
    void *context, uint8_t *data, uint16_t length);

typedef struct {
    volatile uint32_t generation;
    volatile uint16_t offset;
    volatile bool active;
    volatile bool waiting_tx;
} calendar_annotations_tx_t;

void calendar_annotations_tx_start(calendar_annotations_tx_t *tx);
void calendar_annotations_tx_cancel(calendar_annotations_tx_t *tx);
void calendar_annotations_tx_wake(calendar_annotations_tx_t *tx);
void calendar_annotations_tx_process(calendar_annotations_tx_t *tx,
                                     uint16_t max_packet_len,
                                     calendar_annotations_tx_send_fn send,
                                     void *context);

#endif
