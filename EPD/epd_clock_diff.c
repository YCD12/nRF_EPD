#include "epd_clock_diff.h"
#include "Lunar.h"

// Reference to the global graphics page buffer from Adafruit_GFX.c
extern uint8_t s_gfx_buffer[];

// Forward declaration of UC8179 custom RAM write function
extern void UC8179_Partial_Diff_Push(epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *old_bm, const uint8_t *new_bm);

// Forward declaration of Draw7Number from GUI.c
extern void Draw7Number(Adafruit_GFX *gfx, int n, unsigned int xLoc, unsigned int yLoc, char cS, unsigned int fC, unsigned int bC, int nD);

typedef struct {
    uint8_t h1;
    uint8_t h2;
    uint8_t m1;
    uint8_t m2;
    bool valid;
} clock_state_t;

static clock_state_t s_clock_prev = {0, 0, 0, 0, false};

void epd_clock_diff_seed(uint8_t hh, uint8_t mm) {
    s_clock_prev.h1 = hh / 10;
    s_clock_prev.h2 = hh % 10;
    s_clock_prev.m1 = mm / 10;
    s_clock_prev.m2 = mm % 10;
    s_clock_prev.valid = true;
}

void epd_clock_diff_invalidate(void) {
    s_clock_prev.valid = false;
}

static void draw_digit_bitmap(Adafruit_GFX *gfx, uint8_t *bm, uint16_t w, uint16_t h, uint8_t val, uint16_t offset_x, uint16_t cS, uint16_t margin_y) {
    GFX_begin(gfx, w, h, h);
    gfx->buffer = bm;
    GFX_firstPage(gfx);
    GFX_fillScreen(gfx, GFX_WHITE);
    Draw7Number(gfx, val, offset_x, margin_y, (char)cS, GFX_BLACK, GFX_WHITE, 1);
}

/**
 * @brief 绘制时钟冒号到局部缓冲区
 * @param gfx 临时图形句柄
 * @param bm 目标图像缓冲区指针
 * @param w 缓冲区宽度 (24像素)
 * @param h 缓冲区高度
 * @param cS 缩放因子 (6)
 * @param margin_y 垂直边距
 *
 * 功能说明:
 * 在 24 像素宽的缓冲区内，以 x = 0 绘制一个黑色的双点冒号，背景为白色。
 * 坐标计算与 GUI.c 中的 DrawTime 保持完全一致，确保与屏幕上的物理冒号完全对齐。
 */
static void draw_colon_bitmap(Adafruit_GFX *gfx, uint8_t *bm, uint16_t w, uint16_t h, uint16_t cS, uint16_t margin_y) {
    GFX_begin(gfx, w, h, h);
    gfx->buffer = bm;
    GFX_firstPage(gfx);
    GFX_fillScreen(gfx, GFX_WHITE);
    GFX_fillRect(gfx, 0, margin_y + (9 * cS) / 2 + 1, 2 * cS, 2 * cS, GFX_BLACK);
    GFX_fillRect(gfx, 0, margin_y + (27 * cS) / 2 + 3, 2 * cS, 2 * cS, GFX_BLACK);
}

bool epd_clock_diff_try_partial(epd_model_t *epd, uint32_t timestamp, display_mode_t mode) {
    // Only perform clock-specific partial refresh in MODE_CLOCK_CALENDAR
    if (mode != MODE_CLOCK_CALENDAR || !s_clock_prev.valid) {
        return false;
    }

    tm_t tm_now = {0};
    transformTime(timestamp, &tm_now);

    uint8_t new_vals[4];
    new_vals[0] = tm_now.tm_hour / 10;
    new_vals[1] = tm_now.tm_hour % 10;
    new_vals[2] = tm_now.tm_min / 10;
    new_vals[3] = tm_now.tm_min % 10;

    uint8_t old_vals[4] = {s_clock_prev.h1, s_clock_prev.h2, s_clock_prev.m1, s_clock_prev.m2};

    // Hardcode scale and position parameters for MODE_CLOCK_CALENDAR on 800x480
    // comp_x = 56, comp_y = 182, cS = 6, nD = 2
    int16_t comp_x = 56;
    int16_t comp_y = 182;
    uint16_t cS = 6;
    uint16_t margin_y = 8;

    uint16_t digit_h = 20 * cS + 4 + 2 * margin_y; // 140
    uint16_t align_w = 72;          // ((68 + 7) / 8) * 8

    int16_t min_x = 800;
    int16_t max_x = 0;

    int16_t x_offsets[4];
    x_offsets[0] = 0;
    x_offsets[1] = 68;              // d
    x_offsets[2] = 160;             // 2 * d + 4 * cS = 136 + 24
    x_offsets[3] = 228;             // 2 * d + 4 * cS + d = 160 + 68

    // Partition the global graphics buffer to save RAM
    uint8_t *old_bm = s_gfx_buffer;
    uint8_t *new_bm = s_gfx_buffer + 1500;

    bool min_tens_changed = (new_vals[2] != old_vals[2]);  // 分钟十位是否改变 | Whether minute tens digit has changed
    bool hour_changed = (new_vals[0] != old_vals[0] || new_vals[1] != old_vals[1]);  // 小时是否改变 | Whether hour digits have changed
    bool changed = false;

    for (int i = 0; i < 4; i++) {
        if (new_vals[i] == old_vals[i]) continue;

        int16_t rx = comp_x + x_offsets[i];
        int16_t aligned_x = (rx / 8) * 8;
        uint16_t offset_x = rx - aligned_x;

        Adafruit_GFX temp_gfx;

        draw_digit_bitmap(&temp_gfx, old_bm, align_w, digit_h, old_vals[i], offset_x, cS, margin_y);
        draw_digit_bitmap(&temp_gfx, new_bm, align_w, digit_h, new_vals[i], offset_x, cS, margin_y);

        UC8179_Partial_Diff_Push(epd, aligned_x, comp_y - margin_y, align_w, digit_h, old_bm, new_bm);

        if (aligned_x < min_x) min_x = aligned_x;
        if (aligned_x + align_w > max_x) max_x = aligned_x + align_w;
        changed = true;
    }

    if (changed) {
        if (max_x > min_x && epd->drv->refresh_partial) {
            // 如果分钟十位改变，或者小时改变，主动将窗口左边界扩展至覆盖冒号位置（192像素对齐）
            // 并向芯片的 RAM 写入相同的冒号图像，确保该区域的 DTM1 和 DTM2 完全相同（no change），
            // 使得冒号在该瞬间被同步扫描时，通过 Source 极与 VCOM 摆动同步（LUTKK）物理释放偏置电荷，
            // 且不会因为 RAM 内容不匹配而导致高电压驱动和串扰。
            if (min_tens_changed || hour_changed) {
                Adafruit_GFX temp_gfx;
                draw_colon_bitmap(&temp_gfx, old_bm, 24, digit_h, cS, margin_y);
                draw_colon_bitmap(&temp_gfx, new_bm, 24, digit_h, cS, margin_y);
                UC8179_Partial_Diff_Push(epd, 192, comp_y - margin_y, 24, digit_h, old_bm, new_bm);

                if (min_x > 192) {
                    min_x = 192;
                }
            }
            epd->drv->refresh_partial(epd, min_x, comp_y - margin_y, max_x - min_x, digit_h);
        }
        s_clock_prev.h1 = new_vals[0];
        s_clock_prev.h2 = new_vals[1];
        s_clock_prev.m1 = new_vals[2];
        s_clock_prev.m2 = new_vals[3];
    }

    return true;
}
