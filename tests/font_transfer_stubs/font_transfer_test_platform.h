#ifndef FONT_TRANSFER_TEST_PLATFORM_H
#define FONT_TRANSFER_TEST_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define BLE_EPD_MAX_DATA_LEN 244U
#define EPD_CMD_MAX_DATA_LEN BLE_EPD_MAX_DATA_LEN
#define BLE_CONN_HANDLE_INVALID UINT16_MAX
#define EPD_CMD_FONT_STORE 0x25U
#define EPD_RSP_FONT_ACK 0xA4U
#define EPD_RSP_FONT_LIST 0xA3U
#define EPD_RSP_FONT_STREAM_ACK 0xA5U
#include "font_transfer_state.h"
typedef struct {
    uint16_t conn_handle;
    font_transfer_state_t font;
} ble_epd_t;
#endif
