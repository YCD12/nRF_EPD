#include <stdint.h>
typedef void (*app_sched_event_handler_t)(void*, uint16_t);
uint32_t app_sched_event_put(void* data, uint16_t length, app_sched_event_handler_t handler);
