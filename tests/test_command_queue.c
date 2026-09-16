#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BLE_EPD_MAX_DATA_LEN 244U
#define EPD_CMD_MAX_DATA_LEN BLE_EPD_MAX_DATA_LEN
#define EPD_CMD_QUEUE_SIZE 2U
#include "font_transfer_state.h"
#include "epd_command_queue.h"

static unsigned depth, posts;
static bool full;
static void (*on_exit)(void);
static epd_cmd_queue_t queue;
static app_sched_event_handler_t scheduled;
static void *scheduled_context;
void test_critical_enter(void) { depth++; }
void test_critical_exit(void) {
    assert(depth);
    if (--depth == 0 && on_exit) {
        void (*fn)(void) = on_exit; on_exit = NULL; fn();
    }
}
uint32_t app_sched_event_put(void *data, uint16_t length, app_sched_event_handler_t worker) {
    assert(depth == 0 && length == sizeof(void *)); posts++;
    if (full) return 4;
    assert(scheduled == NULL);
    scheduled = worker;
    memcpy(&scheduled_context, data, length);
    return 0;
}
static void worker(void *data, uint16_t size) { (void)data; (void)size; }
static void irq_push(void) {
    uint8_t packet[] = {0x22, 9};
    assert(epd_cmd_queue_push(&queue, packet, sizeof(packet)));
    epd_cmd_queue_schedule(&queue, &queue, worker);
}
int main(void) {
    epd_cmd_queue_item_t item;
    uint8_t a[] = {0x22, 1}, b[] = {0x20, 2};
    epd_cmd_queue_init(&queue);
    assert(epd_cmd_queue_push(&queue, a, sizeof(a)));
    full = true;
    epd_cmd_queue_schedule(&queue, &queue, worker);
    assert(!queue.is_scheduled && queue.count == 1);
    full = false;
    epd_cmd_queue_schedule(&queue, &queue, worker);
    assert(queue.is_scheduled && scheduled_context == &queue);
    unsigned before = posts;
    assert(epd_cmd_queue_push(&queue, b, sizeof(b)));
    epd_cmd_queue_schedule(&queue, &queue, worker);
    assert(posts == before); /* Only one outstanding worker. */
    scheduled = NULL; queue.is_scheduled = false; queue.is_processing = true;
    on_exit = irq_push; /* Producer immediately follows atomic pop. */
    assert(epd_cmd_queue_pop(&queue, &item));
    assert(item.data[1] == 1 && queue.count == 2 && posts == before);
    assert(epd_cmd_queue_pop(&queue, &item) && item.data[1] == 2);
    assert(epd_cmd_queue_pop(&queue, &item) && item.data[1] == 9);
    assert(!epd_cmd_queue_pop(&queue, &item));
    queue.is_processing = false;
    assert(!epd_cmd_queue_push(&queue, a, 245));
    puts("PASS: command queue IRQ ownership, duplicate posts and full-scheduler retry");
}
