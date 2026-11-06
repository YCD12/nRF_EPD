#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "epd_protocol.h"
#include "resource_protocol.h"
#define CRITICAL_REGION_ENTER() do {} while (0)
#define CRITICAL_REGION_EXIT() do {} while (0)
typedef struct model model_t;
typedef struct { void (*clear)(const model_t *, bool); void (*refresh)(const model_t *); } driver_t;
struct model { const driver_t *drv; };
typedef struct {
    const model_t *epd;
    uint32_t connection_generation;
    bool disconnect_pending, mode_ack_pending;
    uint8_t mode_ack[4];
    struct { bool transfer_active; } transfer_ctx;
} ble_epd_t;
static ble_epd_t *current;
static bool timeout, aborted, init_failed, fail_refresh, replace_connection, clear_full;
static unsigned calls;
static void panel_refresh(const model_t *m) {
    (void)m;
    assert(!current->mode_ack_pending); /* completion cannot precede BUSY return */
    ++calls;
    timeout = fail_refresh;
    if (replace_connection) ++current->connection_generation;
}
static void panel_clear(const model_t *m, bool full) { clear_full = full; panel_refresh(m); }
static const driver_t driver = {panel_clear, panel_refresh};
static const model_t panel = {&driver};
static void EPD_ClearBusyAbort(void) { aborted = false; }
static bool EPD_BusyAbortRequested(void) { return aborted; }
static void epd_service_busy_error_clear(ble_epd_t *p) { (void)p; timeout = false; }
static bool epd_service_busy_error(const ble_epd_t *p) { (void)p; return timeout; }
static bool epd_ensure_display(ble_epd_t *p) { p->epd = &panel; return !init_failed; }
static bool epd_enter_picture_mode(ble_epd_t *p) { (void)p; return true; }
/* PRODUCTION_HANDLERS */
int main(void) {
    ble_epd_t service = {.connection_generation = 1}; current = &service;
    uint8_t refresh[] = {EPD_CMD_REFRESH, 0, 42};
    cmd_display(&service, refresh, sizeof(refresh));
    assert(calls == 1 && service.mode_ack_pending && !service.transfer_ctx.transfer_active);
    assert(service.mode_ack[0] == EPD_RSP_DISPLAY && service.mode_ack[1] == 42 &&
           service.mode_ack[2] == RS_OK && service.mode_ack[3] == EPD_CMD_REFRESH);
    service.mode_ack_pending = false; fail_refresh = true;
    cmd_display(&service, refresh, sizeof(refresh));
    assert(service.mode_ack_pending && service.mode_ack[2] == RS_IO);
    service.mode_ack_pending = false; fail_refresh = false; init_failed = true;
    cmd_display(&service, refresh, sizeof(refresh));
    assert(calls == 2 && service.mode_ack_pending && service.mode_ack[2] == RS_IO);
    service.mode_ack_pending = false; init_failed = false; replace_connection = true;
    cmd_display(&service, refresh, sizeof(refresh));
    assert(calls == 3 && !service.mode_ack_pending);
    replace_connection = false;
    uint8_t clear[] = {EPD_CMD_CLEAR, 0, 43};
    cmd_display(&service, clear, 2); /* legacy refresh=false flag remains unchanged */
    assert(calls == 4 && !clear_full && !service.mode_ack_pending);
    cmd_display(&service, clear, 1);
    assert(calls == 5 && clear_full && !service.mode_ack_pending);
    cmd_display(&service, clear, 3);
    assert(calls == 6 && !clear_full && service.mode_ack_pending && service.mode_ack[1] == 43);
    service.mode_ack_pending = false;
    uint8_t invalid[] = {EPD_CMD_CLEAR, 1, 44, 9};
    cmd_display(&service, invalid, sizeof(invalid));
    assert(calls == 6 && service.mode_ack_pending && service.mode_ack[2] == RS_INVALID);
    puts("PASS: display completion after BUSY, timeout/init errors, legacy clear and connection ownership");
}
