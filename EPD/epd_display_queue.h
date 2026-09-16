#ifndef EPD_DISPLAY_QUEUE_H
#define EPD_DISPLAY_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_scheduler.h"
#include "app_util_platform.h"

typedef struct {
    volatile uint32_t timestamp;
    volatile bool pending;
    volatile bool scheduled;
    volatile bool processing;
} epd_display_queue_t;

static inline void epd_display_queue_init(epd_display_queue_t *queue)
{
    queue->timestamp = 0U;
    queue->pending = false;
    queue->scheduled = false;
    queue->processing = false;
}

static inline void epd_display_queue_request(epd_display_queue_t *queue,
                                             uint32_t timestamp)
{
    CRITICAL_REGION_ENTER();
    queue->timestamp = timestamp;
    queue->pending = true;
    CRITICAL_REGION_EXIT();
}

static inline void epd_display_queue_schedule(epd_display_queue_t *queue,
                                              void *context,
                                              app_sched_event_handler_t worker)
{
    bool post = false;
    CRITICAL_REGION_ENTER();
    if (queue->pending && !queue->scheduled && !queue->processing) {
        queue->scheduled = true;
        post = true;
    }
    CRITICAL_REGION_EXIT();
    if (post && app_sched_event_put(&context, sizeof(context), worker) != 0U) {
        CRITICAL_REGION_ENTER();
        queue->scheduled = false;
        CRITICAL_REGION_EXIT();
    }
}

static inline bool epd_display_queue_begin(epd_display_queue_t *queue,
                                           uint32_t *timestamp)
{
    bool ready = false;
    CRITICAL_REGION_ENTER();
    queue->scheduled = false;
    if (queue->pending && !queue->processing) {
        *timestamp = queue->timestamp;
        queue->pending = false;
        queue->processing = true;
        ready = true;
    }
    CRITICAL_REGION_EXIT();
    return ready;
}

static inline void epd_display_queue_finish(epd_display_queue_t *queue)
{
    CRITICAL_REGION_ENTER();
    queue->processing = false;
    CRITICAL_REGION_EXIT();
}

#endif
