/* Compare against the actual SDK encoder and advertising state machine.
 * Only the SoftDevice calls and hardware utility headers are host shims. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "epd_advertising.h"

static uint8_t device_name[248];
static uint16_t device_name_length;
static uint32_t name_error, uuid_error, configure_error;
static unsigned uuid_calls, uuid_fail_call, configure_calls, start_calls, stop_calls;
static bool bad_uuid_length;
static ble_gap_adv_params_t configured_params;
static const ble_gap_adv_data_t *configured_data;
static ble_adv_evt_t events[32];
static unsigned event_count;
static const ble_uuid_t vendor_uuid = {1, BLE_UUID_TYPE_VENDOR_BEGIN};

uint32_t sd_ble_gap_device_name_get(uint8_t *out, uint16_t *length)
{
    if (name_error) return name_error;
    uint16_t copied = *length < device_name_length ? *length : device_name_length;
    memcpy(out, device_name, copied);
    *length = device_name_length;
    return NRF_SUCCESS;
}

uint32_t sd_ble_uuid_encode(const ble_uuid_t *uuid, uint8_t *length, uint8_t *out)
{
    ++uuid_calls;
    if (uuid_error && uuid_calls == uuid_fail_call) return uuid_error;
    if (uuid->type != BLE_UUID_TYPE_BLE && uuid->type != BLE_UUID_TYPE_VENDOR_BEGIN)
        return NRF_ERROR_INVALID_PARAM;
    *length = bad_uuid_length ? 4 : uuid->type == BLE_UUID_TYPE_BLE ? 2 : 16;
    if (out != NULL) {
        if (*length == 16) {
            const uint8_t base[16] = {0xec, 0x5a, 0x67, 0x1c, 0xc1, 0xb6, 0x46, 0xfb,
                                      0x8d, 0x91, 0x28, 0xd8, 0x22, 0x36, 0x75, 0x62};
            memcpy(out, base, 16);
        }
        unsigned offset = *length == 16 ? 12 : 0;
        out[offset] = (uint8_t)uuid->uuid;
        out[offset + 1] = (uint8_t)(uuid->uuid >> 8);
    }
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_addr_get(ble_gap_addr_t *addr)
{
    (void)addr;
    assert(!"unexpected address encoding");
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t sd_ble_gap_appearance_get(uint16_t *appearance)
{
    (void)appearance;
    assert(!"unexpected appearance encoding");
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t sd_ble_gap_adv_set_configure(uint8_t *handle, const ble_gap_adv_data_t *data,
                                    const ble_gap_adv_params_t *params)
{
    ++configure_calls;
    if (configure_error) return configure_error;
    assert(params != NULL);
    configured_params = *params;
    configured_data = data;
    *handle = 0;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_adv_start(uint8_t handle, uint8_t tag)
{
    assert(handle == 0 && tag == 1);
    ++start_calls;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_adv_stop(uint8_t handle)
{
    assert(handle == 0);
    ++stop_calls;
    return NRF_SUCCESS;
}

static void on_adv(ble_adv_evt_t event)
{
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = event;
}

static void reset_stubs(void)
{
    name_error = uuid_error = configure_error = 0;
    uuid_calls = uuid_fail_call = configure_calls = start_calls = stop_calls = 0;
    bad_uuid_length = false;
    configured_data = NULL;
    event_count = 0;
}

static uint32_t compact_init(ble_advertising_t *adv, const ble_uuid_t *uuid)
{
    return epd_advertising_init(adv, uuid, 400, 12000, 1, on_adv);
}

static void compare_sdk(uint16_t name_length, const ble_uuid_t *uuid)
{
    ble_advertising_t reference = {0}, compact = {0};
    ble_advertising_init_t init = {0};
    reset_stubs();
    device_name_length = name_length;
    for (unsigned i = 0; i < sizeof(device_name); ++i) device_name[i] = (uint8_t)('A' + i % 26);
    init.advdata.name_type = BLE_ADVDATA_FULL_NAME;
    init.advdata.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_LIMITED_DISC_MODE;
    init.srdata.uuids_complete.uuid_cnt = 1;
    init.srdata.uuids_complete.p_uuids = (ble_uuid_t *)uuid;
    init.config.ble_adv_fast_enabled = true;
    init.config.ble_adv_fast_interval = 400;
    init.config.ble_adv_fast_timeout = 12000;
    init.evt_handler = on_adv;
    assert(ble_advertising_init(&reference, &init) == NRF_SUCCESS);
    ble_advertising_conn_cfg_tag_set(&reference, 1);
    assert(compact_init(&compact, uuid) == NRF_SUCCESS);
    assert(configure_calls == 2 && configured_data == NULL);
    assert(compact.adv_data.adv_data.len == reference.adv_data.adv_data.len);
    assert(compact.adv_data.scan_rsp_data.len == reference.adv_data.scan_rsp_data.len);
    assert(memcmp(compact.enc_advdata[0], reference.enc_advdata[0], reference.adv_data.adv_data.len) == 0);
    assert(memcmp(compact.enc_scan_rsp_data[0], reference.enc_scan_rsp_data[0], reference.adv_data.scan_rsp_data.len) == 0);
    assert(compact.adv_data.adv_data.len <= 31);
    assert(compact.enc_advdata[0][4] == (name_length > 26 ? BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME
                                                       : BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME));
    assert(compact.enc_scan_rsp_data[0][1] == (uuid->type == BLE_UUID_TYPE_BLE
           ? BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE : BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE));
    /* Normalize instance-local pointers before comparing the complete SDK state. */
    ble_advertising_t normalized = compact;
    normalized.adv_data.adv_data.p_data = reference.adv_data.adv_data.p_data;
    normalized.adv_data.scan_rsp_data.p_data = reference.adv_data.scan_rsp_data.p_data;
    normalized.p_adv_data = reference.p_adv_data;
    assert(memcmp(&normalized, &reference, sizeof(reference)) == 0);
    ble_advertising_t before = compact;
    assert(compact_init(&compact, uuid) == NRF_ERROR_INVALID_STATE);
    assert(memcmp(&before, &compact, sizeof(compact)) == 0);
}

static void failures(void)
{
    assert(compact_init(NULL, &vendor_uuid) == NRF_ERROR_NULL);
    ble_advertising_t null_uuid = {0};
    assert(compact_init(&null_uuid, NULL) == NRF_ERROR_NULL);
    for (unsigned failure = 0; failure < 5; ++failure) {
        ble_advertising_t adv = {0};
        reset_stubs();
        uint32_t expected = NRF_ERROR_BUSY;
        if (failure == 0) name_error = expected;
        if (failure == 1 || failure == 2) { uuid_error = expected; uuid_fail_call = failure; }
        if (failure == 3) configure_error = expected;
        if (failure == 4) { bad_uuid_length = true; expected = NRF_ERROR_INVALID_PARAM; }
        assert(compact_init(&adv, &vendor_uuid) == expected);
        assert(!adv.initialized && !start_calls);
        assert(configure_calls == (failure == 3 ? 1U : 0U));
        reset_stubs();
        assert(compact_init(&adv, &vendor_uuid) == NRF_SUCCESS);
    }
    ble_advertising_t adv = {0};
    const ble_uuid_t invalid = {1, BLE_UUID_TYPE_UNKNOWN};
    reset_stubs();
    assert(compact_init(&adv, &invalid) == NRF_ERROR_INVALID_PARAM);
    assert(!adv.initialized && configure_calls == 0);
}

static void lifecycle(void)
{
    ble_advertising_t adv = {0};
    ble_evt_t event = {0};
    reset_stubs();
    assert(compact_init(&adv, &vendor_uuid) == NRF_SUCCESS);
    assert(ble_advertising_start(&adv, BLE_ADV_MODE_FAST) == NRF_SUCCESS);
    assert(start_calls == 1 && configured_data == &adv.adv_data);
    assert(configured_params.interval == 400 && configured_params.duration == 12000);
    assert(configured_params.properties.type == BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
    assert(events[event_count - 1] == BLE_ADV_EVT_FAST);

    event.header.evt_id = BLE_GAP_EVT_CONNECTED;
    event.evt.gap_evt.conn_handle = 7;
    event.evt.gap_evt.params.connected.role = BLE_GAP_ROLE_PERIPH;
    ble_advertising_on_ble_evt(&event, &adv);
    event.header.evt_id = BLE_GAP_EVT_DISCONNECTED;
    ble_advertising_on_ble_evt(&event, &adv);
    assert(start_calls == 2 && adv.adv_mode_current == BLE_ADV_MODE_FAST);

    event.header.evt_id = BLE_GAP_EVT_ADV_SET_TERMINATED;
    event.evt.gap_evt.params.adv_set_terminated.reason = BLE_GAP_EVT_ADV_SET_TERMINATED_REASON_TIMEOUT;
    ble_advertising_on_ble_evt(&event, &adv);
    assert(adv.adv_mode_current == BLE_ADV_MODE_IDLE);
    assert(events[event_count - 1] == BLE_ADV_EVT_IDLE);

    /* Same configuration update used when entering buttonless DFU. */
    ble_adv_modes_config_t config = adv.adv_modes_config;
    config.ble_adv_on_disconnect_disabled = true;
    ble_advertising_modes_config_set(&adv, &config);
    unsigned starts_before_dfu = start_calls;
    event.header.evt_id = BLE_GAP_EVT_CONNECTED;
    event.evt.gap_evt.params.connected.role = BLE_GAP_ROLE_PERIPH;
    ble_advertising_on_ble_evt(&event, &adv);
    event.header.evt_id = BLE_GAP_EVT_DISCONNECTED;
    ble_advertising_on_ble_evt(&event, &adv);
    assert(start_calls == starts_before_dfu);
}

int main(void)
{
    const ble_uuid_t standard_uuid = {0x180f, BLE_UUID_TYPE_BLE};
    const unsigned lengths[] = {0, 1, 7, 13, 25, 26, 27, 31, 248};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        compare_sdk(lengths[i], &vendor_uuid);
        compare_sdk(lengths[i], &standard_uuid);
    }
    failures();
    lifecycle();
    puts("advertising: SDK byte/state equivalence, failures, reconnect, timeout and DFU OK");
    return 0;
}
