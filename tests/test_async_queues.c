#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "calendar_annotations.h"
#include "calendar_annotations_tx.h"
#include "epd_display_queue.h"

static unsigned critical_depth;
static bool scheduler_full;
static unsigned scheduler_posts;
static app_sched_event_handler_t scheduled_worker;
static void *scheduled_context;

void test_critical_enter(void) { critical_depth++; }
void test_critical_exit(void) { assert(critical_depth); critical_depth--; }
uint32_t app_sched_event_put(void *data, uint16_t length,
                             app_sched_event_handler_t worker)
{
    assert(critical_depth == 0U && length == sizeof(void *));
    scheduler_posts++;
    if (scheduler_full) return 4U;
    scheduled_worker = worker;
    memcpy(&scheduled_context, data, length);
    return 0U;
}

static void worker(void *data, uint16_t size) { (void)data; (void)size; }

static void test_display_queue_retries_and_coalesces(void)
{
    epd_display_queue_t queue;
    uint32_t timestamp;
    int context;

    epd_display_queue_init(&queue);
    epd_display_queue_request(&queue, 10U);
    scheduler_full = true;
    epd_display_queue_schedule(&queue, &context, worker);
    assert(queue.pending && !queue.scheduled && scheduler_posts == 1U);

    scheduler_full = false;
    epd_display_queue_schedule(&queue, &context, worker);
    assert(queue.pending && queue.scheduled && scheduled_worker == worker);
    assert(scheduled_context == &context);
    assert(epd_display_queue_begin(&queue, &timestamp) && timestamp == 10U);
    assert(queue.processing && !queue.pending);

    epd_display_queue_request(&queue, 11U);
    epd_display_queue_request(&queue, 12U);
    epd_display_queue_schedule(&queue, &context, worker);
    assert(scheduler_posts == 2U); /* A running display owns the worker slot. */
    epd_display_queue_finish(&queue);
    epd_display_queue_schedule(&queue, &context, worker);
    assert(scheduler_posts == 3U && queue.scheduled);
    assert(epd_display_queue_begin(&queue, &timestamp) && timestamp == 12U);
    epd_display_queue_finish(&queue);
}

static unsigned send_attempts;
static unsigned sent_payload;
static bool block_once;
static bool fail_once;
static unsigned interrupt_action;

static calendar_annotations_tx_result_t send_packet(void *context,
                                                     uint8_t *data,
                                                     uint16_t length)
{
    assert(critical_depth == 0U);
    if (interrupt_action == 1U) calendar_annotations_tx_wake(context);
    if (interrupt_action == 2U) calendar_annotations_tx_start(context);
    if (interrupt_action == 3U) calendar_annotations_tx_cancel(context);
    assert(data[0] == CALENDAR_ANNO_NOTIFY);
    send_attempts++;
    if (fail_once) {
        fail_once = false;
        return CALENDAR_ANNO_TX_FAILED;
    }
    if (block_once) {
        block_once = false;
        return CALENDAR_ANNO_TX_BLOCKED;
    }
    sent_payload += length - CALENDAR_ANNO_NOTIFY_HDR;
    return CALENDAR_ANNO_TX_SENT;
}

static void test_annotation_tx_resumes_after_backpressure(void)
{
    uint8_t blob[48] = {0};
    const uint8_t *empty;
    calendar_annotations_tx_t tx = {0};

    calendar_annotations_reset();
    assert(calendar_annotations_blob(&empty) == CALENDAR_ANNOTATIONS_HEADER_SIZE);
    memcpy(blob, empty, CALENDAR_ANNOTATIONS_HEADER_SIZE);
    blob[6] = 2U;
    for (unsigned i = 0; i < 2U; i++) {
        unsigned off = CALENDAR_ANNOTATIONS_HEADER_SIZE +
                       i * CALENDAR_ANNOTATIONS_RECORD_SIZE;
        blob[off] = (uint8_t)2026U;
        blob[off + 1U] = (uint8_t)(2026U >> 8);
        blob[off + 2U] = 9U;
        blob[off + 3U] = (uint8_t)(i + 1U);
        blob[off + 4U] = 1U;
    }
    assert(calendar_annotations_load(blob, sizeof(blob)));

    block_once = true;
    calendar_annotations_tx_start(&tx);
    calendar_annotations_tx_process(&tx, 20U, send_packet, NULL);
    assert(tx.active && tx.waiting_tx && tx.offset == 0U);
    calendar_annotations_tx_wake(&tx);
    while (tx.active) {
        calendar_annotations_tx_process(&tx, 20U, send_packet, NULL);
        if (tx.waiting_tx) calendar_annotations_tx_wake(&tx);
    }
    assert(sent_payload == sizeof(blob));
    assert(send_attempts == 5U); /* One blocked try plus four payload chunks. */

    fail_once = true;
    calendar_annotations_tx_start(&tx);
    calendar_annotations_tx_process(&tx, 20U, send_packet, NULL);
    assert(!tx.active && !tx.waiting_tx && tx.offset == 0U);

    calendar_annotations_tx_start(&tx);
    interrupt_action = 1U; /* HVN completion arrives before send returns. */
    calendar_annotations_tx_process(&tx, 20U, send_packet, &tx);
    assert(tx.active && !tx.waiting_tx && tx.offset > 0U);
    block_once = true;
    unsigned offset = tx.offset;
    calendar_annotations_tx_process(&tx, 20U, send_packet, &tx);
    assert(tx.active && !tx.waiting_tx && tx.offset == offset);
    interrupt_action = 2U; /* A new GET supersedes the in-flight send. */
    calendar_annotations_tx_process(&tx, 20U, send_packet, &tx);
    assert(tx.active && !tx.waiting_tx && tx.offset == 0U);
    interrupt_action = 3U; /* Disconnect supersedes the in-flight send. */
    calendar_annotations_tx_process(&tx, 20U, send_packet, &tx);
    assert(!tx.active && !tx.waiting_tx && tx.offset == 0U);
    interrupt_action = 0U;
}

int main(void)
{
    test_display_queue_retries_and_coalesces();
    test_annotation_tx_resumes_after_backpressure();
    puts("PASS: display request retry/coalescing and annotation TX backpressure");
    return 0;
}
