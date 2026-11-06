/*!
 * @file epd_service_control.c
 * @brief EPD 服务的通知字符串、模式/显示 ACK、记录回执与配置回读
 *
 * 由 EPD/service/EPD_service.c 按职责拆出；跨模块接口声明见
 * epd_service_internal.h。只搬代码，不改行为/协议/时序。
 */

#include "EPD_service.h"
#include "epd_service_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "main.h"
#include "calendar_annotations_store.h"
#include "resource_store.h"
#include "resource_layout.h"

/* 拼 t=/v=/mtu= 通知串。 */
static size_t epd_append_char(char* out, size_t out_size, size_t pos, char value) {
    if (out_size > 0 && pos < out_size - 1) {
        out[pos] = value;
    }
    return pos + 1;
}

/* 追加无前导零的十进制，超长时只计长度、不再写入。 */
static size_t epd_append_uint(char* out, size_t out_size, size_t pos, uint32_t value) {
    char digits[10];
    uint8_t len = 0;

    do {
        digits[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && len < sizeof(digits));

    while (len > 0) {
        pos = epd_append_char(out, out_size, pos, digits[--len]);
    }
    return pos;
}

/* 生成 "t=123" / "v=3300" 这类前缀+数字串。 */
static size_t epd_make_prefixed_uint(char* out, size_t out_size, const char* prefix, uint32_t value) {
    size_t pos = 0;

    while (*prefix != '\0') {
        pos = epd_append_char(out, out_size, pos, *prefix++);
    }
    pos = epd_append_uint(out, out_size, pos, value);

    if (out_size > 0) {
        out[(pos < out_size) ? pos : (out_size - 1)] = '\0';
    }
    return (pos < out_size) ? pos : (out_size - 1);
}

/* 生成 "mtu=<n> rle=1" 能力串：网页按 parseInt(...,10) 取 MTU、按
 * includes('rle=1') 判定支持压缩块。输出上限 16B（"mtu=65535 rle=1" 为
 * 15 字符 + NUL），调用方缓冲须 >= 16B；超长时安全截断并保证 NUL。 */
static size_t epd_make_mtu_string(char* out, size_t out_size, uint16_t mtu) {
    static const char suffix[] = " rle=1";
    size_t pos = epd_make_prefixed_uint(out, out_size, "mtu=", mtu);

    for (size_t i = 0; suffix[i] != '\0'; i++) {
        pos = epd_append_char(out, out_size, pos, suffix[i]);
    }
    if (out_size > 0) {
        out[(pos < out_size) ? pos : (out_size - 1)] = '\0';
    }
    return (pos < out_size) ? pos : (out_size - 1);
}

static void epd_send_prefixed_uint(ble_epd_t *p_epd, const char *prefix, uint32_t value)
{
    /* Longest prefix (mtu=), uint32 decimal and terminator fit in 15 bytes. */
    char buf[16];
    size_t len = epd_make_prefixed_uint(buf, sizeof(buf), prefix, value);
    ble_epd_string_send(p_epd, (uint8_t *)buf, len);
}

/**@brief 发送当前时间到BLE对端
 *
 * @param[in] p_epd  EPD服务结构指针
 */

void epd_send_time(ble_epd_t* p_epd) {
    epd_send_prefixed_uint(p_epd, "t=", timestamp());
}

/**@brief 发送当前电池电压到BLE对端 */
void epd_send_voltage(ble_epd_t* p_epd) {
    epd_send_prefixed_uint(p_epd, "v=", EPD_ReadVoltageAndCache());
}

/**@brief 发送MTU大小与 RLE 能力到BLE对端
 *
 * @param[in] p_epd  EPD服务结构指针
 */
void epd_send_mtu(ble_epd_t* p_epd) {
    /* "mtu=65535 rle=1" 共 15 字符 + NUL，16B 缓冲刚好。 */
    char buf[16];
    size_t len = epd_make_mtu_string(buf, sizeof(buf), p_epd->max_data_len);
    ble_epd_string_send(p_epd, (uint8_t *)buf, len);
}

/* MODE and display completion share one reliable control-notification slot.
 * The client waits for the matching token before issuing another operation. */
void epd_command_ack(ble_epd_t *p_epd, const uint8_t *data, uint16_t length,
                            uint8_t result)
{
    if (length < 3U || (data[0] != EPD_CMD_SET_MODE &&
        data[0] != EPD_CMD_CLEAR && data[0] != EPD_CMD_REFRESH)) return;
    bool mode = data[0] == EPD_CMD_SET_MODE;
    CRITICAL_REGION_ENTER();
    p_epd->mode_ack[0] = mode ? EPD_RSP_MODE : EPD_RSP_DISPLAY;
    p_epd->mode_ack[1] = data[2];
    p_epd->mode_ack[2] = result;
    p_epd->mode_ack[3] = mode ? data[1] : data[0];
    p_epd->mode_ack_pending = true;
    CRITICAL_REGION_EXIT();
}

/* Called only from the main loop. Persistent writes and failed scheduler
 * submissions must make progress even when no further BLE commands arrive. */
uint32_t resource_notification_send(void *context, uint32_t generation,
                                           uint8_t *data, uint16_t length)
{
    ble_epd_t *p_epd = context;
    uint32_t result = NRF_ERROR_INVALID_STATE;
    CRITICAL_REGION_ENTER();
    if (p_epd->connection_generation == generation && !p_epd->disconnect_pending)
        result = ble_epd_notification_try_send(p_epd, data, length);
    else if (p_epd->offline.state == EPD_OFFLINE_ACK)
        /* A disconnect/reconnect during activation cancels the old request. */
        p_epd->offline.state = EPD_ONLINE;
    CRITICAL_REGION_EXIT();
    return result;
}

void epd_record_ack_process(ble_epd_t *p_epd)
{
    uint8_t result;
    uint32_t revision, crc;
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID || p_epd->disconnect_pending ||
        !p_epd->is_notification_enabled) {
        p_epd->record_ack_pending = false;
        (void)epd_config_take_save_result(&result, &revision, &crc);
        (void)calendar_annotations_store_take_save_result(&result, &revision, &crc);
        return;
    }
    if (!p_epd->record_ack_pending) {
        uint8_t partition = 0U;
        if (epd_config_take_save_result(&result, &revision, &crc))
            partition = RS_PART_EPD_CONFIG;
        else if (calendar_annotations_store_take_save_result(&result, &revision, &crc))
            partition = RS_PART_CALENDAR_ANNOTATIONS;
        if (partition != 0U) {
            p_epd->record_ack[0] = EPD_RSP_RECORD_COMMIT;
            p_epd->record_ack[1] = partition;
            p_epd->record_ack[2] = result;
            p_epd->record_ack[3] = 0U;
            resource_put_u32(p_epd->record_ack + 4, revision);
            resource_put_u32(p_epd->record_ack + 8, crc);
            p_epd->record_ack_pending = true;
        }
    }
    if (p_epd->record_ack_pending &&
        ble_epd_notification_try_send(p_epd, p_epd->record_ack,
                                      sizeof(p_epd->record_ack)) == NRF_SUCCESS)
        p_epd->record_ack_pending = false;
}

void epd_config_readback_process(ble_epd_t *p_epd)
{
    if (!p_epd->config_read_pending) return;
    if (p_epd->conn_handle == BLE_CONN_HANDLE_INVALID || p_epd->disconnect_pending ||
        !p_epd->is_notification_enabled) {
        p_epd->config_read_pending = false;
        return;
    }
    if (p_epd->cmd_queue.count || p_epd->cmd_queue.is_processing ||
        p_epd->init_pending || p_epd->init_running || epd_config_busy()) return;
    uint8_t reply[3U + sizeof(epd_config_t)] = {EPD_RSP_CONFIG, p_epd->config_read_token};
    reply[2] = epd_config_is_saved(&p_epd->config) ? RS_OK : RS_IO;
    memcpy(reply + 3, &p_epd->config, sizeof(p_epd->config));
    if (ble_epd_notification_try_send(p_epd, reply, sizeof(reply)) == NRF_SUCCESS)
        p_epd->config_read_pending = false;
}

/* Reliable control notifications are retried from main, never busy-waited
 * inside a BLE callback. Disconnect generations prevent replies leaking into
 * a later connection. */
void epd_control_notifications_process(ble_epd_t *p_epd)
{
    uint8_t reply[sizeof(epd_config_t) + 2U];
    uint8_t ack[sizeof(p_epd->mode_ack)];
    uint16_t length;
    uint8_t stage = p_epd->initial_notify;
    uint32_t generation = p_epd->connection_generation;
    bool ack_pending;
    if (!p_epd->is_notification_enabled || p_epd->disconnect_pending ||
        p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) {
        p_epd->initial_notify = 0U;
        p_epd->mode_ack_pending = false;
        return;
    }
    if (stage) {
        if (stage == 1U) {
            memcpy(reply, &p_epd->config, sizeof(epd_config_t));
            length = sizeof(epd_config_t);
        } else if (stage == 2U) {
            /* "mtu=<n> rle=1"：reply 16B 缓冲刚好容纳最大长度。 */
            length = epd_make_mtu_string((char *)reply, sizeof(reply),
                                         p_epd->max_data_len);
        } else {
            length = epd_make_prefixed_uint((char *)reply, sizeof(reply), "v=",
                                            EPD_ReadVoltageAndCache());
        }
        if (resource_notification_send(p_epd, generation, reply, length) == NRF_SUCCESS) {
            CRITICAL_REGION_ENTER();
            if (generation == p_epd->connection_generation && p_epd->initial_notify == stage)
                p_epd->initial_notify = stage < 3U ? stage + 1U : 0U;
            CRITICAL_REGION_EXIT();
        }
    }
    CRITICAL_REGION_ENTER();
    ack_pending = p_epd->mode_ack_pending;
    if (ack_pending) memcpy(ack, p_epd->mode_ack, sizeof(ack));
    CRITICAL_REGION_EXIT();
    if (ack_pending && resource_notification_send(p_epd, generation, ack,
                                                   sizeof(ack)) == NRF_SUCCESS) {
        CRITICAL_REGION_ENTER();
        if (generation == p_epd->connection_generation && p_epd->mode_ack_pending &&
            memcmp(ack, p_epd->mode_ack, sizeof(ack)) == 0)
            p_epd->mode_ack_pending = false;
        CRITICAL_REGION_EXIT();
    }
}
