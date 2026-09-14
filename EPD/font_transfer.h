#ifndef FONT_TRANSFER_H
#define FONT_TRANSFER_H

#ifdef FONT_TRANSFER_HOST_TEST
#include "font_transfer_test_platform.h"
#else
#include "EPD_service.h"
#endif
#include "font_store.h"

#define FONT_STORE_OP_GET          0x00U
#define FONT_STORE_OP_PACK_BEGIN   0x06U
#define FONT_STORE_OP_PACK_DATA    0x07U
#define FONT_STORE_OP_PACK_COMMIT  0x08U
#define FONT_STORE_OP_PACK_ABORT   0x09U
#define FONT_STORE_OP_STREAM_DATA  0x0AU
#define FONT_STORE_IDLE_TIMEOUT_S  30U
#define FONT_STORE_BEGIN_RATE_LIMIT_S 60U

void cmd_font_store(ble_epd_t* epd, uint8_t* data, uint16_t length);
void font_store_send_list(ble_epd_t* epd);
void font_store_send_ack(ble_epd_t* epd, uint8_t operation, font_store_result_t result, uint8_t slot);
void font_stream_reset(ble_epd_t* epd);
bool font_task_schedule(ble_epd_t* epd, uint8_t task);
void font_task_wake_tx(ble_epd_t* epd, uint8_t task);
void font_task_retry(ble_epd_t* epd);

/* Service integration: neither hook is invoked while holding a font lock. */
uint32_t ble_epd_notification_try_send(ble_epd_t* epd, uint8_t* data, uint16_t length);
void epd_refresh_fonts(ble_epd_t* epd);
uint16_t crc16_compute(const uint8_t* data, uint16_t length);

#endif
