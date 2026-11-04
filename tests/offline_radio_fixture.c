#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "EPD_config.h"
#include "epd_refresh_policy.h"

#define NRF_SUCCESS 0U
#define NRF_ERROR_INVALID_STATE 8U
#define BLE_CONN_HANDLE_INVALID 65535U
#define BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION 0x13U
#define BLE_ADV_MODE_FAST 1U
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_DEBUG(...) ((void)0)
#define APP_ERROR_CHECK(code) assert((code) == NRF_SUCCESS)
typedef enum { BLE_ADV_EVT_FAST, BLE_ADV_EVT_IDLE } ble_adv_evt_t;

/* Minimal direct-GPIOTE shims: main.c now owns the wakeup-pin PORT event
 * instead of linking the generic nrfx GPIOTE driver. */
#define NRF_GPIO_PIN_NOPULL 0U
#define NRF_GPIO_PIN_NOSENSE 0U
#define NRF_GPIO_PIN_SENSE_HIGH 1U
#define NRF_GPIOTE_EVENTS_PORT 0U
#define NRF_GPIOTE_INT_PORT_MASK 1U
#define WAKEUP_GPIO_IRQ_PRIORITY 6U
typedef unsigned nrf_gpio_pin_sense_t;
typedef unsigned nrf_gpiote_events_t;
enum { GPIOTE_IRQn = 6 };
static struct { unsigned adv_handle; } m_advertising = {5};
static struct { epd_config_t config; } m_epd;
static unsigned m_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool m_wakeup_armed, ready;
static uint8_t m_wakeup_pin = 0xFFU;
static unsigned starts, disconnects, sleeps, gpio_arm, gpio_disarm;
static unsigned disconnect_error;
static bool sense_armed;
static unsigned ble_advertising_start(void *adv, unsigned mode)
{ (void)adv; assert(mode == BLE_ADV_MODE_FAST); starts++; return 0; }
static unsigned sd_ble_gap_disconnect(unsigned handle, unsigned reason)
{ assert(handle == m_conn_handle && reason == 0x13); disconnects++; return disconnect_error; }
static bool epd_resources_ready(void) { return ready; }
static void sleep_mode_enter(void) { sleeps++; }
static void nrf_gpio_cfg_input(unsigned pin, unsigned pull)
{ assert(pin < 32); (void)pull; }
static void nrf_gpio_cfg_sense_set(unsigned pin, nrf_gpio_pin_sense_t sense)
{ assert(pin < 32); sense_armed = (sense != NRF_GPIO_PIN_NOSENSE); }
static void nrf_gpiote_int_enable(unsigned mask)
{ assert(mask == NRF_GPIOTE_INT_PORT_MASK); gpio_arm++; }
static void nrf_gpiote_int_disable(unsigned mask)
{ assert(mask == NRF_GPIOTE_INT_PORT_MASK); gpio_disarm++; }
static void nrf_gpiote_event_clear(nrf_gpiote_events_t event)
{ (void)event; } /* EVENTS_PORT is write-1-to-clear; PIN_CNF sense remains set. */
static bool nrf_gpiote_event_is_set(nrf_gpiote_events_t event)
{ (void)event; return sense_armed; }
static void NVIC_DisableIRQ(unsigned irq) { (void)irq; }
static void NVIC_ClearPendingIRQ(unsigned irq) { (void)irq; }
static void NVIC_SetPriority(unsigned irq, unsigned priority)
{ (void)irq; assert(priority == WAKEUP_GPIO_IRQ_PRIORITY); }
static void NVIC_EnableIRQ(unsigned irq) { (void)irq; }

/* PRODUCTION_HANDLERS */

int main(void)
{
    m_epd.config.display_mode = MODE_CLOCK;
    m_epd.config.wakeup_pin = 7;
    ready = true;
    advertising_start();
    m_conn_handle = 42;
    disconnect_for_run();
    assert(disconnects == 1 && starts == 1 && !m_wakeup_armed && gpio_arm == 0);
    m_conn_handle = BLE_CONN_HANDLE_INVALID;
    disconnect_for_run();
    assert(disconnects == 1);
    /* Disconnect preserves the normal timeout policy; only timeout arms wake. */
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(starts == 1 && sleeps == 0 && m_wakeup_armed && gpio_arm == 1);
    setup_wakeup_pin(7);
    assert(gpio_arm == 1);
    GPIOTE_IRQHandler();
    assert(!m_wakeup_armed && starts == 2 && gpio_disarm == 1 && m_wakeup_pin == 0xFFU);

    m_epd.config.wakeup_pin = 255;
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(starts == 3); /* No wake pin: advertising restarts as before. */
    m_conn_handle = 42;
    disconnect_error = NRF_ERROR_INVALID_STATE;
    disconnect_for_run();
    assert(disconnects == 2 && gpio_arm == 1);
    m_conn_handle = BLE_CONN_HANDLE_INVALID;
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(starts == 4);
    m_epd.config.wakeup_pin = 7;
    ready = false;
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(starts == 5 && !m_wakeup_armed); /* Damaged GUI resources stay repairable. */
    ready = true;
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(starts == 5 && m_wakeup_armed && sleeps == 0 && gpio_arm == 2);
    GPIOTE_IRQHandler();
    m_epd.config.display_mode = MODE_PICTURE;
    on_adv_evt(BLE_ADV_EVT_IDLE);
    assert(sleeps == 1); /* Keep the existing picture-mode timeout behavior. */
    puts("PASS: apply/disconnect preserves advertising timeout, wake and recovery policy");
}
