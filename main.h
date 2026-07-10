#include <stdint.h>

uint32_t timestamp(void);
void set_timestamp(uint32_t timestamp);
void sleep_mode_enter(void);
void app_feed_wdt(void);
#ifndef BLE_LOG_REDIRECT_ALL_NRF_LOG
#define BLE_LOG_REDIRECT_ALL_NRF_LOG 0
#endif

#if BLE_LOG_REDIRECT_ALL_NRF_LOG
void ble_log(const char *format, ...);
void ble_log_hexdump(const void *p_data, uint16_t len);
#else
#define ble_log(...) do { } while(0)
#define ble_log_hexdump(...) do { } while(0)
#endif
uint32_t get_reset_reason(void);
