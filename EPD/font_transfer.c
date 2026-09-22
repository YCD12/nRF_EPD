#include "font_transfer.h"
#include "main.h"
#include "app_scheduler.h"
#include "app_util_platform.h"
#include "nrf_log.h"
#include <string.h>
#include <stddef.h>

#if FONT_STORE_MUTABLE

void cmd_font_store(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length);
static void font_stream_worker(void* p_event_data, uint16_t event_size);
static void font_stream_ack_worker(void* p_event_data, uint16_t event_size);
static void font_store_list_worker(void* p_event_data, uint16_t event_size);
static void font_store_ack_worker(void* p_event_data, uint16_t event_size);
static void font_control_worker(void* p_event_data, uint16_t event_size);
static bool font_timeout_abort_worker(void* p_event_data, uint16_t event_size);
static void font_task_worker(void* p_event_data, uint16_t event_size);
void font_stream_reset(ble_epd_t* p_epd);
bool font_task_schedule(ble_epd_t* p_epd, uint8_t task);
void font_task_wake_tx(ble_epd_t* p_epd, uint8_t task);
static void font_task_wait_tx(ble_epd_t* p_epd, uint8_t task);
static void font_task_retry_timer(ble_epd_t* p_epd, uint8_t task);
void font_task_retry(ble_epd_t* p_epd);
static bool font_task_abort_if_requested(ble_epd_t* p_epd);
uint32_t ble_epd_notification_try_send(ble_epd_t* p_epd,
                                               uint8_t* p_string,
                                               uint16_t length);

static uint32_t font_store_get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void font_task_wait_tx(ble_epd_t* p_epd, uint8_t task)
{
    CRITICAL_REGION_ENTER();
    p_epd->font.task_pending |= task;
    p_epd->font.task_wait_tx |= task;
    CRITICAL_REGION_EXIT();
}

static void font_task_retry_timer(ble_epd_t* p_epd, uint8_t task)
{
    CRITICAL_REGION_ENTER();
    p_epd->font.task_pending |= task;
    p_epd->font.task_wait_tx &= (uint8_t)~task;
    p_epd->font.task_retry_pending = true;
    CRITICAL_REGION_EXIT();
}

/* Only scheduler workers send notifications. Arm before HVX so an IRQ-side
 * TX_COMPLETE cannot be lost; other errors use the existing timer retry.
 * On failure the caller restores its owned reply before yielding to main. */
static uint32_t font_notification_send(ble_epd_t *p_epd, uint8_t task,
                                        uint8_t *response, uint16_t length)
{
    font_task_wait_tx(p_epd, task);
    uint32_t err = ble_epd_notification_try_send(p_epd, response, length);
    if (err != NRF_SUCCESS && err != NRF_ERROR_RESOURCES) {
        font_task_retry_timer(p_epd, task);
        NRF_LOG_WARNING("[FONT]: notification failed: 0x%08x\n", err);
    }
    return err;
}

static bool font_task_post(ble_epd_t* p_epd)
{
    bool post = false;
    if (p_epd == NULL) return false;
    CRITICAL_REGION_ENTER();
    if ((p_epd->font.task_pending & (uint8_t)~p_epd->font.task_wait_tx) != 0U &&
        !p_epd->font.task_scheduled) {
        p_epd->font.task_scheduled = true;
        post = true;
    }
    CRITICAL_REGION_EXIT();
    if (!post) return true;
    uint32_t err = app_sched_event_put(&p_epd, sizeof(p_epd), font_task_worker);
    CRITICAL_REGION_ENTER();
    if (err != NRF_SUCCESS) p_epd->font.task_scheduled = false;
    p_epd->font.task_retry_pending = err != NRF_SUCCESS;
    CRITICAL_REGION_EXIT();
    return err == NRF_SUCCESS;
}

bool font_task_schedule(ble_epd_t* p_epd, uint8_t task)
{
    if (p_epd == NULL || task == 0U) return false;
    CRITICAL_REGION_ENTER();
    p_epd->font.task_pending |= task;
    CRITICAL_REGION_EXIT();
    return font_task_post(p_epd);
}

void font_task_wake_tx(ble_epd_t* p_epd, uint8_t task)
{
    if (p_epd == NULL) return;
    CRITICAL_REGION_ENTER();
    p_epd->font.task_wait_tx &= (uint8_t)~task;
    CRITICAL_REGION_EXIT();
    (void)font_task_post(p_epd);
}

void font_task_retry(ble_epd_t* p_epd)
{
    if (p_epd != NULL && p_epd->font.task_retry_pending) (void)font_task_post(p_epd);
}

static void font_task_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    uint8_t tasks;

    (void)event_size;
    if (p_epd == NULL) {
        return;
    }

    CRITICAL_REGION_ENTER();
    p_epd->font.task_scheduled = false;
    tasks = p_epd->font.task_pending &
            (uint8_t)~p_epd->font.task_wait_tx;
    p_epd->font.task_pending &= (uint8_t)~tasks;
    CRITICAL_REGION_EXIT();

    /* Abort first so a stale queued control/data task cannot extend an
     * expired or disconnected transaction.  If the timeout was superseded
     * by a fresh packet, the abort worker returns false and normal work may
     * continue from this same scheduler turn. */
    if ((tasks & FONT_TASK_TIMEOUT_ABORT) != 0U) {
        if (font_timeout_abort_worker(&p_epd, 0U)) {
            return;
        }
    }
    if (font_task_abort_if_requested(p_epd)) return;
    /* Control next: COMMIT can synchronously drain the stream FIFO. */
    if ((tasks & FONT_TASK_CONTROL) != 0U) {
        font_control_worker(&p_epd, 0U);
    }
    if (font_task_abort_if_requested(p_epd)) return;
    if ((tasks & FONT_TASK_STREAM) != 0U) {
        font_stream_worker(&p_epd, 0U);
    }
    if (font_task_abort_if_requested(p_epd)) return;
    if ((tasks & FONT_TASK_LIST) != 0U) {
        font_store_list_worker(&p_epd, 0U);
    }
    if (font_task_abort_if_requested(p_epd)) return;
    if ((tasks & FONT_TASK_OPERATION_ACK) != 0U) {
        font_store_ack_worker(&p_epd, 0U);
    }
    if (font_task_abort_if_requested(p_epd)) return;
    if ((tasks & FONT_TASK_STREAM_ACK) != 0U) {
        font_stream_ack_worker(&p_epd, 0U);
    }
    if (font_task_abort_if_requested(p_epd)) return;

    /* A worker may have queued another task while it was running. */
    (void)font_task_post(p_epd);
}

void font_store_send_ack(ble_epd_t* p_epd, uint8_t operation, font_store_result_t result, uint8_t slot)
{
    if (p_epd == NULL) return;
    CRITICAL_REGION_ENTER();
    font_store_ack_item_t item = {operation, (uint8_t)result, slot};
    if (p_epd->font.ack_count < FONT_STORE_ACK_QUEUE_SIZE) {
        p_epd->font.ack_queue[p_epd->font.ack_tail] = item;
        p_epd->font.ack_tail = (p_epd->font.ack_tail + 1U) % FONT_STORE_ACK_QUEUE_SIZE;
        p_epd->font.ack_count++;
    } else if (!p_epd->font.ack_overflow_pending ||
               (p_epd->font.ack_overflow.status == FONT_STORE_BUSY && result != FONT_STORE_BUSY)) {
        p_epd->font.ack_overflow = item;
        p_epd->font.ack_overflow_pending = true;
    }
    CRITICAL_REGION_EXIT();
    (void)font_task_schedule(p_epd, FONT_TASK_OPERATION_ACK);
}

static void font_store_ack_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    (void)event_size;
    if (p_epd == NULL) return;
    while (!font_task_abort_if_requested(p_epd)) {
        /* Take ownership before notifying; an IRQ can enqueue while HVX
         * executes, but cannot replace the in-flight reply. */
        CRITICAL_REGION_ENTER();
        if (!p_epd->font.ack_inflight_valid) {
            if (p_epd->font.ack_count != 0U) {
                p_epd->font.ack_inflight = p_epd->font.ack_queue[p_epd->font.ack_head];
                p_epd->font.ack_head = (p_epd->font.ack_head + 1U) % FONT_STORE_ACK_QUEUE_SIZE;
                p_epd->font.ack_count--;
                p_epd->font.ack_inflight_valid = true;
            } else if (p_epd->font.ack_overflow_pending) {
                p_epd->font.ack_inflight = p_epd->font.ack_overflow;
                p_epd->font.ack_overflow_pending = false;
                p_epd->font.ack_inflight_valid = true;
            }
        }
        CRITICAL_REGION_EXIT();
        if (!p_epd->font.ack_inflight_valid) return;
        font_store_ack_item_t* item = &p_epd->font.ack_inflight;
        uint8_t response[4] = {EPD_RSP_FONT_ACK, item->operation, item->status, item->slot};
        if (font_notification_send(p_epd, FONT_TASK_OPERATION_ACK,
                                   response, sizeof(response)) != NRF_SUCCESS) return;
        p_epd->font.ack_inflight_valid = false;
        font_task_wake_tx(p_epd, FONT_TASK_OPERATION_ACK);
        app_feed_wdt();
    }
}

static void font_control_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    (void)event_size;
    if (font_task_abort_if_requested(p_epd) || !p_epd->font.cmd_pending) return;
    /* Keep the slot owned until all SPI/CRC reads from it are finished. */
    cmd_font_store(p_epd, p_epd->font.cmd.data, p_epd->font.cmd.length);
    p_epd->font.cmd_pending = false;
}

static bool font_timeout_abort_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    bool was_active;
    bool disconnect_requested;
    bool idle_expired;

    (void)event_size;
    if (p_epd == NULL) {
        return false;
    }
    CRITICAL_REGION_ENTER();
    disconnect_requested = p_epd->font.abort_requested ||
                           p_epd->conn_handle == BLE_CONN_HANDLE_INVALID;
    idle_expired = (p_epd->font.operation_pending || font_store_update_active()) &&
                   p_epd->font.last_activity_ts != 0U &&
                   (uint32_t)(app_uptime_seconds() - p_epd->font.last_activity_ts) >=
                       FONT_STORE_IDLE_TIMEOUT_S;
    if (disconnect_requested || idle_expired) p_epd->font.abort_requested = true;
    CRITICAL_REGION_EXIT();
    if (!disconnect_requested && !idle_expired) {
        /* A packet may have refreshed the activity timestamp after the timer
         * posted this task but before the scheduler executed it. */
        return false;
    }
    was_active = font_store_update_active();
    if (was_active) {
        (void)font_store_abort_update();
    }
    CRITICAL_REGION_ENTER();
    font_stream_reset(p_epd);
    p_epd->font.cmd_pending = false;
    p_epd->font.list_pending = false;
    p_epd->font.operation_pending = false;
    p_epd->font.last_activity_ts = 0U;
    p_epd->font.list_slot = 0U;
    p_epd->font.ack_head = 0U;
    p_epd->font.ack_tail = 0U;
    p_epd->font.ack_count = 0U;
    p_epd->font.ack_overflow_pending = false;
    memset(&p_epd->font.cmd, 0, sizeof(p_epd->font.cmd));
    p_epd->font.task_pending = 0U;
    p_epd->font.task_wait_tx = 0U;
    p_epd->font.task_retry_pending = false;
    p_epd->font.abort_requested = false;
    p_epd->font.ack_inflight_valid = false;
    p_epd->font.list_restart = false;
    CRITICAL_REGION_EXIT();
    if (was_active) {
        NRF_LOG_WARNING("[FONT]: %s aborted package\n",
                        disconnect_requested ? "disconnect" : "idle timeout");
    }
    return true;
}

static bool font_task_abort_if_requested(ble_epd_t* p_epd)
{
    if (p_epd == NULL) {
        return true;
    }
    if (!p_epd->font.abort_requested &&
        p_epd->conn_handle != BLE_CONN_HANDLE_INVALID) {
        return false;
    }
    (void)font_timeout_abort_worker(&p_epd, 0U);
    return true;
}

void font_store_send_list(ble_epd_t* p_epd)
{
    if (p_epd == NULL) {
        return;
    }
    CRITICAL_REGION_ENTER();
    p_epd->font.list_restart = true;
    p_epd->font.list_pending = true;
    CRITICAL_REGION_EXIT();
    (void)font_task_schedule(p_epd, FONT_TASK_LIST);
}

static void font_store_list_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);

    (void)event_size;
    if (p_epd == NULL) {
        return;
    }
    if (font_task_abort_if_requested(p_epd)) return;
    while (p_epd->font.list_pending &&
           (p_epd->font.list_restart || p_epd->font.list_slot < FONT_STORE_SLOT_COUNT)) {
        CRITICAL_REGION_ENTER();
        if (p_epd->font.list_restart) { p_epd->font.list_slot = 0U; p_epd->font.list_restart = false; }
        CRITICAL_REGION_EXIT();
        uint8_t slot = p_epd->font.list_slot;
        font_store_info_t info;
        uint8_t response[11] = {
            EPD_RSP_FONT_LIST, slot, 0U, 0U, 0U, 0U,
            0U, 0U, 0U, 0U, 0U,
        };
        uint32_t err_code;

        if (font_task_abort_if_requested(p_epd)) return;

        /* Always emit one response per slot.  If the store is unavailable,
         * the zeroed record still lets the browser finish the list request
         * and display placeholders instead of timing out. */
        if (font_store_get_info((font_store_slot_t)slot, &info)) {
            response[2] = info.present;
            response[3] = (uint8_t)info.length;
            response[4] = (uint8_t)(info.length >> 8);
            response[5] = (uint8_t)(info.length >> 16);
            response[6] = (uint8_t)(info.length >> 24);
            response[7] = (uint8_t)info.crc32;
            response[8] = (uint8_t)(info.crc32 >> 8);
            response[9] = (uint8_t)(info.crc32 >> 16);
            response[10] = (uint8_t)(info.crc32 >> 24);
        }

        /* This worker runs from the scheduler, never from the BLE write
         * observer.  A full notification queue is retried after
         * BLE_GATTS_EVT_HVN_TX_COMPLETE instead of busy-waiting here. */
        err_code = font_notification_send(p_epd, FONT_TASK_LIST, response, sizeof(response));
        if (err_code != NRF_SUCCESS) {
            return;
        }

        CRITICAL_REGION_ENTER();
        if (p_epd->font.list_restart) p_epd->font.list_slot = 0U;
        else p_epd->font.list_slot++;
        p_epd->font.list_pending = p_epd->font.list_slot < FONT_STORE_SLOT_COUNT;
        CRITICAL_REGION_EXIT();
        font_task_wake_tx(p_epd, FONT_TASK_LIST);
        app_feed_wdt();
    }

}

void font_stream_reset(ble_epd_t* p_epd)
{
    if (p_epd == NULL) return;
    CRITICAL_REGION_ENTER();
    /* Packet storage is unreachable once count is zero; do not clear 720
     * bytes with interrupts masked. */
    memset(&p_epd->font.stream, 0, offsetof(font_stream_rx_t, packets));
    CRITICAL_REGION_EXIT();
}

static void font_stream_send_ack(ble_epd_t* p_epd, uint8_t status)
{
    font_stream_rx_t* stream;

    if (p_epd == NULL) {
        return;
    }
    stream = &p_epd->font.stream;
    CRITICAL_REGION_ENTER();
    stream->ack_pending = true;
    /* Preserve an error until it has been delivered.  A later successful
     * drain must not overwrite a CRC/FIFO/Flash error waiting for TX space. */
    if (status != 0U || stream->ack_status == 0U) {
        stream->ack_status = status;
    }
    CRITICAL_REGION_EXIT();
    (void)font_task_schedule(p_epd, FONT_TASK_STREAM_ACK);
}

static void font_stream_send_error_ack(ble_epd_t* p_epd, uint8_t status)
{
    if (p_epd == NULL) {
        return;
    }
    /* After a protocol error the browser may deliberately reduce its window
     * to one packet.  Keep acknowledging each drained packet until a full
     * normal batch has completed; otherwise the browser waits for an ACK that
     * the MCU is still batching. */
    p_epd->font.stream.force_ack_each_packet = true;
    font_stream_send_ack(p_epd, status);
}

static void font_stream_ack_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);
    font_stream_rx_t* stream;
    uint8_t response[9];
    uint32_t err_code;

    (void)event_size;
    if (p_epd == NULL) {
        return;
    }
    if (font_task_abort_if_requested(p_epd)) return;
    stream = &p_epd->font.stream;
    if (!stream->ack_pending) {
        return;
    }

    CRITICAL_REGION_ENTER();
    response[0] = EPD_RSP_FONT_STREAM_ACK;
    response[1] = (uint8_t)stream->next_sequence;
    response[2] = (uint8_t)(stream->next_sequence >> 8);
    response[3] = (uint8_t)stream->next_offset;
    response[4] = (uint8_t)(stream->next_offset >> 8);
    response[5] = (uint8_t)(stream->next_offset >> 16);
    response[6] = (uint8_t)(stream->next_offset >> 24);
    response[7] = (uint8_t)(FONT_STREAM_FIFO_SIZE - stream->count);
    response[8] = stream->ack_status;
    stream->ack_pending = false;
    stream->ack_status = 0U;
    CRITICAL_REGION_EXIT();
    err_code = font_notification_send(p_epd, FONT_TASK_STREAM_ACK, response, sizeof(response));
    if (err_code != NRF_SUCCESS) {
        CRITICAL_REGION_ENTER();
        stream->ack_pending = true;
        if (stream->ack_status == 0U) stream->ack_status = response[8];
        CRITICAL_REGION_EXIT();
    }
    if (err_code != NRF_SUCCESS) {
        return;
    }

    font_task_wake_tx(p_epd, FONT_TASK_STREAM_ACK);
}

static void font_stream_drain(ble_epd_t* p_epd)
{
    font_stream_rx_t* stream = &p_epd->font.stream;

    while (stream->active && stream->error == 0U && stream->count > 0U &&
           !p_epd->font.abort_requested &&
           p_epd->conn_handle != BLE_CONN_HANDLE_INVALID) {
        font_stream_packet_t* packet = &stream->packets[stream->head];
        font_store_result_t result = font_store_write_pack_chunk(
            packet->offset, packet->data, packet->length);

        CRITICAL_REGION_ENTER();
        stream->head = (uint8_t)((stream->head + 1U) % FONT_STREAM_FIFO_SIZE);
        stream->count--;
        if (result != FONT_STORE_OK) {
            stream->error = (uint8_t)result;
            stream->count = 0U;
        } else {
            stream->pending_ack_packets++;
        }
        CRITICAL_REGION_EXIT();
        app_feed_wdt();
    }

    if (p_epd->font.abort_requested ||
        p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) {
        return;
    }

    bool send_ack = false;
    uint8_t ack_status;
    CRITICAL_REGION_ENTER();
    if (stream->error != 0U ||
        stream->pending_ack_packets >= FONT_STREAM_ACK_EVERY ||
        (stream->next_offset == stream->total_length && stream->count == 0U) ||
        (stream->force_ack_each_packet &&
         stream->pending_ack_packets > 0U && stream->count == 0U)) {
        if (stream->force_ack_each_packet &&
            (stream->pending_ack_packets >= FONT_STREAM_ACK_EVERY ||
             stream->next_offset == stream->total_length)) {
            stream->force_ack_each_packet = false;
        }
        send_ack = true;
        stream->pending_ack_packets = 0U;
    }
    ack_status = stream->error;
    CRITICAL_REGION_EXIT();
    if (send_ack) font_stream_send_ack(p_epd, ack_status);

    if (stream->active && stream->error == 0U && stream->count > 0U) {
        (void)font_task_schedule(p_epd, FONT_TASK_STREAM);
    }
}

static void font_stream_worker(void* p_event_data, uint16_t event_size)
{
    ble_epd_t* p_epd = *((ble_epd_t**)p_event_data);

    (void)event_size;
    if (p_epd != NULL) {
        font_stream_drain(p_epd);
    }
}

static void cmd_font_stream_data(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length)
{
    font_stream_rx_t* stream = &p_epd->font.stream;
    uint16_t sequence;
    uint32_t offset;
    uint16_t received_crc;
    uint16_t calculated_crc;
    size_t data_length;
    font_stream_packet_t* packet;

    if (length < (uint16_t)(FONT_STREAM_PACKET_OVERHEAD + 1U)) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_INVALID);
        if (!stream->active) {
            p_epd->font.operation_pending = false;
        }
        return;
    }
    if (p_epd->font.cmd_pending) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_BUSY);
        return;
    }
    if (!font_store_update_active()) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_INVALID);
        p_epd->font.operation_pending = false;
        return;
    }

    received_crc = (uint16_t)p_data[length - 2U] |
                   ((uint16_t)p_data[length - 1U] << 8);
    calculated_crc = crc16_compute(&p_data[1], (uint16_t)(length - 3U));
    if (received_crc != calculated_crc) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_CRC);
        return;
    }

    /* PACK_BEGIN is shared with the legacy per-packet path.  Switch to the
     * FIFO only when the first high-speed data packet actually arrives. */
    stream->active = true;

    sequence = (uint16_t)p_data[2] | ((uint16_t)p_data[3] << 8);
    offset = font_store_get_u32(&p_data[4]);
    data_length = (size_t)length - FONT_STREAM_PACKET_OVERHEAD;

    if (stream->error != 0U) { font_stream_send_ack(p_epd, stream->error); return; }

    /* A retransmission after a lost cumulative ACK is harmless. */
    if (sequence < stream->next_sequence) {
        font_stream_send_ack(p_epd, stream->error);
        return;
    }
    if (sequence != stream->next_sequence || offset != stream->next_offset) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_INVALID);
        return;
    }
    if (data_length == 0U || data_length > FONT_STREAM_DATA_MAX_LEN ||
        offset > stream->total_length ||
        data_length > (size_t)(stream->total_length - offset)) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_INVALID);
        return;
    }
    if (stream->count >= FONT_STREAM_FIFO_SIZE) {
        font_stream_send_error_ack(p_epd, (uint8_t)FONT_STORE_BUSY);
        return;
    }

    packet = &stream->packets[stream->tail];
    packet->offset = offset;
    packet->length = (uint8_t)data_length;
    memcpy(packet->data, &p_data[8], data_length);
    stream->tail = (uint8_t)((stream->tail + 1U) % FONT_STREAM_FIFO_SIZE);
    stream->count++;
    stream->next_sequence++;
    stream->next_offset += (uint32_t)data_length;

    (void)font_task_schedule(p_epd, FONT_TASK_STREAM);
}

void cmd_font_store(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length)
{
    uint8_t operation;
    font_store_result_t result;
    uint32_t now;

    if (length < 2U) {
        return;
    }
    if (p_epd->font.abort_requested) return;
    operation = p_data[1];
    now = app_uptime_seconds();
    if (operation == FONT_STORE_OP_GET) {
        font_store_send_list(p_epd);
        return;
    }
    p_epd->font.last_activity_ts = now;
    if (operation == FONT_STORE_OP_STREAM_DATA) {
        cmd_font_stream_data(p_epd, p_data, length);
        return;
    }
    /* Keep the guard asserted until COMMIT/DELETE/ABORT finishes.  This is
     * intentionally separate from font_store_update_active(): it also covers
     * the short interval while the command is queued or while a manifest is
     * being rewritten. */
    p_epd->font.operation_pending = true;
    if (operation == FONT_STORE_OP_PACK_BEGIN) {
        font_store_pack_slot_t slots[FONT_STORE_SLOT_COUNT];
        uint8_t slot;

        if (p_epd->font.last_begin_ts != 0U &&
            (uint32_t)(now - p_epd->font.last_begin_ts) <
                FONT_STORE_BEGIN_RATE_LIMIT_S &&
            !font_store_update_active()) {
            font_store_send_ack(p_epd, operation, FONT_STORE_BUSY, 0xFFU);
            p_epd->font.operation_pending = false;
            return;
        }

        /* [op, total:u32, pack_crc:u32, (length:u32, crc:u32) * 3] */
        if (length < 34U) {
            font_store_send_ack(p_epd, operation, FONT_STORE_INVALID, 0xFFU);
            p_epd->font.operation_pending = false;
            return;
        }
        for (slot = 0U; slot < FONT_STORE_SLOT_COUNT; slot++) {
            uint16_t base = (uint16_t)(10U + slot * 8U);
            slots[slot].length = font_store_get_u32(&p_data[base]);
            slots[slot].crc32 = font_store_get_u32(&p_data[base + 4U]);
        }
        result = font_store_begin_pack(font_store_get_u32(&p_data[2]),
                                       font_store_get_u32(&p_data[6]), slots);
        font_store_send_ack(p_epd, operation, result, 0xFFU);
        if (result == FONT_STORE_OK) {
            p_epd->font.last_begin_ts = now;
            font_stream_reset(p_epd);
            p_epd->font.stream.active = false;
            p_epd->font.stream.total_length = font_store_get_u32(&p_data[2]);
        } else {
            p_epd->font.operation_pending = false;
        }
    } else if (operation == FONT_STORE_OP_PACK_DATA) {
        if (length < 7U) {
            font_store_send_ack(p_epd, operation, FONT_STORE_INVALID, 0xFFU);
            p_epd->font.operation_pending = false;
            return;
        }
        result = font_store_write_pack_chunk(font_store_get_u32(&p_data[2]),
                                             &p_data[6], (size_t)(length - 6U));
        /* GATT write completion only means that SoftDevice accepted the
         * packet.  Return the actual storage result so the web client can
         * apply back-pressure and detect a dropped/out-of-order chunk. */
        font_store_send_ack(p_epd, operation, result, 0xFFU);
        if (result != FONT_STORE_OK && !font_store_update_active()) {
            p_epd->font.operation_pending = false;
        }
    } else if (operation == FONT_STORE_OP_PACK_COMMIT) {
        if (length < 2U) {
            p_epd->font.operation_pending = false;
            return;
        }
        if (p_epd->font.stream.active) {
            font_stream_drain(p_epd);
        }
        if (p_epd->font.abort_requested ||
            p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) {
            return;
        }
        if (p_epd->font.stream.error != 0U) {
            result = (font_store_result_t)p_epd->font.stream.error;
        } else {
            result = font_store_commit_pack();
        }
        font_store_send_ack(p_epd, operation, result, 0xFFU);
        if (result == FONT_STORE_OK) {
            font_stream_reset(p_epd);
            epd_refresh_fonts(p_epd);
        }
        if (!font_store_update_active()) {
            p_epd->font.operation_pending = false;
        }
    } else if (operation == FONT_STORE_OP_PACK_ABORT) {
        result = font_store_abort_update();
        font_stream_reset(p_epd);
        font_store_send_ack(p_epd, operation, result, 0xFFU);
        p_epd->font.operation_pending = false;
    } else {
        font_store_send_ack(p_epd, operation, FONT_STORE_INVALID, 0xFFU);
        p_epd->font.operation_pending = false;
    }
    app_feed_wdt();
}

#endif /* FONT_STORE_MUTABLE */
