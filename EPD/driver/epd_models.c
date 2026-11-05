/**
 * @file epd_models.c
 * @brief 编译进固件的屏幕型号注册表（driver 层）
 *
 * 新增面板时在此表登记型号；未登记的型号不会被链接，便于按产品裁剪 Flash。
 */
#include "epd_panel.h"

/* 面板实现（driver/ 目录）；新增面板时在此追加 extern 与表项。 */
extern const epd_model_t epd_uc8179_750_Partial_bwr;

static const epd_model_t* const epd_models[] = {
    &epd_uc8179_750_Partial_bwr,
};

#define EPD_MODEL_COUNT (sizeof(epd_models) / sizeof(epd_models[0]))

const epd_model_t* epd_get_model(epd_model_id_t id)
{
    for (uint8_t i = 0U; i < (uint8_t)EPD_MODEL_COUNT; i++) {
        if (epd_models[i]->id == id) return epd_models[i];
    }
    /* 未知 ID 回退到默认型号，保证空配置/旧记录仍可显示。 */
    return epd_models[0];
}

const epd_model_t* epd_init(epd_model_id_t id)
{
    const epd_model_t* epd = epd_get_model(id);
    if (epd != NULL && epd->drv != NULL && epd->drv->init != NULL) {
        epd->drv->init(epd);
    }
    return epd;
}

bool epd_model_supported(uint8_t id)
{
    for (uint8_t i = 0U; i < (uint8_t)EPD_MODEL_COUNT; i++) {
        if ((uint8_t)epd_models[i]->id == id) return true;
    }
    return false;
}
