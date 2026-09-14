#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "font_transfer.h"
#include "app_scheduler.h"

static ble_epd_t epd;
static unsigned lock_depth, events_head, events_tail, sends, writes;
static uint32_t now = 100U, received;
static bool active, scheduler_full, tx_full, inject_tx_complete, inject_ack, inject_packet;
static uint8_t written[32];
static void (*after_unlock)(void);
static struct { app_sched_event_handler_t handler; ble_epd_t* epd; } events[128];
static uint8_t responses[128][11];
static unsigned response_lengths[128];

void test_critical_enter(void) { lock_depth++; }
void test_critical_exit(void)
{
    assert(lock_depth);
    lock_depth--;
    if (!lock_depth && after_unlock) {
        void (*fn)(void) = after_unlock;
        after_unlock = NULL;
        fn();
    }
}
uint32_t app_uptime_seconds(void) { return now; }
void app_feed_wdt(void) {}
void epd_refresh_fonts(ble_epd_t* p) { (void)p; }

uint32_t app_sched_event_put(void* data, uint16_t length, app_sched_event_handler_t handler)
{
    assert(lock_depth == 0U && length == sizeof(ble_epd_t*));
    if (scheduler_full) return NRF_ERROR_NO_MEM;
    assert(events_tail < 128U);
    events[events_tail].handler = handler;
    memcpy(&events[events_tail++].epd, data, length);
    return NRF_SUCCESS;
}
static void run_events(void)
{
    while (events_head < events_tail) {
        unsigned i = events_head++;
        events[i].handler(&events[i].epd, sizeof(ble_epd_t*));
    }
    events_head = events_tail = 0U;
}
static void reset(void)
{
    memset(&epd, 0, sizeof(epd));
    epd.conn_handle = 1U;
    active = true;
    lock_depth = events_head = events_tail = sends = writes = received = 0U;
    scheduler_full = tx_full = inject_tx_complete = inject_ack = inject_packet = false;
    after_unlock = NULL;
}
uint16_t crc16_compute(const uint8_t* data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    for (unsigned i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = crc & 1U ? (crc >> 1) ^ 0x8408U : crc >> 1;
    }
    return crc;
}
static void send_packet(uint16_t sequence, uint8_t value)
{
    uint8_t packet[11] = {EPD_CMD_FONT_STORE, FONT_STORE_OP_STREAM_DATA,
                         sequence, sequence >> 8, sequence, 0, 0, 0, value, 0, 0};
    uint16_t crc = crc16_compute(packet + 1, 8U);
    packet[9] = crc; packet[10] = crc >> 8;
    cmd_font_store(&epd, packet, sizeof(packet));
}
bool font_store_update_active(void) { return active; }
font_store_result_t font_store_begin_pack(uint32_t total, uint32_t crc, const font_store_pack_slot_t* slots)
{
    (void)total; (void)crc; (void)slots;
    assert(lock_depth == 0U);
    active = true;
    return FONT_STORE_OK;
}
font_store_result_t font_store_write_pack_chunk(uint32_t offset, const uint8_t* bytes, size_t length)
{
    assert(lock_depth == 0U && offset == received);
    assert(received + length <= sizeof(written));
    /* Simulate an IRQ arriving while the scheduler waits for Flash. */
    if (inject_packet) { inject_packet = false; send_packet(1U, 0x22U); }
    if (epd.font.cmd_pending) {
        /* A second control command must see the slot as occupied throughout
         * the current command's Flash and input CRC processing. */
        assert(epd.font.cmd.data[1] == FONT_STORE_OP_PACK_DATA);
    }
    memcpy(written + received, bytes, length);
    received += length; writes++;
    return FONT_STORE_OK;
}
font_store_result_t font_store_commit_pack(void) { active = false; return FONT_STORE_OK; }
font_store_result_t font_store_abort_update(void) { active = false; return FONT_STORE_OK; }
bool font_store_get_info(font_store_slot_t slot, font_store_info_t* info)
{
    (void)slot; memset(info, 0, sizeof(*info)); return true;
}
uint32_t ble_epd_notification_try_send(ble_epd_t* p, uint8_t* data, uint16_t length)
{
    assert(lock_depth == 0U && p == &epd && length <= 11U);
    if (inject_tx_complete) {
        inject_tx_complete = false;
        font_task_wake_tx(p, FONT_TASK_OPERATION_ACK);
        return NRF_ERROR_RESOURCES;
    }
    if (inject_ack) {
        inject_ack = false;
        font_store_send_ack(p, FONT_STORE_OP_PACK_COMMIT, FONT_STORE_CRC, 0xFFU);
    }
    if (tx_full) return NRF_ERROR_RESOURCES;
    assert(sends < 128U);
    memcpy(responses[sends], data, length);
    response_lengths[sends++] = length;
    return NRF_SUCCESS;
}
static void enqueue_from_irq(void)
{
    font_store_send_ack(&epd, FONT_STORE_OP_PACK_ABORT, FONT_STORE_OK, 0xFFU);
}

int main(void)
{
    reset();
    epd.font.stream.total_length = 2U;
    inject_packet = true;
    send_packet(0U, 0x11U);
    run_events();
    assert(writes == 2U && received == 2U && epd.font.stream.count == 0U);
    assert(written[0] == 0x11U && written[1] == 0x22U);
    assert(sends && responses[sends - 1U][0] == EPD_RSP_FONT_STREAM_ACK);

    reset();
    font_store_send_ack(&epd, FONT_STORE_OP_PACK_BEGIN, FONT_STORE_OK, 0xFFU);
    after_unlock = enqueue_from_irq; /* IRQ at the task snapshot boundary. */
    inject_ack = true;              /* IRQ while a notification is sent. */
    run_events();
    assert(sends == 3U);
    assert(responses[0][1] == FONT_STORE_OP_PACK_BEGIN);
    assert(responses[1][1] == FONT_STORE_OP_PACK_ABORT);
    assert(responses[2][1] == FONT_STORE_OP_PACK_COMMIT && responses[2][2] == FONT_STORE_CRC);

    reset();
    inject_tx_complete = true;
    font_store_send_ack(&epd, FONT_STORE_OP_PACK_BEGIN, FONT_STORE_OK, 0xFFU);
    run_events();
    assert(sends == 1U); /* TX_COMPLETE inside failed HVX must not be lost. */

    reset();
    scheduler_full = true;
    font_store_send_ack(&epd, FONT_STORE_OP_PACK_ABORT, FONT_STORE_OK, 0xFFU);
    assert(epd.font.task_retry_pending && !epd.font.task_scheduled);
    scheduler_full = false;
    font_task_retry(&epd); run_events(); assert(sends == 1U);

    reset();
    tx_full = true;
    font_store_send_ack(&epd, FONT_STORE_OP_PACK_ABORT, FONT_STORE_OK, 0xFFU);
    run_events(); assert(sends == 0U && epd.font.ack_inflight_valid);
    tx_full = false;
    font_task_wake_tx(&epd, FONT_TASK_OPERATION_ACK); run_events(); assert(sends == 1U);

    reset();
    epd.font.cmd_pending = true;
    epd.font.cmd.length = 7U;
    epd.font.cmd.data[0] = EPD_CMD_FONT_STORE;
    epd.font.cmd.data[1] = FONT_STORE_OP_PACK_DATA;
    epd.font.cmd.data[6] = 0x55U;
    font_task_schedule(&epd, FONT_TASK_CONTROL); run_events();
    assert(writes == 1U && !epd.font.cmd_pending && written[0] == 0x55U);

    reset();
    font_store_send_list(&epd); run_events();
    assert(sends == 3U);
    font_store_send_list(&epd); run_events();
    assert(sends == 6U && responses[3][1] == 0U);
    puts("PASS: font task IRQ interleaving, FIFO ownership, TX wakeup and control lifetime");
    return 0;
}
