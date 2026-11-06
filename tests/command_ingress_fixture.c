#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "epd_cmd_policy.h"
#include "epd_offline.h"
#include "epd_protocol.h"
#include "resource_protocol.h"

#define EPD_CMD_QUEUE_SIZE 2U
#define EPD_CMD_MAX_DATA_LEN 16U
#include "epd_command_queue.h"

#define NRF_LOG_DEBUG(...) ((void)0)
#define NRF_LOG_INFO(...) ((void)0)

typedef struct {
    epd_offline_t offline;
    epd_cmd_queue_t cmd_queue;
    uint32_t connection_generation;
    bool config_read_pending;
    uint8_t config_read_token;
    bool mode_ack_pending;
    uint8_t mode_ack[4];
} ble_epd_t;

static unsigned immediate_count;
static uint8_t immediate_cmd;
static uint16_t immediate_length;

void test_critical_enter(void) {}
void test_critical_exit(void) {}
uint32_t app_sched_event_put(void *data, uint16_t length,
                            app_sched_event_handler_t worker)
{
    (void)data; (void)length; (void)worker;
    return 0;
}
bool resource_protocol_entered(void) { return false; }
static bool resource_transfer_running(void) { return false; }
static void resource_transfer_receive(uint8_t *data, uint16_t length, uint32_t generation)
{
    (void)data; (void)length; (void)generation;
}
static bool resource_blocks_display_command(uint8_t cmd) { (void)cmd; return true; }
static bool is_bulk_data_command(uint8_t cmd)
{
    return cmd == EPD_CMD_WRITE_BLOCK || cmd == EPD_CMD_SET_ANNOTATIONS;
}
static bool is_urgent_command(uint8_t cmd) { return epd_cmd_is_urgent(cmd); }
static void ble_log(const char *format, ...) { (void)format; }
static void epd_process_next_command(void *data, uint16_t length)
{
    (void)data; (void)length;
}
static void epd_execute_command(ble_epd_t *service, uint8_t *data, uint16_t length)
{
    (void)service;
    ++immediate_count;
    immediate_cmd = data[0];
    immediate_length = length;
}

/* PRODUCTION_HANDLERS */

int main(void)
{
    ble_epd_t service = {0};
    uint8_t large[244] = {EPD_CMD_WRITE_BLOCK};
    uint8_t annotation[244] = {EPD_CMD_SET_ANNOTATIONS};
    uint8_t config[15] = {EPD_CMD_SET_CONFIG};
    uint8_t refresh[] = {EPD_CMD_REFRESH};
    uint8_t sleep[] = {EPD_CMD_SLEEP};

    epd_service_on_write(&service, refresh, sizeof(refresh));
    epd_service_on_write(&service, sleep, sizeof(sleep));
    assert(service.cmd_queue.count == 2 && immediate_count == 0);
    service.cmd_queue.count = 0;
    service.cmd_queue.head = 0;
    service.cmd_queue.tail = 0;

    epd_service_on_write(&service, large, sizeof(large));
    assert(immediate_count == 1 && immediate_cmd == EPD_CMD_WRITE_BLOCK &&
           immediate_length == sizeof(large));
    epd_service_on_write(&service, annotation, sizeof(annotation));
    assert(immediate_count == 2 && immediate_cmd == EPD_CMD_SET_ANNOTATIONS &&
           immediate_length == sizeof(annotation));
    assert(service.cmd_queue.count == 0);

    epd_service_on_write(&service, config, sizeof(config));
    assert(service.cmd_queue.count == 1 && service.cmd_queue.items[0].length == sizeof(config));
    service.cmd_queue.count = EPD_CMD_QUEUE_SIZE;
    uint8_t mode[] = {EPD_CMD_SET_MODE, 2, 17, 1, 0, 0, 0, 0, 8};
    epd_service_on_write(&service, mode, sizeof(mode));
    assert(service.mode_ack_pending && service.mode_ack[0] == EPD_RSP_MODE);
    assert(service.mode_ack[1] == 17 && service.mode_ack[2] == RS_BUSY);
    uint8_t display[] = {EPD_CMD_REFRESH, 0, 18};
    epd_service_on_write(&service, display, sizeof(display));
    assert(service.mode_ack[0] == EPD_RSP_DISPLAY && service.mode_ack[1] == 18 && service.mode_ack[2] == RS_BUSY);
    puts("PASS: compact display commands queue; CRC/annotation packets bypass small control queue");
}
