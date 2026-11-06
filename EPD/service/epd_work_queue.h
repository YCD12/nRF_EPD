#ifndef EPD_WORK_QUEUE_H
#define EPD_WORK_QUEUE_H

#include <stdbool.h>
#include "app_scheduler.h"
#include "app_util_platform.h"

/* The caller reserves its scheduled flag atomically before posting. Keep the
 * pointer copy and failed-post rollback identical for command/display work. */
static inline void epd_work_post(volatile bool *scheduled, void *context,
                                 app_sched_event_handler_t worker)
{
    if (app_sched_event_put(&context, sizeof(context), worker) != 0U) {
        CRITICAL_REGION_ENTER();
        *scheduled = false; /* Main loop / next tick retries. */
        CRITICAL_REGION_EXIT();
    }
}

#endif
