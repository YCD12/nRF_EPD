#ifndef CALENDAR_ANNOTATIONS_STORE_H
#define CALENDAR_ANNOTATIONS_STORE_H

#include <stdbool.h>

typedef void (*calendar_annotations_store_applied_fn)(void);

void calendar_annotations_store_init(void);
void calendar_annotations_store_on_applied(calendar_annotations_store_applied_fn fn);
void calendar_annotations_store_save(void);
bool calendar_annotations_store_clear(void);
bool calendar_annotations_store_busy(void);

#endif
