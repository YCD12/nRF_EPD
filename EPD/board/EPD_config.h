#ifndef __EPD_CONFIG_H
#define __EPD_CONFIG_H

#ifndef BLE_LOG_REDIRECT_ALL_NRF_LOG
#define BLE_LOG_REDIRECT_ALL_NRF_LOG 1
#endif

#include <stdbool.h>
#include <stdint.h>

// EPD 配置
typedef struct {
    uint8_t mosi_pin;             // SPI MOSI
    uint8_t sclk_pin;             // SPI SCLK
    uint8_t cs_pin;               // SPI 片选
    uint8_t dc_pin;               // 数据/命令
    uint8_t rst_pin;              // 复位
    uint8_t busy_pin;             // 忙状态
    uint8_t bs_pin;               // 总线选择
    uint8_t model_id;             // EPD 型号 ID
    uint8_t wakeup_pin;           // 唤醒
    uint8_t led_pin;              // LED
    uint8_t en_pin;               // 使能
    uint8_t display_mode;         // 显示模式
    uint8_t week_start;           // 周起始日（0=周日，1=周一）
    uint8_t full_refresh_minutes; // 全刷间隔（分钟；0=每小时，30=每 30 分钟）
} epd_config_t;

#define EPD_CONFIG_SIZE (sizeof(epd_config_t) / sizeof(uint8_t))

uint32_t epd_config_init(epd_config_t* cfg);
void epd_config_process(void);             // Main-context external-Flash save/delete
bool epd_config_clear_complete(void);
bool epd_config_busy(void);
bool epd_config_is_saved(const epd_config_t* cfg);
/* Transient boot/activation read errors must not be saved as defaults.
 * Retry in main at most once per second; true means cfg has a terminal result
 * (valid bytes or erased bytes for absent/invalid data). Explicit writes cancel. */
bool epd_config_read_pending(void);
bool epd_config_retry_read(epd_config_t* cfg);
bool epd_config_take_save_result(uint8_t* result, uint32_t* revision, uint32_t* data_crc);
void epd_config_read(epd_config_t* cfg);   // 从 Flash 读取
void epd_config_write(epd_config_t* cfg);  // 写入 Flash
void epd_config_clear(epd_config_t* cfg);  // 清除 Flash 中的配置
bool epd_config_empty(epd_config_t* cfg);  // 是否为空（全 0xFF）

#endif
