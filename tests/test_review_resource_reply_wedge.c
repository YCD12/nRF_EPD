/* Firmware review 2026-11-04: resource-reply delivery-failure recovery.
 *
 * Production code under test:
 *   EPD/resource_transfer.c  (resource_transfer_process, reply_pending branch)
 *   EPD/EPD_service.c:457-463 (display worker early-return while a resource
 *                              reply is pending)
 *
 * Original defect: every reply-send error was ignored. A connected peer whose
 * command-characteristic notifications are disabled makes HVX return
 * NRF_ERROR_INVALID_STATE forever; reply_pending then never cleared and
 * epd_gui_update() deferred display work indefinitely (the 60 s resource
 * session timeout only covers a resource_protocol_entered() maintenance
 * session, not a plain STATUS/READ/VERIFY request). A reconnect was the only
 * recovery.
 *
 * Fixed contract (EPD/resource_transfer.c:74-87):
 *   NRF_SUCCESS                                  -> reply_pending = false
 *   NRF_ERROR_RESOURCES / NRF_ERROR_BUSY         -> retry on a later turn
 *   any other error (INVALID_STATE, INVALID_PARAM, ...) -> terminal, drop
 * A dropped reply lets the display worker run again; the peer's retried
 * request is re-executed/deduplicated and answered when it can receive.
 *
 * This regression test is intentionally NOT part of the default suite (it
 * links the transport directly and simulates a failed notification path).
 * Run explicitly after any transport change:
 *   x86_64-w64-mingw32-gcc -Wall -Werror -std=c11 \
 *     -Itests/font_transfer_stubs -IEPD \
 *     -ISDK/17.1.0_ddde560/components/libraries/util \
 *     -ISDK/17.1.0_ddde560/components/softdevice/s112/headers \
 *     EPD/resource_transfer.c tests/test_review_resource_reply_wedge.c \
 *     -o output/work-20261104/bugs/build/test_review_resource_reply_wedge.exe
 *   output/work-20261104/bugs/build/test_review_resource_reply_wedge.exe
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "epd_protocol.h"
#include "resource_protocol.h"
#include "resource_transfer.h"
#include "sdk_errors.h"

/* ---- protocol side stubs (no Flash / no store in this unit test) ---- */
static bool s_entered;
static unsigned s_execute_calls;

/* tests/font_transfer_stubs/app_util_platform.h maps the critical region to
 * these hooks; single-threaded host test, so they only count nesting. */
static int s_critical_depth;
void test_critical_enter(void) { s_critical_depth++; }
void test_critical_exit(void) { s_critical_depth--; }

void resource_protocol_reset(void) { s_entered = false; }
bool resource_protocol_entered(void) { return s_entered; }
uint8_t resource_protocol_execute(const uint8_t *q, uint8_t n, uint8_t *a) {
    (void)n;
    s_execute_calls++;
    a[0] = EPD_RSP_RESOURCE;
    a[1] = q[1];
    a[2] = q[2];
    a[3] = RS_OK;
    return 4;
}

/* ---- mock notification transport ---- */
static uint32_t s_send_result;
static unsigned s_send_calls;

static uint32_t mock_send(void *context, uint32_t generation, uint8_t *data, uint16_t length) {
    (void)context;
    (void)generation;
    (void)data;
    (void)length;
    s_send_calls++;
    return s_send_result;
}

static void pump(uint32_t now) {
    resource_transfer_process(NULL, 1U, true, true, now, mock_send);
}

static void request(uint8_t token) {
    const uint8_t status_request[] = {EPD_CMD_RESOURCE, 0x00, token};
    resource_transfer_receive(status_request, sizeof(status_request), 1U);
}

int main(void) {
    /* Baseline sanity: a successful reply send clears the pending slot. */
    s_send_result = NRF_SUCCESS;
    s_critical_depth = 0;
    pump(0U);
    assert(s_critical_depth == 0);

    /* 1) Terminal delivery failures are dropped after one turn, so
     *    resource_transfer_reply_pending() no longer blocks the display
     *    worker in EPD/EPD_service.c:457-463. */
    {
        const uint32_t terminal[] = {NRF_ERROR_INVALID_STATE, NRF_ERROR_INVALID_PARAM};
        for (unsigned i = 0; i < sizeof(terminal) / sizeof(terminal[0]); i++) {
            unsigned executes_before = s_execute_calls;
            s_send_result = terminal[i];
            request((uint8_t)(0x10U + i)); /* unique request: bypass dedup cache */
            pump(1U + i);
            assert(s_execute_calls == executes_before + 1U);
            assert(s_send_calls >= 1U);
            assert(!resource_transfer_reply_pending()); /* fixed contract */
            /* The display worker can run again; queue a later request and
             * verify the transport is still usable. */
            s_send_result = NRF_SUCCESS;
            request((uint8_t)(0x20U + i));
            pump(10U + i);
            assert(!resource_transfer_reply_pending());
        }
    }

    /* 2) Transient backpressure stays retryable: NRF_ERROR_RESOURCES and
     *    NRF_ERROR_BUSY must NOT drop the reply. */
    {
        const uint32_t transient[] = {NRF_ERROR_RESOURCES, NRF_ERROR_BUSY};
        for (unsigned i = 0; i < sizeof(transient) / sizeof(transient[0]); i++) {
            s_send_result = transient[i];
            request((uint8_t)(0x30U + i));
            pump(20U + i);
            assert(resource_transfer_reply_pending()); /* retry on later turn */
            s_send_result = NRF_SUCCESS;
            pump(21U + i);
            assert(!resource_transfer_reply_pending());
        }
    }

    /* 3) A disconnect/generation change still clears a pending reply. */
    {
        s_send_result = NRF_ERROR_RESOURCES;
        request(0x40);
        pump(30U);
        assert(resource_transfer_reply_pending());
        resource_transfer_process(NULL, 2U, false, true, 31U, mock_send);
        assert(!resource_transfer_reply_pending());
    }

    printf("PASS: resource reply delivery contract "
           "(terminal errors drop, RESOURCES/BUSY retry, disconnect clears)\n");
    return 0;
}
