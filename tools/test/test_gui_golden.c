/*
 * GUI 黄金图回归（主机无头渲染）- Golden Image Regression (headless host)
 * -----------------------------------------------------------------------
 * 用与真机相同的 GUI/Lunar/Adafruit_GFX 源码 + res_hal_host 读取
 * tools/epd_resources.bin，渲染固定场景后对整个帧缓冲做 CRC32，
 * 与嵌入的期望哈希比对，锁死"资源包 + 渲染路径"的像素级一致性。
 *
 * 编译:
 *   make -f tools/test/Makefile.golden
 * 运行:
 *   /tmp/test_gui_golden tools/epd_resources.bin
 * 更新期望哈希（资源/布局变更后 intentional）:
 *   /tmp/test_gui_golden tools/epd_resources.bin --update-golden
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GUI.h"
#include "Lunar.h"
#include "res_hal.h"
#include "res_hal_host.h"

#define GOLDEN_WIDTH   800U
#define GOLDEN_HEIGHT  480U
/* 2026-06-15 08:00:00 本地时间（固定场景，便于跨平台复现） */
#define GOLDEN_TIMESTAMP 1781481600UL

/* 由 --update-golden 生成；变更资源/UI 后需重跑更新 */
#define GOLDEN_CRC32_EXPECTED 0x8678EADBUL

typedef struct {
    uint8_t* black;
    uint8_t* color;
    uint16_t width;
    uint16_t height;
} frame_capture_t;

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    while (len-- > 0) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc;
}

static uint32_t crc32_finalize(uint32_t crc) {
    return ~crc;
}

static void capture_page(void* user_data, uint8_t* black, uint8_t* color,
                       uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    frame_capture_t* cap = (frame_capture_t*)user_data;
    uint16_t row_bytes = (cap->width + 7U) / 8U;
    uint16_t src_row_bytes = (w + 7U) / 8U;

    if (black == NULL) {
        return;
    }

    for (uint16_t row = 0; row < h; row++) {
        uint32_t dst_off = (uint32_t)(y + row) * row_bytes + (x / 8U);
        uint32_t src_off = (uint32_t)row * src_row_bytes;
        memcpy(cap->black + dst_off, black + src_off, src_row_bytes);
        if (color != NULL && cap->color != NULL) {
            memcpy(cap->color + dst_off, color + src_off, src_row_bytes);
        }
    }
}

static uint32_t frame_crc32(const frame_capture_t* cap) {
    uint32_t crc = 0xFFFFFFFFUL;
    size_t plane_bytes = (size_t)((cap->width + 7U) / 8U) * cap->height;

    crc = crc32_update(crc, cap->black, plane_bytes);
    if (cap->color != NULL) {
        crc = crc32_update(crc, cap->color, plane_bytes);
    }
    return crc32_finalize(crc);
}

static int render_scene(uint32_t* out_crc) {
    frame_capture_t cap;
    size_t plane_bytes;
    gui_data_t data;
    uint32_t crc;

    plane_bytes = (size_t)((GOLDEN_WIDTH + 7U) / 8U) * GOLDEN_HEIGHT;
    cap.width = GOLDEN_WIDTH;
    cap.height = GOLDEN_HEIGHT;
    cap.black = (uint8_t*)calloc(1, plane_bytes);
    cap.color = (uint8_t*)calloc(1, plane_bytes);
    if (cap.black == NULL || cap.color == NULL) {
        free(cap.black);
        free(cap.color);
        return -1;
    }

    memset(cap.black, 0xFF, plane_bytes);
    memset(cap.color, 0xFF, plane_bytes);

    lunar_db_reset();
    if (GUI_ReloadUICache(GOLDEN_WIDTH, GOLDEN_HEIGHT, 2U) != 0) {
        fprintf(stderr, "GUI_ReloadUICache failed\n");
        free(cap.black);
        free(cap.color);
        return -1;
    }

    data.mode = MODE_CALENDAR;
    data.color = 2;
    data.width = GOLDEN_WIDTH;
    data.height = GOLDEN_HEIGHT;
    data.timestamp = GOLDEN_TIMESTAMP;
    data.week_start = 0;
    data.temperature = 25;
    data.voltage_mv = 3300;
    strncpy(data.ssid, "NRF_EPD_TEST", sizeof(data.ssid) - 1);
    data.ssid[sizeof(data.ssid) - 1] = '\0';

    DrawGUI(&data, capture_page, &cap);

    crc = frame_crc32(&cap);
    if (out_crc != NULL) {
        *out_crc = crc;
    }

    free(cap.black);
    free(cap.color);
    return 0;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "tools/epd_resources.bin";
    bool update_golden = false;
    uint32_t crc = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--update-golden") == 0) {
            update_golden = true;
        }
    }

    if (res_hal_host_init(path) != 0) {
        fprintf(stderr, "res_hal_host_init failed: %s\n", path);
        return 2;
    }

    if (render_scene(&crc) != 0) {
        res_hal_host_shutdown();
        return 3;
    }

    res_hal_host_shutdown();

    if (update_golden) {
        printf("GOLDEN_CRC32=0x%08lX  /* update GOLDEN_CRC32_EXPECTED in test_gui_golden.c */\n",
               (unsigned long)crc);
        return 0;
    }

    if (GOLDEN_CRC32_EXPECTED == 0U) {
        fprintf(stderr, "GOLDEN_CRC32_EXPECTED not set. Run with --update-golden first.\n");
        printf("computed CRC32=0x%08lX\n", (unsigned long)crc);
        return 4;
    }

    if (crc != GOLDEN_CRC32_EXPECTED) {
        fprintf(stderr, "golden mismatch: expected 0x%08lX got 0x%08lX\n",
                (unsigned long)GOLDEN_CRC32_EXPECTED, (unsigned long)crc);
        return 1;
    }

    printf("==== GUI golden: CRC32=0x%08lX OK (%ux%u calendar BWR) ====\n",
           (unsigned long)crc, GOLDEN_WIDTH, GOLDEN_HEIGHT);
    return 0;
}
