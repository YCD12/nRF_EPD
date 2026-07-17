#ifndef UC8179_H
#define UC8179_H

#include <stdbool.h>

/* Chip-specific feature flags — called directly by EPD_service where
 * the vtable does not yet have a corresponding slot. */
void UC8179_Set_Partial_Strong_BW_Allowed(bool allowed);
bool UC8179_HadBusyTimeout(void);
void UC8179_ClearBusyTimeout(void);

#endif /* UC8179_H */
