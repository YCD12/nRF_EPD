/* Firmware review 2026-11-04: conditional clock visibility vs. partial refresh.
 *
 * Background (docs/device-flash-review-20261017.md): a clock component with a
 * runtime visibility condition (UI_FLAG_SYNC_ONLY / JIEQI_AHEAD / JIEQI_TODAY)
 * may be omitted by the full-scene renderer, so a hardware OLD/NEW partial
 * refresh must never regenerate OLD for it.
 *
 * At current HEAD the rendering-side guard exists and works:
 *   GUI/ui_resources.c:254-266   ui_resources_clock_geom() only returns
 *                                partial_allowed=true for flags==0, default
 *                                colors and no conflicting overlapping component;
 *                                a SYNC_ONLY overlap only sets partial_sync_overlay.
 *   EPD/app/epd_clock_refresh.c:206-217  try() requires resource geometry +
 *                                partial_allowed + !(overlay in Jan-2025 month).
 *   EPD/app/epd_clock_refresh.c:224-228  any failure invalidates OLD.
 * This test proves that with the production UIR2 default payload and the real
 * ui_resources.c / epd_clock_refresh.c code.
 *
 * Residual defect this test also pins down (LOW severity, power/time only):
 * EPD_service.c calls epd_clock_refresh_mark_valid() after every successful
 * clock-mode full refresh without consulting the same geometry. epd_refresh_kind()
 * therefore answered EPD_REFRESH_KIND_PARTIAL for a conditional clock, the
 * service ran init_partial(), and only then epd_clock_refresh_try() refused.
 * The panel got an extra reset/partial configuration before the mandatory full
 * refresh on every periodic tick.
 *
 * FIXED 2026-11-04: epd_clock_refresh_try() grew a prepare_only parameter that
 * runs exactly the same eligibility checks (OLD baseline, UI geometry,
 * conditional-clock flags and the Jan-2025 overlay window) without drawing or
 * touching the panel. EPD_service.c calls it with prepare_only=true before
 * init_partial() and skips the panel partial configuration when it returns
 * false. Measured cost: +144 B Flash (free 2,484 -> 2,340 B); no RAM change.
 *
 * Wired into the default host suite (tests/run_host_tests.sh); it can also be
 * run alone:
 *   x86_64-w64-mingw32-gcc -Wall -Werror -std=c11 -DUI_RESOURCES_HOST_TEST \
 *      -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service -IGUI -Itests/stubs \
 *     EPD/app/epd_clock_refresh.c EPD/app/epd_refresh_policy.c \
 *     tests/stubs/ui_resources_stub.c tests/test_review_clock_partial_eligibility.c \
 *     -o output/work-20261104/bugs/build/test_review_clock_partial_eligibility.exe
 *   output/work-20261104/bugs/build/test_review_clock_partial_eligibility.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "GUI.h"
#include "clock_layout.h"
#include "epd_clock_refresh.h"
#include "epd_refresh_policy.h"
#include "ui_resources.h"

/* tests/stubs/ui_resources_stub.c (includes the real GUI/ui_resources.c) */
void ui_resources_stub_set_component(ui_component_id_t id, const ui_component_t *value);

/* ---- clock-render hooks required by EPD/app/epd_clock_refresh.c ---- */
static unsigned s_draw_calls;
static unsigned s_push_calls;
static unsigned s_refresh_calls;
static bool s_push_result = true;
static uint16_t s_last_lx, s_last_ly, s_last_lw, s_last_lh;

bool GUI_RenderValid(void) { return true; }
static void init_partial(const epd_model_t *epd) { (void)epd; }

/* Mirrors GUI/GUI.c:782-799 for the loaded resource geometry. */
void GetTimeRefreshAreaFromLocal(const gui_data_t *data,
                                 uint16_t lx, uint16_t ly, uint16_t lw, uint16_t lh,
                                 uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h) {
    clock_refresh_geom_t geom;
    s_last_lx = lx;
    s_last_ly = ly;
    s_last_lw = lw;
    s_last_lh = lh;
    memset(&geom, 0, sizeof(geom));
    if (data != NULL)
        (void)ui_resources_clock_geom(data->mode == MODE_CLOCK, 800U, 480U, &geom);
    if (x) *x = (uint16_t)(geom.origin_x + lx);
    if (y) *y = (uint16_t)(geom.origin_y + ly);
    if (w) *w = geom.canvas_w != 0U ? lw : 0U;
    if (h) *h = geom.canvas_h != 0U ? lh : 0U;
}

/* One page per strip: emit the same rectangle GUI/GUI.c:757-776 emits after
 * GFX_setWindow(s_time_window_x, s_time_window_y, s_time_window_w, s_time_window_h). */
void DrawGUI_ClockOnly(const gui_data_t *data, buffer_callback callback, void *callback_data) {
    static uint8_t page[(48U + 1U) * 208U];
    (void)data;
    s_draw_calls++;
    assert(callback != NULL);
    memset(page, 0xFF, sizeof(page));
    callback(callback_data, page, NULL, s_last_lx, s_last_ly, s_last_lw, s_last_lh);
}

static bool diff_push(const epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *old_bm, const uint8_t *new_bm) {
    (void)epd;
    assert(old_bm != NULL && new_bm != NULL);
    assert(w % 8U == 0U && w >= 8U && h != 0U);
    assert(x % 8U == 0U);
    assert((uint32_t)(w / 8U) * h <= 512U); /* EPD_CLOCK_OLD window buffer */
    s_push_calls++;
    return s_push_result;
}

static bool refresh_partial(const epd_model_t *epd, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h) {
    (void)epd;
    assert(x % 8U == 0U && w % 8U == 0U && w >= 16U && h >= 2U);
    s_refresh_calls++;
    return true;
}

int main(void) {
    epd_driver_t drv = {.diff_push = diff_push, .refresh_partial = refresh_partial,
                        .init_partial = init_partial};
    epd_model_t model = {.drv = &drv, .width = 800, .height = 480};
    ui_component_t clock;
    clock_refresh_geom_t split_geom, full_geom;
    static const uint8_t flags[] = {UI_FLAG_SYNC_ONLY, UI_FLAG_JIEQI_AHEAD,
                                    UI_FLAG_JIEQI_TODAY};
    const uint32_t ts0 = 1789347600U; /* 2026, outside the Jan-2025 overlay month */

    assert(ui_resources_ready());
    clock = *ui_resource_component(UI_COMP_SPLIT_TIME);
    assert(clock.kind == UI_KIND_CLOCK && clock.flags == 0U);

    /* Baseline: default split/full clock may use hardware OLD/NEW. */
    assert(ui_resources_clock_geom(false, 800U, 480U, &split_geom));
    assert(ui_resources_clock_geom(true, 800U, 480U, &full_geom));
    assert(split_geom.resource_geometry && split_geom.partial_allowed);
    assert(full_geom.resource_geometry && full_geom.partial_allowed);
    /* The default full-screen layout's SYNC_ONLY banner overlaps the clock;
     * the split default has no overlapping conditional component. */
    assert(full_geom.partial_sync_overlay);
    assert(!split_geom.partial_sync_overlay);

    /* 1) Rendering-side fix present: every conditional clock loses partial
     *    eligibility; a normal clock keeps it. */
    for (unsigned i = 0; i < sizeof(flags); i++) {
        ui_component_t mutant = clock;
        mutant.flags = flags[i];
        ui_resources_stub_set_component(UI_COMP_SPLIT_TIME, &mutant);
        assert(ui_resources_clock_geom(false, 800U, 480U, &split_geom));
        assert(!split_geom.partial_allowed);

        gui_data_t data = {.mode = MODE_CLOCK_CALENDAR, .width = 800, .height = 480,
                           .timestamp = ts0 + 60U};
        epd_clock_refresh_mark_valid(ts0); /* as EPD_service.c does */
        assert(epd_clock_refresh_is_valid());
        assert(!epd_clock_refresh_try(&model, &data, true));
        s_draw_calls = s_push_calls = s_refresh_calls = 0U;
        assert(!epd_clock_refresh_try(&model, &data, false));
        assert(s_draw_calls == 0U && s_push_calls == 0U && s_refresh_calls == 0U);
        /* The geometry/overlay guards inside try() return false WITHOUT
         * invalidating; the service caller does it after switching to the
         * full-refresh fallback. */
        assert(epd_clock_refresh_is_valid());

        /* 2) Service guard: the dry run EPD_service.c performs before
         *    init_partial() also returns false, so the panel is never switched
         *    to partial mode and no reset/configuration is wasted. The pure
         *    refresh-kind policy still says PARTIAL (the hardware is capable);
         *    the UI eligibility check lives in the dry run. */
        epd_clock_refresh_mark_valid(ts0);
        epd_refresh_kind_in_t kind_in = {
            .mode = MODE_CLOCK_CALENDAR,
            .timestamp = ts0 + 60U,
            .force_full_refresh = false,
            .last_full_clear = ts0,
            .clock_state_valid = epd_clock_refresh_is_valid(),
            .full_refresh_minutes = 0U,
            .hardware_partial_ok = true,
            .hour = 12U,
        };
        assert(epd_refresh_kind(&kind_in) == EPD_REFRESH_KIND_PARTIAL);
        assert(!epd_clock_refresh_try(&model, &data, true));
        ui_resources_stub_set_component(UI_COMP_SPLIT_TIME, &clock);
    }

    /* 3) Original bug guard: the default SYNC_ONLY banner shares the clock
     *    canvas during the initial wall-clock month, so OLD/NEW is refused
     *    there even for an otherwise partial-eligible clock. */
    {
        gui_data_t data = {.mode = MODE_CLOCK, .width = 800, .height = 480,
                           .timestamp = 1735689660U}; /* 2025-01-01 */
        assert(ui_resources_clock_geom(true, 800U, 480U, &full_geom));
        assert(full_geom.partial_allowed && full_geom.partial_sync_overlay);
        assert(!epd_clock_refresh_try(&model, &data, true));
        epd_clock_refresh_mark_valid(1735689600U);
        s_draw_calls = s_push_calls = s_refresh_calls = 0U;
        assert(!epd_clock_refresh_try(&model, &data, false));
        assert(s_draw_calls == 0U && s_push_calls == 0U && s_refresh_calls == 0U);
        /* Same guard style: try() refuses without invalidating; the service
         * invalidates when it falls back to the full refresh. */
        assert(epd_clock_refresh_is_valid());
    }

    /* 4) Control: an unconditional split clock in 2026 still uses the real
     *    OLD/NEW partial path (fix must not disable the feature globally). */
    {
        gui_data_t data = {.mode = MODE_CLOCK_CALENDAR, .width = 800, .height = 480,
                           .timestamp = ts0 + 60U};
        epd_clock_refresh_mark_valid(ts0);
        assert(epd_clock_refresh_try(&model, &data, true));
        s_draw_calls = s_push_calls = s_refresh_calls = 0U;
        s_push_result = true;
        assert(epd_clock_refresh_try(&model, &data, false));
        assert(s_draw_calls > 0U && s_push_calls > 0U && s_refresh_calls == 1U);
        assert(epd_clock_refresh_is_valid());
    }

    printf("PASS: conditional clock partial-refresh eligibility gate (no wasted "
           "init_partial) and OLD/NEW guards\n");
    return 0;
}
