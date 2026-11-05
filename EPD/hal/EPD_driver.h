/**
 * EPD 驱动程序头文件
 *
 * 电子墨水屏底层驱动接口
 */
#ifndef __EPD_DRIVER_H
#define __EPD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "EPD_config.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "epd_panel.h"

/* 面板型号/驱动虚表/型号注册表已下沉到 hal/epd_panel.h（driver 层公共接口）；
 * 本头文件只保留 MCU 外设访问原语（SPI/GPIO/复位/BUSY/LED/ADC）。
 * UC81xx 系列命令码由 driver/uc81xx.h 提供。 */

// GPIO 电平
#define LOW (0x0)
#define HIGH (0x1)

// GPIO 模式
#define DEFAULT (0xFF)        // 默认（断开）
#define INPUT (0x0)           // 输入
#define OUTPUT (0x1)          // 输出
#define INPUT_PULLUP (0x2)    // 输入上拉
#define INPUT_PULLDOWN (0x3)  // 输入下拉

// 类 Arduino 封装
void pinMode(uint32_t pin, uint32_t mode);
#define digitalWrite(pin, value) nrf_gpio_pin_write(pin, value)
#define digitalRead(pin) nrf_gpio_pin_read(pin)
#define delay(ms) nrf_delay_ms(ms)

// GPIO
void EPD_GPIO_Load(epd_config_t* cfg);  // 从配置加载引脚
void EPD_GPIO_Init(void);               // 初始化 GPIO
void EPD_GPIO_Ensure(void);             // 未初始化则初始化，不增加额外引用
void EPD_GPIO_Uninit(void);             // 恢复默认状态

// SPI
void EPD_SPI_Write(uint8_t* value, uint8_t len); /* DC 已由调用方设好 */
void EPD_SPI_Read(uint8_t* value, uint8_t len);  /* MOSI 改输入作半双工读 */

// EPD 基础操作
void EPD_WriteCmd(uint8_t cmd);           /* DC=0 */
void EPD_WriteData(uint8_t* value, uint8_t len); /* DC=1 */
void EPD_ReadData(uint8_t* value, uint8_t len);
void EPD_WriteByte(uint8_t value);    /* 单字节数据 */
uint8_t EPD_ReadByte(void);           /* 单字节数据 */
// 一次写入命令及若干数据字节
#define EPD_Write(cmd, ...)                  \
    do {                                     \
        uint8_t _data[] = {__VA_ARGS__};     \
        EPD_WriteCmd(cmd);                   \
        EPD_WriteData(_data, sizeof(_data)); \
    } while (0)
void EPD_FillRAM(uint8_t cmd, uint8_t value, uint32_t len);  // 用相同值填充显存
void EPD_Reset(uint32_t value, uint16_t duration);           // 复位脚复位
bool EPD_WaitBusy(uint32_t value, uint16_t timeout);         // 等忙结束，超时返回 false
void EPD_RequestBusyAbort(void);    // 传图打断时钟刷新等待
void EPD_ClearBusyAbort(void);      // 开始新一轮刷新前清除
bool EPD_BusyAbortRequested(void);  // WaitBusy 是否应提前退出

// LED：低电平有效（LOW=亮，HIGH=灭）
/**
 * LED 行为：
 * - 屏幕刷新/唤醒：保持熄灭
 * - BLE 连接/断开：闪一次
 * - 低电量保持（<=2.80 V）：每 30 s 闪一次
 */
void EPD_LED_ON(void);
void EPD_LED_OFF(void);
void EPD_LED_BLINK(void);  // BLE 连接/断开、低电量保持时闪一次

// VDD 电压（毫伏）
/**
 * 电压读取与低电量检测：
 * - EPD_ReadVoltage()：直接读 ADC（mV）
 * - EPD_ReadVoltageAndCache()：读取并缓存，供后续低电量判断
 *
 * 电压缓存在每次屏幕刷新时更新，低电量判断基于最近一次刷新时的电压。
 */
#define LOW_BATTERY_THRESHOLD_MV 2800      // 低电量阈值（mV）
#define CRITICAL_BATTERY_SHUTDOWN_MV 2700  // 临界关机阈值（mV）
/* Main context only; 0 means unavailable/timeout, never a critical voltage. */
uint16_t EPD_ReadVoltage(void);            // 例如 3300 表示 3.3V
uint16_t EPD_ReadVoltageAndCache(void);    // 读取并缓存

/* 面板驱动层 BUSY 超时标志（全局，历史语义）。
 * 具体驱动在 WaitBusy 超时时置位；服务层在初始化/刷新前后清理、查询。
 * 面板实例尚未创建（睡眠后）时也可读写，不依赖具体驱动头文件。 */
void EPD_PanelBusyErrorSet(void);
void EPD_PanelBusyErrorClear(void);
bool EPD_PanelBusyError(void);

#endif
