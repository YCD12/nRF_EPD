#ifndef __EPD_CONFIG_H
#define __EPD_CONFIG_H
#include <stdbool.h>
#include <stdint.h>

// EPD (E-Paper Display) configuration structure
// EPD（电子墨水屏）配置结构体
typedef struct {
    uint8_t mosi_pin;                  // SPI MOSI pin (Master Out Slave In) - SPI主出从入引脚
    uint8_t sclk_pin;                  // SPI SCLK pin (Serial Clock) - SPI串行时钟引脚
    uint8_t cs_pin;                    // SPI CS pin (Chip Select) - SPI片选引脚
    uint8_t dc_pin;                    // Data/Command control pin - 数据/命令控制引脚
    uint8_t rst_pin;                   // Reset pin - 复位引脚
    uint8_t busy_pin;                  // Busy status pin - 忙状态引脚
    uint8_t bs_pin;                    // Bus select pin - 总线选择引脚
    uint8_t model_id;                  // EPD model ID - EPD型号ID
    uint8_t wakeup_pin;                // Wakeup pin - 唤醒引脚
    uint8_t led_pin;                   // LED pin - LED指示灯引脚
    uint8_t en_pin;                    // Enable pin - 使能引脚
    uint8_t display_mode;              // Display mode - 显示模式
    uint8_t week_start;                // Week start day (0=Sunday, 1=Monday) - 周起始日（0=周日，1=周一）
    uint8_t full_refresh_minutes;      // Full refresh interval in minutes (0=hourly, 30=every 30min, etc.) - 全刷新时间间隔（分钟）（0=每小时，30=每30分钟，等）
} epd_config_t;

// EPD configuration size in bytes
// EPD配置大小（字节数）
#define EPD_CONFIG_SIZE (sizeof(epd_config_t) / sizeof(uint8_t))

// Initialize EPD configuration and Flash Data Storage (FDS)
// 初始化EPD配置和Flash数据存储(FDS)
void epd_config_init(epd_config_t* cfg);

// Read EPD configuration from flash memory
// 从Flash存储器读取EPD配置
void epd_config_read(epd_config_t* cfg);

// Write EPD configuration to flash memory
// 将EPD配置写入Flash存储器
void epd_config_write(epd_config_t* cfg);

// Clear EPD configuration from flash memory
// 从Flash存储器清除EPD配置
void epd_config_clear(epd_config_t* cfg);

// Check if EPD configuration is empty (all bytes are 0xFF)
// 检查EPD配置是否为空（所有字节都是0xFF）
bool epd_config_empty(epd_config_t* cfg);

#endif
