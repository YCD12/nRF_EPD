#include <stdint.h>

uint32_t timestamp(void);
void set_timestamp(uint32_t timestamp);
void sleep_mode_enter(void);
void app_feed_wdt(void);
void ble_log(const char *format, ...);
void ble_log_hexdump(const void *p_data, uint16_t len);
uint32_t get_reset_reason(void);
