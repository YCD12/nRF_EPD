#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "sdk_config.h"
#include "nrf_sortlist.h"

#if APP_TIMER_KEEPS_RTC_ACTIVE || APP_TIMER_CONFIG_USE_SCHEDULER
#error This regression must exercise the default idle-stop and IRQ-callback configuration
#endif

#define ASSERT assert
#define NRF_LOG_DEBUG(...)
#define NRF_LOG_INFO(...)
#define NRF_LOG_WARNING(...)
#define NRF_LOG_ERROR(...)
#define NRF_LOG_INST_DEBUG(...)
#define CRITICAL_REGION_ENTER()
#define CRITICAL_REGION_EXIT()
#define UNUSED_RETURN_VALUE(value) ((void)(value))
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#undef NRF_SORTLIST_INST_NAME
#define NRF_SORTLIST_INST_NAME(name) NULL
#define CONCAT_2(a, b) a##b
#define DRV_RTC_MAX_CNT 0xFFFFFFu
#define RTC_COUNTER_COUNTER_Msk DRV_RTC_MAX_CNT
#define APP_TIMER_IDLE_VAL UINT64_MAX
#define APP_TIMER_IS_IDLE(timer) ((timer)->end_val == APP_TIMER_IDLE_VAL)
#define APP_TIMER_TICKS(ms) ((uint32_t)(((ms) * UINT64_C(32768) + \
    500 * (APP_TIMER_CONFIG_RTC_FREQUENCY + 1)) / \
    (1000 * (APP_TIMER_CONFIG_RTC_FREQUENCY + 1))))
#define APP_TIMER_SAFE_WINDOW APP_TIMER_TICKS(APP_TIMER_SAFE_WINDOW_MS)
#define APP_TIMER_RTC_MAX_VALUE (DRV_RTC_MAX_CNT - APP_TIMER_SAFE_WINDOW)
#define NRF_SUCCESS 0u
#define NRF_ERROR_TIMEOUT 1u
#define APP_TIMER_WITH_PROFILER 0

typedef uint32_t ret_code_t;
typedef struct { unsigned unused; } drv_rtc_t;
typedef struct {
    nrf_sortlist_item_t list_item;
    uint64_t end_val;
    uint32_t repeat_period;
    void (*handler)(void *);
    void *p_context;
} app_timer_t;

/* PRODUCTION_REQUEST_TYPES */

static drv_rtc_t m_rtc_inst;
static app_timer_t * volatile mp_active_timer;
static bool m_global_active = true;
static uint64_t m_base_counter, m_stamp64;
static bool compare_func(nrf_sortlist_item_t *, nrf_sortlist_item_t *);
NRF_SORTLIST_DEF(m_app_timer_sortlist, compare_func);
uint32_t app_timer_cnt_get(void);
uint32_t app_timer_cnt_diff_compute(uint32_t, uint32_t);

/* RTC shim: interrupt delivery can be delayed while its counter keeps running. */
static uint32_t counter, compare_value;
static unsigned start_calls, stop_calls;
static bool running;
static uint32_t drv_rtc_counter_get(const drv_rtc_t *instance) {
    (void)instance;
    return counter;
}
static uint32_t drv_rtc_compare_get(const drv_rtc_t *instance, unsigned channel) {
    (void)instance;
    (void)channel;
    return compare_value;
}
static void drv_rtc_start(const drv_rtc_t *instance) {
    (void)instance;
    start_calls++;
    running = true;
}
static void drv_rtc_stop(const drv_rtc_t *instance) {
    (void)instance;
    stop_calls++;
    running = false;
}
static void drv_rtc_compare_disable(const drv_rtc_t *instance, unsigned channel) {
    (void)instance;
    (void)channel;
}
static ret_code_t drv_rtc_windowed_compare_set(const drv_rtc_t *instance,
    unsigned channel, uint32_t value, uint32_t window) {
    (void)instance;
    (void)channel;
    (void)window;
    compare_value = value;
    return NRF_SUCCESS;
}

/* FIFO shim preserves the request queue and production processing order. */
typedef unsigned nrf_atfifo_item_get_t;
static timer_req_t requests[8];
static unsigned request_count;
#define m_req_fifo NULL
static timer_req_t *nrf_atfifo_item_get(void *fifo, nrf_atfifo_item_get_t *ctx) {
    (void)fifo;
    *ctx = 0;
    return request_count ? &requests[0] : NULL;
}
static bool nrf_atfifo_item_free(void *fifo, nrf_atfifo_item_get_t *ctx) {
    (void)fifo;
    (void)ctx;
    assert(request_count);
    for (unsigned i = 1; i < request_count; ++i) requests[i - 1] = requests[i];
    --request_count;
    return true;
}
static ret_code_t timer_req_schedule(app_timer_req_type_t type, app_timer_t *timer) {
    assert(request_count < 8);
    requests[request_count++] = (timer_req_t){type, timer};
    return NRF_SUCCESS;
}

/* PRODUCTION_TIMER_ENGINE */

static const uint32_t period = APP_TIMER_TICKS(1000);
static unsigned clock_callbacks, other_callbacks;
static uint32_t callback_delay;

static void clock_callback(void *context) {
    (void)context;
    ++clock_callbacks;
    /* Simulate one slow callback, then let overdue callbacks catch up. */
    counter += callback_delay;
    callback_delay = 0;
}
static void other_callback(void *context) {
    (void)context;
    ++other_callbacks;
}
static void requests_and_update(void) {
    timer_req_process(&m_rtc_inst);
    rtc_update(&m_rtc_inst);
}
static void deliver_compare(uint32_t at) {
    assert(running);
    counter = at;
    on_compare_evt(&m_rtc_inst);
    requests_and_update();
}

int main(void) {
    app_timer_t clock = {.end_val = APP_TIMER_IDLE_VAL, .repeat_period = 1,
                         .handler = clock_callback};
    app_timer_t other = {.end_val = APP_TIMER_IDLE_VAL, .handler = other_callback};

    rtc_update(&m_rtc_inst);
    assert(stop_calls == 1 && !running); /* Control: empty engine stops RTC. */
    stop_calls = 0;

    assert(app_timer_start(&clock, period, NULL) == NRF_SUCCESS);
    requests_and_update();
    assert(running && start_calls == 1 && mp_active_timer == &clock);
    assert(clock.end_val == period && compare_value == period && !stop_calls);

    deliver_compare(period);
    assert(clock_callbacks == 1 && clock.end_val == 2 * period);
    assert(mp_active_timer == &clock && running && !stop_calls);

    /* Three overdue periods must be caught up on the original time grid. */
    deliver_compare(4 * period + period / 4);
    assert(clock_callbacks == 4 && clock.end_val == 5 * period);
    assert(compare_value == 5 * period && running && !stop_calls);

    /* A queued timer can be stopped without stopping the active clock. */
    assert(app_timer_start(&other, 3 * period, NULL) == NRF_SUCCESS);
    requests_and_update();
    assert(mp_active_timer == &clock && sortlist_peek() == &other);
    assert(app_timer_stop(&other) == NRF_SUCCESS);
    requests_and_update();
    assert(!sortlist_peek() && mp_active_timer == &clock && !stop_calls);

    /* An earlier timer preempts the clock, then stop restores it. */
    assert(app_timer_start(&other, period / 4, NULL) == NRF_SUCCESS);
    requests_and_update();
    assert(mp_active_timer == &other && sortlist_peek() == &clock);
    assert(app_timer_stop(&other) == NRF_SUCCESS);
    requests_and_update();
    assert(mp_active_timer == &clock && running && !stop_calls);

    /* Delay inside callback must advance end_val by period, never now+period. */
    callback_delay = 2 * period + period / 4;
    deliver_compare(5 * period);
    assert(clock_callbacks == 7 && clock.end_val == 8 * period);
    assert(compare_value == 8 * period && running && !stop_calls);
    assert(other_callbacks == 0);

    /* Expiring a one-shot timer also leaves the permanent clock running. */
    assert(app_timer_start(&other, period / 4, NULL) == NRF_SUCCESS);
    requests_and_update();
    deliver_compare(counter + period / 4);
    assert(other_callbacks == 1 && other.end_val == APP_TIMER_IDLE_VAL);
    assert(mp_active_timer == &clock && running && !stop_calls);

    /* Stop the final timer: the SDK's idle-stop branch becomes reachable. */
    assert(app_timer_stop(&clock) == NRF_SUCCESS);
    requests_and_update();
    assert(!running && stop_calls == 1 && mp_active_timer == NULL);
    puts("PASS: SDK RTC idle-stop; permanent clock survives overdue/slow callbacks and other timer stops");
    return 0;
}
