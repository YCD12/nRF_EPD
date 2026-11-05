/**
 * Copyright (c) 2015 - 2021, Nordic Semiconductor ASA
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/* SDK 17.1 startup adapter, specialized for the EPD advertising payload. */
#include "epd_advertising.h"

uint32_t epd_advertising_init(ble_advertising_t *advertising,
                             const ble_uuid_t *service_uuid,
                             uint32_t interval, uint16_t timeout,
                             uint8_t conn_cfg_tag,
                             ble_adv_evt_handler_t handler)
{
    if (advertising == NULL || service_uuid == NULL) return NRF_ERROR_NULL;
    if (advertising->initialized) return NRF_ERROR_INVALID_STATE;

    uint8_t *adv = advertising->enc_advdata[0];
    uint8_t *scan = advertising->enc_scan_rsp_data[0];
    const uint16_t name_capacity = BLE_GAP_ADV_SET_DATA_SIZE_MAX - 5U;
    uint16_t name_length = name_capacity;
    uint32_t err = sd_ble_gap_device_name_get(adv + 5, &name_length);
    if (err != NRF_SUCCESS) return err;

    adv[0] = 2;
    adv[1] = BLE_GAP_AD_TYPE_FLAGS;
    adv[2] = BLE_GAP_ADV_FLAGS_LE_ONLY_LIMITED_DISC_MODE;
    adv[4] = name_length <= name_capacity ? BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME
                                        : BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME;
    if (name_length > name_capacity) name_length = name_capacity;
    adv[3] = (uint8_t)(name_length + 1U);

    /* Ask SoftDevice for the registered UUID bytes; vendor type IDs are not
     * UUID bytes and must never be emitted as a 16-bit service UUID. */
    uint8_t uuid_length = 0;
    err = sd_ble_uuid_encode(service_uuid, &uuid_length, NULL);
    if (err != NRF_SUCCESS) return err;
    if (uuid_length != 2U && uuid_length != 16U) return NRF_ERROR_INVALID_PARAM;
    err = sd_ble_uuid_encode(service_uuid, &uuid_length, scan + 2);
    if (err != NRF_SUCCESS) return err;
    scan[0] = (uint8_t)(uuid_length + 1U);
    scan[1] = uuid_length == 2U ? BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE
                              : BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE;

    advertising->adv_mode_current = BLE_ADV_MODE_IDLE;
    advertising->adv_modes_config.ble_adv_fast_enabled = true;
    advertising->adv_modes_config.ble_adv_fast_interval = interval;
    advertising->adv_modes_config.ble_adv_fast_timeout = timeout;
    advertising->conn_cfg_tag = conn_cfg_tag;
    advertising->evt_handler = handler;
    advertising->current_slave_link_conn_handle = BLE_CONN_HANDLE_INVALID;
    advertising->adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
    advertising->adv_data.adv_data.p_data = adv;
    advertising->adv_data.adv_data.len = name_length + 5U;
    advertising->adv_data.scan_rsp_data.p_data = scan;
    advertising->adv_data.scan_rsp_data.len = uuid_length + 2U;
    advertising->p_adv_data = &advertising->adv_data;

    advertising->adv_params.primary_phy = BLE_GAP_PHY_1MBPS;
    advertising->adv_params.duration = timeout;
    advertising->adv_params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    advertising->adv_params.filter_policy = BLE_GAP_ADV_FP_ANY;
    advertising->adv_params.interval = interval;
    err = sd_ble_gap_adv_set_configure(&advertising->adv_handle, NULL,
                                      &advertising->adv_params);
    if (err == NRF_SUCCESS) advertising->initialized = true;
    return err;
}
