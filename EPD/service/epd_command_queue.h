#ifndef EPD_COMMAND_QUEUE_H
#define EPD_COMMAND_QUEUE_H

#include <string.h>
#include "epd_work_queue.h"

/* The wire limits are supplied by the service. */
typedef struct {
    uint8_t data[EPD_CMD_MAX_DATA_LEN];
    uint16_t length;
} epd_cmd_queue_item_t;
typedef struct {
    epd_cmd_queue_item_t items[EPD_CMD_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    volatile uint8_t count;
    volatile bool is_processing;
    volatile bool is_scheduled;
} epd_cmd_queue_t;

static inline void epd_cmd_queue_init(epd_cmd_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

static inline bool epd_cmd_queue_push(epd_cmd_queue_t *queue,
                                       const uint8_t *data, uint16_t length)
{
    bool accepted = false;
    if (data == NULL || length == 0U || length > EPD_CMD_MAX_DATA_LEN) return false;
    CRITICAL_REGION_ENTER();
    if (queue->count < EPD_CMD_QUEUE_SIZE) {
        epd_cmd_queue_item_t *item = &queue->items[queue->tail];
        item->length = length;
        memcpy(item->data, data, length);
        queue->tail = (queue->tail + 1U) % EPD_CMD_QUEUE_SIZE;
        queue->count++;
        accepted = true;
    }
    CRITICAL_REGION_EXIT();
    return accepted;
}

static inline bool epd_cmd_queue_pop(epd_cmd_queue_t *queue, epd_cmd_queue_item_t *item)
{
    bool available = false;
    CRITICAL_REGION_ENTER();
    if (queue->count != 0U) {
        /* Keep the two waiting slots available during slow command execution.
         * Only the bounded packet copy runs with interrupts masked. */
        memcpy(item, &queue->items[queue->head], sizeof(*item));
        queue->head = (queue->head + 1U) % EPD_CMD_QUEUE_SIZE;
        queue->count--;
        available = true;
    }
    CRITICAL_REGION_EXIT();
    return available;
}

static inline void epd_cmd_queue_schedule(epd_cmd_queue_t *queue, void *context,
                                           app_sched_event_handler_t worker)
{
    bool post = false;
    CRITICAL_REGION_ENTER();
    if (queue->count != 0U && !queue->is_processing && !queue->is_scheduled) {
        queue->is_scheduled = true;
        post = true;
    }
    CRITICAL_REGION_EXIT();
    if (post) epd_work_post(&queue->is_scheduled, context, worker);
}

#endif
