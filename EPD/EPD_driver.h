/**
 * EPD驱动程序头文件
 * EPD Driver Header File
 *
 * 提供电子墨水屏(E-Paper Display)的底层驱动接口
 * Provides low-level driver interface for E-Paper Display
 */
#ifndef __EPD_DRIVER_H
#define __EPD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "EPD_config.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"

// EPD driver IC types
// EPD驱动芯片类型
typedef enum {
    EPD_DRIVER_IC_UC8179 = 0x12,  // UC8179驱动芯片
} epd_driver_ic_t;

// UC81xx commands
// UC81xx系列驱动芯片命令集
enum {
    UC81xx_PSR = 0x00,    // Panel Setting - 面板设置
    UC81xx_PWR = 0x01,    // Power Setting - 电源设置
    UC81xx_POF = 0x02,    // Power OFF - 电源关闭
    UC81xx_PFS = 0x03,    // Power OFF Sequence Setting - 电源关闭序列设置
    UC81xx_PON = 0x04,    // Power ON - 电源开启
    UC81xx_PMES = 0x05,   // Power ON Measure - 电源开启测量
    UC81xx_BTST = 0x06,   // Booster Soft Start - 升压软启动
    UC81xx_DSLP = 0x07,   // Deep sleep - 深度睡眠
    UC81xx_DTM1 = 0x10,   // Display Start Transmission 1 - 显示开始传输1(黑白数据)
    UC81xx_DSP = 0x11,    // Data Stop - 数据停止
    UC81xx_DRF = 0x12,    // Display Refresh - 显示刷新
    UC81xx_DTM2 = 0x13,   // Display Start transmission 2 - 显示开始传输2(红色数据)
    UC81xx_LUTC = 0x20,   // VCOM LUT (LUTC) - VCOM查找表
    UC81xx_LUTWW = 0x21,  // W2W LUT (LUTWW) - 白到白查找表
    UC81xx_LUTBW = 0x22,  // B2W LUT (LUTBW / LUTR) - 黑到白查找表
    UC81xx_LUTWB = 0x23,  // W2B LUT (LUTWB / LUTW) - 白到黑查找表
    UC81xx_LUTBB = 0x24,  // B2B LUT (LUTBB / LUTB) - 黑到黑查找表
    UC81xx_PLL = 0x30,    // PLL control - 锁相环控制
    UC81xx_TSC = 0x40,    // Temperature Sensor Calibration - 温度传感器校准
    UC81xx_TSE = 0x41,    // Temperature Sensor Selection - 温度传感器选择
    UC81xx_TSW = 0x42,    // Temperature Sensor Write - 温度传感器写入
    UC81xx_TSR = 0x43,    // Temperature Sensor Read - 温度传感器读取
    UC81xx_CDI = 0x50,    // Vcom and data interval setting - Vcom和数据间隔设置
    UC81xx_LPD = 0x51,    // Lower Power Detection - 低电量检测
    UC81xx_TCON = 0x60,   // TCON setting - 时序控制器设置
    UC81xx_TRES = 0x61,   // Resolution setting - 分辨率设置
    UC81xx_GSST = 0x65,   // GSST Setting - 门驱动源设置
    UC81xx_REV = 0x70,    // Revision - 版本号
    UC81xx_FLG = 0x71,    // Get Status - 获取状态
    UC81xx_AMV = 0x80,    // Auto Measurement Vcom - 自动测量Vcom
    UC81xx_VV = 0x81,     // Read Vcom Value - 读取Vcom值
    UC81xx_VDCS = 0x82,   // VCM_DC Setting - VCM直流设置
    UC81xx_PTL = 0x90,    // Partial Window - 部分刷新窗口
    UC81xx_PTIN = 0x91,   // Partial In - 进入部分刷新
    UC81xx_PTOUT = 0x92,  // Partial Out - 退出部分刷新
    UC81xx_PGM = 0xA0,    // Program Mode - 编程模式
    UC81xx_APG = 0xA1,    // Active Progrmming - 激活编程
    UC81xx_ROTP = 0xA2,   // Read OTP - 读取一次性可编程存储器
    UC81xx_CCSET = 0xE0,  // Cascade Setting - 级联设置
    UC81xx_PWS = 0xE3,    // Power Saving - 省电模式
    UC81xx_TSSET = 0xE5,  // Force Temperauture - 强制温度
};

// EPD color type - 电子墨水屏颜色类型
typedef enum {
    EPD_COLOR_BW = 1,    // 黑白两色
    EPD_COLOR_BWR = 2,   // 黑白红三色
    EPD_COLOR_BWRY = 3,  // 黑白红黄四色
} epd_color_t;

// Do not change the existing IDs!
// 不要修改现有的ID！这些ID用于识别不同的EPD型号
typedef enum {
    EPD_UC8179_750_BWR = 7,  // 7.5寸黑白红三色墨水屏(UC8179驱动)
} epd_model_id_t;

struct epd_driver;

// EPD型号结构体 - EPD Model Structure
typedef struct {
    epd_model_id_t id;      // 型号ID - Model ID
    epd_color_t color;      // 颜色类型 - Color type
    struct epd_driver* drv; // 驱动指针 - Driver pointer
    uint16_t width;         // 屏幕宽度(像素) - Screen width in pixels
    uint16_t height;        // 屏幕高度(像素) - Screen height in pixels
} epd_model_t;

/**@brief EPD driver structure.
 * EPD驱动结构体
 *
 * @details This structure contains epd driver functions.
 * 此结构体包含EPD驱动的所有功能函数
 */
typedef struct epd_driver {
    epd_driver_ic_t ic;                            /**< EPD driver IC type - EPD驱动芯片类型 */
    void (*init)(epd_model_t* epd);                /**< Initialize the e-Paper register - 初始化电子墨水屏寄存器 */
    void (*clear)(epd_model_t* epd, bool refresh); /**< Clear screen - 清屏(refresh=true则立即刷新显示) */
    void (*write_image)(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h);                                              /**< write image - 写入图像数据到显存 */
    void (*write_ram)(epd_model_t* epd, uint8_t cfg, uint8_t* data, uint8_t len); /* write data to epd ram - 直接写入数据到EPD显存 */
    void (*refresh)(epd_model_t* epd);     /**< Sends the image buffer in RAM to e-Paper and displays - 将显存数据刷新到屏幕上显示 */
    void (*sleep)(epd_model_t* epd);       /**< Enter sleep mode - 进入睡眠模式以降低功耗 */
    int8_t (*read_temp)(epd_model_t* epd); /**< Read temperature from driver chip - 从驱动芯片读取温度值 */
    // Partial refresh functions (optional, for UC8179)
    // 部分刷新功能函数(可选,用于UC8179芯片)
    void (*init_partial)(epd_model_t *epd);         /**< Initialize for partial refresh mode - 初始化部分刷新模式 */
    void (*clear_partial)(epd_model_t *epd, bool refresh);  /**< Clear screen using partial refresh - 使用部分刷新方式清屏 */
    void (*write_image_partial)(epd_model_t *epd, uint8_t *black, uint8_t *color, uint16_t x, uint16_t y, uint16_t w, uint16_t h); /**< write image for partial refresh - 写入图像数据用于部分刷新 */
    void (*refresh_partial)(epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h);  /**< Partial refresh a specific area - 局部刷新指定区域 */
} epd_driver_t;

// GPIO电平定义 - GPIO level definitions
#define LOW (0x0)   // 低电平
#define HIGH (0x1)  // 高电平

// GPIO模式定义 - GPIO mode definitions
#define DEFAULT (0xFF)          // 默认状态(断开连接)
#define INPUT (0x0)             // 输入模式
#define OUTPUT (0x1)            // 输出模式
#define INPUT_PULLUP (0x2)      // 输入上拉模式
#define INPUT_PULLDOWN (0x3)    // 输入下拉模式

// Arduino like function wrappers
// 类Arduino风格的函数封装
void pinMode(uint32_t pin, uint32_t mode);  // 设置引脚模式
#define digitalWrite(pin, value) nrf_gpio_pin_write(pin, value)  // 写入数字电平
#define digitalRead(pin) nrf_gpio_pin_read(pin)                   // 读取数字电平
#define delay(ms) nrf_delay_ms(ms)                                // 延时(毫秒)

// GPIO
// GPIO初始化相关函数
void EPD_GPIO_Load(epd_config_t* cfg);  // 加载GPIO配置(从配置结构体)
void EPD_GPIO_Init(void);               // 初始化GPIO引脚
void EPD_GPIO_Uninit(void);             // 反初始化GPIO引脚(恢复默认状态)

// SPI
// SPI通信函数
void EPD_SPI_Write(uint8_t* value, uint8_t len);  // SPI写入数据
void EPD_SPI_Read(uint8_t* value, uint8_t len);   // SPI读取数据

// EPD
// EPD基础操作函数
void EPD_WriteCmd(uint8_t cmd);                        // 写入命令到EPD
void EPD_WriteData(uint8_t* value, uint8_t len);       // 写入数据到EPD
void EPD_ReadData(uint8_t* value, uint8_t len);        // 从EPD读取数据
void EPD_WriteByte(uint8_t value);                     // 写入单字节数据
uint8_t EPD_ReadByte(void);                            // 读取单字节数据
// 宏定义:简化写入命令和数据的操作 - Macro: simplify command and data writing
#define EPD_Write(cmd, ...)                  \
    do {                                     \
        uint8_t _data[] = {__VA_ARGS__};     \
        EPD_WriteCmd(cmd);                   \
        EPD_WriteData(_data, sizeof(_data)); \
    } while (0)
void EPD_FillRAM(uint8_t cmd, uint8_t value, uint32_t len);  // 填充显存(用相同的值填充指定长度)
void EPD_Reset(uint32_t value, uint16_t duration);           // 复位EPD(通过复位引脚)
void EPD_WaitBusy(uint32_t value, uint16_t timeout);         // 等待EPD忙状态结束

// LED Control (active low: LOW = ON, HIGH = OFF)
// LED控制(低电平有效: LOW=点亮, HIGH=熄灭)
/**
 * LED行为说明 - LED Behavior:
 * - 全刷新: LED在刷新期间保持点亮,完成后熄灭 - Full refresh: LED stays ON during refresh, OFF after complete
 * - 部分刷新: LED保持熄灭状态(不闪烁) - Partial refresh: LED stays OFF (no blinking)
 * - BLE连接/断开: LED闪烁一次 - BLE connect/disconnect: LED blinks once
 * - 低电量(<2.8V): LED每2秒闪烁一次 - Low battery (<2.8V): LED blinks every 2 seconds
 */
void EPD_LED_ON(void);    // Turn LED on (for full refresh) - 点亮LED(用于全刷新)
void EPD_LED_OFF(void);   // Turn LED off - 熄灭LED
void EPD_LED_BLINK(void); // Blink LED once (for BLE events, low battery) - LED闪烁一次(用于BLE事件、低电量提示)

// VDD voltage (in millivolts)
// VDD电压(单位:毫伏)
/**
 * 电压读取和低电量检测说明:
 * Voltage Reading and Low Battery Detection:
 *
 * - EPD_ReadVoltage(): 直接读取ADC获取当前电压(单位:mV)
 *   EPD_ReadVoltage(): Directly read ADC to get current voltage (in mV)
 *
 * - EPD_ReadVoltageAndCache(): 读取电压并缓存,用于后续低电量检测
 *   EPD_ReadVoltageAndCache(): Read voltage and cache for subsequent low battery detection
 *
 * - EPD_IsLowBattery(): 使用缓存的电压值判断是否低电量,避免频繁ADC操作
 *   EPD_IsLowBattery(): Use cached voltage to check low battery, avoiding frequent ADC operations
 *
 * 注意: 电压缓存在每次屏幕刷新时更新,因此低电量检测基于最近一次刷新时的电压
 * Note: Voltage cache is updated during each screen refresh, so low battery detection is based on the voltage from the most recent refresh
 */
#define LOW_BATTERY_THRESHOLD_MV 2800      // Low battery voltage threshold (mV) - 低电量阈值(毫伏)
#define CRITICAL_BATTERY_SHUTDOWN_MV 2700  // Critical battery shutdown threshold (mV) - 临界关机阈值(毫伏)
uint16_t EPD_ReadVoltage(void);  // Returns voltage in mV (e.g., 3300 for 3.3V) - 返回电压值(毫伏),例如3300表示3.3V
uint16_t EPD_ReadVoltageAndCache(void);  // Read voltage and cache for low battery detection - 读取电压并缓存用于低电量检测
bool EPD_IsLowBattery(void);  // Uses cached voltage, no ADC operation - 使用缓存电压,无需ADC操作

/**
 * EPD型号初始化函数
 * EPD Model Initialization Function
 *
 * @param id EPD型号ID
 * @return 返回EPD型号结构体指针
 *
 * 重要说明: 此函数会根据ID查找对应的EPD型号并初始化
 * Important: This function finds and initializes the EPD model based on the ID
 */
epd_model_t* epd_init(epd_model_id_t id);

#endif
