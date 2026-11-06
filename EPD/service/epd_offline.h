#ifndef EPD_OFFLINE_H
#define EPD_OFFLINE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EPD_ONLINE, EPD_OFFLINE_ACK, EPD_OFFLINE_DRAIN, EPD_OFFLINE_READY, EPD_OFFLINE_ACTIVE
} epd_offline_state_t;
typedef struct {
    volatile uint16_t notifications;
    volatile epd_offline_state_t state;
} epd_offline_t;

/* Offline means disconnected from the central, with the normal advertising
 * policy unchanged. Serialize queued/completed updates in the critical region. */
static inline void epd_offline_queued(epd_offline_t *s, bool final_ack)
{
    ++s->notifications;
    if (final_ack) s->state = EPD_OFFLINE_DRAIN;
}

static inline void epd_offline_completed(epd_offline_t *s, uint16_t count)
{
    s->notifications = count < s->notifications ? s->notifications - count : 0U;
    if (s->state == EPD_OFFLINE_DRAIN && !s->notifications) s->state = EPD_OFFLINE_READY;
}

static inline void epd_offline_disconnected(epd_offline_t *s)
{
    s->notifications = 0U;
    if (s->state != EPD_OFFLINE_ACTIVE)
        s->state = s->state >= EPD_OFFLINE_DRAIN ? EPD_OFFLINE_READY : EPD_ONLINE;
}

#endif
