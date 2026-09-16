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

// EPD 驱动芯片类型
typedef enum {
    EPD_DRIVER_IC_UC8179 = 0x12,  // UC8179
} epd_driver_ic_t;

// UC81xx 系列命令
enum {
    UC81xx_PSR = 0x00,    // 面板设置
    UC81xx_PWR = 0x01,    // 电源设置
    UC81xx_POF = 0x02,    // 电源关闭
    UC81xx_PFS = 0x03,    // 电源关闭序列
    UC81xx_PON = 0x04,    // 电源开启
    UC81xx_PMES = 0x05,    // 电源开启测量
    UC81xx_BTST = 0x06,    // 升压软启动
    UC81xx_DSLP = 0x07,    // 深度睡眠
    UC81xx_DTM1 = 0x10,    // KW：OLD 数据；KWR：黑白数据
    UC81xx_DSP = 0x11,    // 数据停止
    UC81xx_DRF = 0x12,    // 显示刷新
    UC81xx_DTM2 = 0x13,    // KW：NEW 数据；KWR：红色数据
    UC81xx_DUSPI = 0x15,  // 双 SPI 模式
    UC81xx_LUTC = 0x20,   // VCOM LUT (LUTC)
    UC81xx_LUTWW = 0x21,  // 白到白 LUT (LUTWW)
    UC81xx_LUTBW = 0x22,  // 黑到白 LUT (LUTBW / LUTR)
    UC81xx_LUTWB = 0x23,  // 白到黑 LUT (LUTWB / LUTW)
    UC81xx_LUTBB = 0x24,  // 黑到黑 LUT (LUTBB / LUTB)
    UC81xx_PLL = 0x30,    // PLL 控制
    UC81xx_TSC = 0x40,    // 温度传感器校准
    UC81xx_TSE = 0x41,    // 温度传感器选择
    UC81xx_TSW = 0x42,    // 温度传感器写入
    UC81xx_TSR = 0x43,    // 温度传感器读取
    UC81xx_CDI = 0x50,    // Vcom 和数据间隔
    UC81xx_LPD = 0x51,    // 低电量检测
    UC81xx_TCON = 0x60,   // TCON 时序
    UC81xx_TRES = 0x61,   // 分辨率
    UC81xx_GSST = 0x65,   // 门驱动源设置
    UC81xx_REV = 0x70,    // 版本号
    UC81xx_FLG = 0x71,    // 获取状态
    UC81xx_AMV = 0x80,    // 自动测量 Vcom
    UC81xx_VV = 0x81,    // 读取 Vcom
    UC81xx_VDCS = 0x82,   // VCM 直流设置
    UC81xx_PTL = 0x90,    // 局刷窗口
    UC81xx_PTIN = 0x91,   // 进入局刷
    UC81xx_PTOUT = 0x92,  // 退出局刷
    UC81xx_PGM = 0xA0,    // 编程模式
    UC81xx_APG = 0xA1,    // 激活编程
    UC81xx_ROTP = 0xA2,   // 读取 OTP
    UC81xx_CCSET = 0xE0,  // 级联设置
    UC81xx_PWS = 0xE3,    // 省电
    UC81xx_TSSET = 0xE5,  // 强制温度
};

// 颜色类型
typedef enum {
    EPD_COLOR_BW = 1,    // 黑白
    EPD_COLOR_BWR = 2,   // 黑白红
    EPD_COLOR_BWRY = 3,  // 黑白红黄
} epd_color_t;

// 不要修改现有 ID，用于识别 EPD 型号
typedef enum {
    EPD_UC8179_750_BWR = 7,  // 7.5 寸黑白红（UC8179）
} epd_model_id_t;

struct epd_driver;

// EPD 型号
typedef struct {
    epd_model_id_t id;      // 型号 ID
    epd_color_t color;      // 颜色类型
    const struct epd_driver* drv; // 驱动
    uint16_t width;         // 宽度（像素）
    uint16_t height;        // 高度（像素）
} epd_model_t;

/**@brief EPD 驱动函数表 */
typedef struct epd_driver {
    epd_driver_ic_t ic;                            /**< 驱动芯片类型 */
    void (*init)(const epd_model_t * epd);                /**< 初始化寄存器 */
    void (*clear)(const epd_model_t * epd, bool refresh); /**< 清屏；refresh=true 立即刷新 */
    void (*write_image)(const epd_model_t * epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h);                                              /**< 写入图像到显存 */
    void (*write_ram)(const epd_model_t * epd, uint8_t cfg, uint8_t* data, uint8_t len); /**< 直接写入显存 */
    void (*refresh)(const epd_model_t * epd);     /**< 将显存刷新到屏幕 */
    void (*sleep)(const epd_model_t * epd);       /**< 进入睡眠以降功耗 */
    int8_t (*read_temp)(const epd_model_t * epd); /**< 从驱动芯片读温度 */
    /* 可选：真 OLD/NEW 局刷路径 */
    void (*init_partial)(const epd_model_t *epd);
    bool (*refresh_partial)(const epd_model_t *epd, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h);
    /* 在同一 PTIN/PTL 窗口写入打包的 OLD 与 NEW 位图 */
    bool (*diff_push)(const epd_model_t *epd, uint16_t x, uint16_t y,
                      uint16_t w, uint16_t h,
                      const uint8_t *old_bm, const uint8_t *new_bm);
} epd_driver_t;

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
uint16_t EPD_ReadVoltage(void);            // 例如 3300 表示 3.3V
uint16_t EPD_ReadVoltageAndCache(void);    // 读取并缓存

/**
 * 按 ID 查找并初始化 EPD 型号
 *
 * @param id 型号 ID
 * @return 型号结构体指针
 */
const epd_model_t * epd_get_model(epd_model_id_t id);
const epd_model_t * epd_init(epd_model_id_t id); /* 查找型号并调用 drv->init */

#endif
