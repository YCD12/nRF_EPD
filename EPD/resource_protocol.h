#ifndef RESOURCE_PROTOCOL_H
#define RESOURCE_PROTOCOL_H
#include <stdbool.h>
#include "resource_store.h"

#define RESOURCE_PACKET_MAX 20U
/* Called in main only. The transport supplies stop-and-wait duplicate caching. */
uint8_t resource_protocol_execute(const uint8_t *request, uint8_t length, uint8_t *reply);
void resource_protocol_reset(void);
bool resource_protocol_entered(void);
#endif
