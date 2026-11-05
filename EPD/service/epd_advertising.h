#ifndef EPD_ADVERTISING_H
#define EPD_ADVERTISING_H

#include "ble_advertising.h"

/* Startup-only adapter for SDK 17.1: legacy Flags + GAP name, with one
 * service UUID in scan response. The instance must be zero-initialized.
 * SDK ble_advertising still owns start, stop, reconnect and DFU behavior. */
uint32_t epd_advertising_init(ble_advertising_t *advertising,
                             const ble_uuid_t *service_uuid,
                             uint32_t interval, uint16_t timeout,
                             uint8_t conn_cfg_tag,
                             ble_adv_evt_handler_t handler);

#endif
