#ifndef CALENDAR_ANNOTATIONS_STORE_H
#define CALENDAR_ANNOTATIONS_STORE_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*calendar_annotations_store_applied_fn)(void);

void calendar_annotations_store_init(void);
void calendar_annotations_store_on_applied(calendar_annotations_store_applied_fn fn);
void calendar_annotations_store_save(void);
void calendar_annotations_store_process(void);
bool calendar_annotations_store_clear(void);
bool calendar_annotations_store_busy(void);
/* A deferred read can pause for resource repair; active RX or writes cannot. */
bool calendar_annotations_store_maintenance_ready(void);
bool calendar_annotations_store_valid(void);
bool calendar_annotations_store_take_save_result(uint8_t* result, uint32_t* revision,
                                                 uint32_t* data_crc);

#endif
