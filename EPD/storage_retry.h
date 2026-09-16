#ifndef STORAGE_RETRY_H
#define STORAGE_RETRY_H
#include <stdbool.h>
#include <stdint.h>
bool epd_storage_gc_pending(void);
bool epd_storage_retry(uint32_t result, uint8_t *attempts, uint32_t *due);
uint32_t epd_storage_init(void);
void epd_storage_process(void);
#endif
