#ifndef RESOURCE_PROTOCOL_H
#define RESOURCE_PROTOCOL_H
#include <stdbool.h>
#include "resource_store.h"

#define RESOURCE_PACKET_MAX 20U
enum {
    RESOURCE_CAP_WRITE = 1U,
    RESOURCE_CAP_ACTIVATE = 2U,
    RESOURCE_CAP_OFFLINE = 4U
};
typedef enum { RESOURCE_ENTER, RESOURCE_INVALIDATE, RESOURCE_ACTIVATE, RESOURCE_OFFLINE } resource_event_t;
typedef rs_result_t (*resource_event_handler_t)(void *context, resource_event_t event,
                                               uint16_t changed_partitions);
/* The application owns activation; the wire protocol only owns transactions. */
void resource_protocol_set_handler(resource_event_handler_t handler, void *context);
/* Called in main only. The transport supplies stop-and-wait duplicate caching. */
uint8_t resource_protocol_execute(const uint8_t *request, uint8_t length, uint8_t *reply);
void resource_protocol_reset(void);
bool resource_protocol_entered(void);
#endif
