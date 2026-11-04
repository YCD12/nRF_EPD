#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "EPD_driver.h"
#include "UC8179.h"

typedef struct {
    uint8_t command;
    uint8_t data[64];
    size_t length;
} write_event_t;

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

static write_event_t events[128];
static size_t event_count;
static unsigned int reset_count;
static bool busy_ready = true;

extern const epd_model_t epd_uc8179_750_Partial_bwr;

static void clear_events(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0U;
}

static size_t find_command(uint8_t command, size_t start)
{
    for (size_t i = start; i < event_count; ++i) {
        if (events[i].command == command) {
            return i;
        }
    }
    return event_count;
}

static size_t count_command(uint8_t command)
{
    size_t count = 0U;

    for (size_t i = 0U; i < event_count; ++i) {
        if (events[i].command == command) {
            ++count;
        }
    }
    return count;
}

static void expect_data(size_t event, const uint8_t *expected, size_t length)
{
    assert(event < event_count);
    assert(events[event].length == length);
    assert(memcmp(events[event].data, expected, length) == 0);
}

void app_feed_wdt(void)
{
}

void EPD_WriteCmd(uint8_t command)
{
    assert(event_count < ARRAY_SIZE(events));
    events[event_count].command = command;
    ++event_count;
}

void EPD_WriteData(uint8_t *data, uint8_t length)
{
    write_event_t *event;

    assert(event_count > 0U);
    event = &events[event_count - 1U];
    assert(event->length + length <= sizeof(event->data));
    memcpy(&event->data[event->length], data, length);
    event->length += length;
}

void EPD_ReadData(uint8_t *data, uint8_t length)
{
    memset(data, 0, length);
}

void EPD_WriteByte(uint8_t value)
{
    EPD_WriteData(&value, 1U);
}

uint8_t EPD_ReadByte(void)
{
    return 0U;
}

void EPD_FillRAM(uint8_t command, uint8_t value, uint32_t length)
{
    EPD_WriteCmd(command);
    while (length-- > 0U) {
        EPD_WriteData(&value, 1U);
    }
}

void EPD_Reset(uint32_t value, uint16_t duration)
{
    (void)value;
    (void)duration;
    ++reset_count;
}

bool EPD_WaitBusy(uint32_t value, uint16_t timeout)
{
    (void)value;
    (void)timeout;
    return busy_ready;
}

bool EPD_BusyAbortRequested(void)
{
    return false;
}

static void test_partial_command_contract(void)
{
    static const uint8_t expected_power[] = {0x07, 0x17, 0x3F, 0x3F, 0x03};
    static const uint8_t expected_psr[] = {0x3F};
    static const uint8_t expected_cdi[] = {0xA9, 0x07};
    static const uint8_t expected_tres[] = {0x03, 0x20, 0x01, 0xE0};
    static const uint8_t expected_duspi[] = {0x00};
    static const uint8_t expected_tcon[] = {0x22};
    static const uint8_t expected_vdcs[] = {0x22};
    static const uint8_t expected_window[] = {
        0x00, 0x00, 0x00, 0x0F,
        0x00, 0x00, 0x00, 0x01,
        0x00,
    };
    static const uint8_t old_bitmap[] = {0xA5, 0x3C, 0xF0, 0x0F};
    static const uint8_t new_bitmap[] = {0x5A, 0xC3, 0x0F, 0xF0};
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    clear_events();
    reset_count = 0U;

    UC8179_Init_Partial(&model);
    assert(reset_count == 1U);

    size_t power = find_command(UC81xx_PWR, 0U);
    size_t tres = find_command(UC81xx_TRES, power + 1U);
    size_t duspi = find_command(UC81xx_DUSPI, tres + 1U);
    size_t tcon = find_command(UC81xx_TCON, duspi + 1U);
    size_t psr = find_command(UC81xx_PSR, tcon + 1U);
    size_t vdcs = find_command(UC81xx_VDCS, psr + 1U);
    size_t cdi = find_command(UC81xx_CDI, vdcs + 1U);
    expect_data(power, expected_power, sizeof(expected_power));
    expect_data(tres, expected_tres, sizeof(expected_tres));
    expect_data(duspi, expected_duspi, sizeof(expected_duspi));
    expect_data(tcon, expected_tcon, sizeof(expected_tcon));
    expect_data(psr, expected_psr, sizeof(expected_psr));
    expect_data(vdcs, expected_vdcs, sizeof(expected_vdcs));
    expect_data(cdi, expected_cdi, sizeof(expected_cdi));
    assert(power < tres && tres < duspi && duspi < tcon);
    assert(tcon < psr && psr < vdcs && vdcs < cdi);

    static const uint8_t lut_hold[6] = {0x00, 0x01, 0x2A, 0x02, 0x00, 0x01};
    static const uint8_t lut_kw[6] = {0x20, 0x01, 0x2A, 0x02, 0x00, 0x01};
    static const uint8_t lut_wk[6] = {0x10, 0x01, 0x2A, 0x02, 0x00, 0x01};
    static const uint8_t *const expected_luts[6] = {
        lut_hold, lut_hold, lut_kw, lut_wk, lut_hold, lut_hold,
    };

    size_t previous_lut = cdi;
    for (uint8_t i = 0U; i < 6U; ++i) {
        uint8_t command = (uint8_t)(UC81xx_LUTC + i);
        size_t lut = find_command(command, previous_lut + 1U);
        assert(lut < event_count);
        assert(events[lut].length == 42U);
        assert(memcmp(events[lut].data, expected_luts[i], 6U) == 0);
        for (size_t byte = 6U; byte < 42U; ++byte) {
            assert(events[lut].data[byte] == 0U);
        }
        previous_lut = lut;
    }

    assert(UC8179_Partial_Diff_Push(&model,
                                    0U,
                                    0U,
                                    16U,
                                    2U,
                                    old_bitmap,
                                    new_bitmap));

    size_t write_partial_in = find_command(UC81xx_PTIN, previous_lut + 1U);
    size_t write_window = find_command(UC81xx_PTL, write_partial_in + 1U);
    size_t dtm1 = find_command(UC81xx_DTM1, write_window + 1U);
    size_t dtm2 = find_command(UC81xx_DTM2, dtm1 + 1U);
    size_t write_partial_out = find_command(UC81xx_PTOUT, dtm2 + 1U);

    assert(previous_lut < write_partial_in);
    assert(write_partial_in < write_window && write_window < dtm1);
    assert(dtm1 < dtm2 && dtm2 < write_partial_out);
    expect_data(write_window, expected_window, sizeof(expected_window));
    expect_data(dtm1, old_bitmap, sizeof(old_bitmap));
    expect_data(dtm2, new_bitmap, sizeof(new_bitmap));

    /* RAM must be populated before the high-voltage supply is enabled. */
    assert(find_command(UC81xx_PON, 0U) == event_count);

    clear_events();
    assert(UC8179_Refresh_Partial(&model, 0U, 0U, 16U, 2U));

    size_t power_on = find_command(UC81xx_PON, 0U);
    size_t partial_in = find_command(UC81xx_PTIN, power_on + 1U);
    size_t window = find_command(UC81xx_PTL, partial_in + 1U);
    size_t refresh = find_command(UC81xx_DRF, window + 1U);
    size_t partial_out = find_command(UC81xx_PTOUT, refresh + 1U);
    size_t power_off = find_command(UC81xx_POF, partial_out + 1U);

    assert(power_on < partial_in);
    assert(partial_in < window);
    assert(window < refresh);
    assert(refresh < partial_out);
    assert(partial_out < power_off);
    expect_data(window, expected_window, sizeof(expected_window));
    assert(count_command(UC81xx_PON) == 1U);
    assert(count_command(UC81xx_DRF) == 1U);
    assert(count_command(UC81xx_POF) == 1U);
}

static void test_full_refresh_restores_full_mode(void)
{
    static const uint8_t expected_psr[] = {0x0F};
    static const uint8_t expected_vdcs[] = {0x00};
    static const uint8_t expected_cdi[] = {0x13, 0x07};
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    busy_ready = true;
    UC8179_ClearBusyTimeout();
    clear_events();
    UC8179_Init_Partial(&model);
    clear_events();
    epd_uc8179_750_Partial_bwr.drv->refresh(&model);

    size_t psr = find_command(UC81xx_PSR, 0U);
    size_t vdcs = find_command(UC81xx_VDCS, psr + 1U);
    size_t cdi = find_command(UC81xx_CDI, vdcs + 1U);
    size_t power_on = find_command(UC81xx_PON, cdi + 1U);
    size_t refresh = find_command(UC81xx_DRF, power_on + 1U);
    size_t power_off = find_command(UC81xx_POF, refresh + 1U);

    expect_data(psr, expected_psr, sizeof(expected_psr));
    expect_data(vdcs, expected_vdcs, sizeof(expected_vdcs));
    expect_data(cdi, expected_cdi, sizeof(expected_cdi));
    assert(psr < vdcs && vdcs < cdi && cdi < power_on);
    assert(power_on < refresh && refresh < power_off);
}

/* Refresh → Sleep：POF 恰好一次（Refresh 内），Sleep 只发 DSLP 0xA5。
 * 守住 UC8179_Sleep 不再重复 PowerOff 的“高压必已关闭”不变式（C-02）。 */
static void test_sleep_after_refresh_emits_single_power_off(void)
{
    static const uint8_t expected_dslp[] = {0xA5};
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    assert(epd_uc8179_750_Partial_bwr.drv->sleep != NULL);
    busy_ready = true;
    UC8179_ClearBusyTimeout();
    clear_events();
    UC8179_Init_Partial(&model);
    clear_events();
    epd_uc8179_750_Partial_bwr.drv->refresh(&model);

    /* 全刷自身在 DRF 之后恰好一次 POF。 */
    size_t refresh_pof = find_command(UC81xx_POF, 0U);
    size_t refresh_events = event_count;
    assert(refresh_pof < refresh_events);
    assert(count_command(UC81xx_POF) == 1U);

    epd_uc8179_750_Partial_bwr.drv->sleep(&model);

    /* Sleep 只发一条 DSLP：不得再发 POF，命令序列中 POF 仍恰好 1 次。 */
    assert(event_count == refresh_events + 1U);
    assert(count_command(UC81xx_POF) == 1U);
    assert(count_command(UC81xx_DSLP) == 1U);
    assert(find_command(UC81xx_POF, refresh_events) == event_count);

    size_t dsleep = find_command(UC81xx_DSLP, refresh_events);
    assert(dsleep == refresh_events);
    assert(refresh_pof < dsleep);
    expect_data(dsleep, expected_dslp, sizeof(expected_dslp));
}

static void test_power_on_failure_is_latched_and_powered_off(void)
{
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    clear_events();
    UC8179_ClearBusyTimeout();
    busy_ready = false;
    assert(!UC8179_Refresh_Partial(&model, 0U, 0U, 16U, 2U));
    assert(UC8179_HadBusyTimeout());

    size_t power_on = find_command(UC81xx_PON, 0U);
    size_t power_off = find_command(UC81xx_POF, power_on + 1U);
    assert(power_on < power_off);
    assert(find_command(UC81xx_DRF, 0U) == event_count);

    busy_ready = true;
    UC8179_ClearBusyTimeout();
}

static void test_partial_model_vtable(void)
{
    assert(epd_uc8179_750_Partial_bwr.drv != NULL);
    assert(epd_uc8179_750_Partial_bwr.drv->init_partial == UC8179_Init_Partial);
    assert(epd_uc8179_750_Partial_bwr.drv->diff_push == UC8179_Partial_Diff_Push);
    assert(epd_uc8179_750_Partial_bwr.drv->refresh_partial == UC8179_Refresh_Partial);
}

static void test_edge_window_is_accepted(void)
{
    static const uint8_t old_bitmap[4] = {0xFF, 0x00, 0xAA, 0x55};
    static const uint8_t new_bitmap[4] = {0x00, 0xFF, 0x55, 0xAA};
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    clear_events();
    assert(UC8179_Partial_Diff_Push(&model, 784U, 478U, 16U, 2U,
                                    old_bitmap, new_bitmap));
}

static void expect_window_rejected(const epd_model_t *model,
                                   uint16_t x,
                                   uint16_t y,
                                   uint16_t w,
                                   uint16_t h)
{
    static const uint8_t old_bitmap[4] = {0};
    static const uint8_t new_bitmap[4] = {0};

    clear_events();
    assert(!UC8179_Partial_Diff_Push(model,
                                     x,
                                     y,
                                     w,
                                     h,
                                     old_bitmap,
                                     new_bitmap));
    assert(event_count == 0U);

    clear_events();
    assert(!UC8179_Refresh_Partial(model, x, y, w, h));
    assert(event_count == 0U);
}

static void test_invalid_windows_are_rejected(void)
{
    epd_model_t model = {
        .id = EPD_UC8179_750_BWR,
        .color = EPD_COLOR_BWR,
        .drv = NULL,
        .width = 800U,
        .height = 480U,
    };

    /* PTL requires HRED > HRST and VRED > VRST. */
    expect_window_rejected(&model, 0U, 0U, 8U, 2U);
    expect_window_rejected(&model, 0U, 0U, 16U, 1U);
    expect_window_rejected(&model, 0U, 0U, 0U, 2U);
    expect_window_rejected(&model, 0U, 0U, 16U, 0U);

    /* DTM rows are byte-oriented; do not silently shift or widen input. */
    expect_window_rejected(&model, 1U, 0U, 16U, 2U);
    expect_window_rejected(&model, 0U, 0U, 15U, 2U);

    /* Do not clip: caller-provided OLD/NEW buffers describe the exact window. */
    expect_window_rejected(&model, 792U, 0U, 16U, 2U);
    expect_window_rejected(&model, 0U, 479U, 16U, 2U);
}

int main(void)
{
    test_partial_command_contract();
    test_full_refresh_restores_full_mode();
    test_sleep_after_refresh_emits_single_power_off();
    test_power_on_failure_is_latched_and_powered_off();
    test_partial_model_vtable();
    test_edge_window_is_accepted();
    test_invalid_windows_are_rejected();
    puts("PASS: UC8179 official partial-refresh command contract");
    return 0;
}
