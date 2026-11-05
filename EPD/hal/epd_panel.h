/**
 * @file epd_panel.h
 * @brief 墨水屏面板模型与驱动虚表（driver 层公共接口）
 *
 * 分层约定（见 docs/epd-architecture-20261104.md）：
 * - hal/ 只提供 SPI/GPIO/ADC/BUSY 等 MCU 外设原语；
 * - driver/ 的面板实现（如 UC8179）实现本文件的 epd_driver_t；
 * - service/app 只依赖本头文件中的型号/虚表/包装，不直接包含具体面板头，
 *   新增屏幕时无需修改服务层。
 */
#ifndef EPD_PANEL_H
#define EPD_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 驱动 IC 类型（面板实现内部使用） */
typedef enum {
    EPD_DRIVER_IC_UC8179 = 0x12,
} epd_driver_ic_t;

/* 颜色模式 */
typedef enum {
    EPD_COLOR_BW = 1,
    EPD_COLOR_BWR = 2,
    EPD_COLOR_BWRY = 3,
} epd_color_t;

/* 型号 ID：对外/持久化/网页协议共享，不要修改已有编号 */
typedef enum {
    EPD_UC8179_750_BWR = 7, /* 7.5 寸 800x480 黑白红（UC8179） */
} epd_model_id_t;

struct epd_driver;

/**@brief 屏幕型号描述（宽高按面板像素） */
typedef struct {
    epd_model_id_t id;
    epd_color_t color;
    const struct epd_driver* drv;
    uint16_t width;
    uint16_t height;
} epd_model_t;

/**@brief 面板驱动函数表
 *
 * 必选：init/clear/write_image/write_ram/refresh/sleep。
 * 可选：read_temp、真 OLD/NEW 局刷（init_partial/refresh_partial/diff_push）、
 * 驱动级 BUSY 错误状态（clear_busy_error/busy_error）。
 */
typedef struct epd_driver {
    epd_driver_ic_t ic;
    void (*init)(const epd_model_t* epd);
    void (*clear)(const epd_model_t* epd, bool refresh);
    void (*write_image)(const epd_model_t* epd, uint8_t* black, uint8_t* color,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*write_ram)(const epd_model_t* epd, uint8_t cfg, uint8_t* data, uint8_t len);
    void (*refresh)(const epd_model_t* epd);
    void (*sleep)(const epd_model_t* epd);
    int8_t (*read_temp)(const epd_model_t* epd);

    /* 可选：真 OLD/NEW 局刷路径 */
    void (*init_partial)(const epd_model_t* epd);
    bool (*refresh_partial)(const epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w,
                            uint16_t h);
    bool (*diff_push)(const epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t* old_bm, const uint8_t* new_bm);

    /* 可选：驱动级 BUSY 错误状态（服务层只通过下方包装访问） */
    void (*clear_busy_error)(const epd_model_t* epd);
    bool (*busy_error)(const epd_model_t* epd);
} epd_driver_t;

/* ---- 型号注册表（driver/epd_models.c） ---- */
/** 按 ID 查型号；未命中回退到表中第一项（保持历史行为）。 */
const epd_model_t* epd_get_model(epd_model_id_t id);
/** 查型号并调用 drv->init（面板复位 + 寄存器配置）。 */
const epd_model_t* epd_init(epd_model_id_t id);
/** 该型号 ID 是否已编译进固件（配置校验/网页回填使用）。 */
bool epd_model_supported(uint8_t id);

/* ---- 可选能力的空指针安全包装（service/app 使用） ----
 * 保持 static inline：实测改为 out-of-line/noinline 后 LTO 反增 16 B
 * （55,948 B vs 55,932 B），故不回退为外置函数。 */
static inline void epd_panel_clear_busy_error(const epd_model_t* epd)
{
    if (epd != NULL && epd->drv != NULL && epd->drv->clear_busy_error != NULL)
        epd->drv->clear_busy_error(epd);
}

static inline bool epd_panel_busy_error(const epd_model_t* epd)
{
    return epd != NULL && epd->drv != NULL && epd->drv->busy_error != NULL &&
           epd->drv->busy_error(epd);
}

/** 面板是否支持真 OLD/NEW 局刷（三项可选操作都存在）。 */
static inline bool epd_panel_can_partial(const epd_model_t* epd)
{
    return epd != NULL && epd->drv != NULL && epd->drv->init_partial != NULL &&
           epd->drv->refresh_partial != NULL && epd->drv->diff_push != NULL;
}

#endif /* EPD_PANEL_H */
