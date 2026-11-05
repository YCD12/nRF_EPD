/**
 * EPD驱动程序实现文件
 *
 * 实现电子墨水屏的底层驱动功能,包括GPIO、SPI、电压检测等
 */
#include "EPD_driver.h"

#include "app_error.h"
#include "app_timer.h"
#include "nrf_drv_spi.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_soc.h"
#include "main.h"

#define BUFFER_SIZE 128  // 缓冲区大小(用于填充显存)

// GPIO引脚定义(这些值会从配置文件中加载)
static uint32_t EPD_MOSI_PIN = 5;  // SPI主出从入引脚
static uint32_t EPD_SCLK_PIN = 8;  // SPI时钟引脚
static uint32_t EPD_CS_PIN = 9;  // SPI片选引脚
static uint32_t EPD_DC_PIN = 10;  // 数据/命令选择引脚
static uint32_t EPD_RST_PIN = 11;  // 复位引脚
static uint32_t EPD_BUSY_PIN = 12;  // 忙状态引脚
static uint32_t EPD_BS_PIN = 13;  // 字节选择引脚
static uint32_t EPD_EN_PIN = 0xFF;  // 使能引脚(0xFF表示未使用)
static uint32_t EPD_LED_PIN = 0xFF;  // LED引脚(0xFF表示未使用)

#define SPI_INSTANCE 0                                               /**< SPI 实例索引 */
static const nrf_drv_spi_t spi = NRF_DRV_SPI_INSTANCE(SPI_INSTANCE); /**< SPI 实例 */

// 根据不同的芯片型号定义SPI寄存器访问方式
#define HAL_SPI_INSTANCE spi.u.spi.p_reg

// 类Arduino风格的函数封装
/**
 * 设置GPIO引脚模式
 *
 * @param pin GPIO引脚号
 * @param mode 引脚模式(INPUT/OUTPUT/INPUT_PULLUP/INPUT_PULLDOWN/DEFAULT)
 */
void pinMode(uint32_t pin, uint32_t mode) {
    if (!nrf_gpio_pin_present_check(pin)) return;
    switch (mode) {
        case INPUT:  // 输入模式(无上下拉)
            nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
            break;
        case INPUT_PULLUP:  // 输入上拉模式
            nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLUP);
            break;
        case INPUT_PULLDOWN:  // 输入下拉模式
            nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLDOWN);
            break;
        case OUTPUT:  // 输出模式
            nrf_gpio_cfg_output(pin);
            break;
        case DEFAULT:  // 默认模式(断开连接)
        default:
            nrf_gpio_cfg_default(pin);
            break;
    }
}

// GPIO相关函数
static uint16_t m_driver_refs = 0;  // 驱动引用计数(用于支持多次初始化/反初始化)

/**
 * 加载EPD的GPIO配置
 *
 * @param cfg EPD配置结构体指针
 *
 * 从配置结构体中加载所有GPIO引脚定义
 */
void EPD_GPIO_Load(epd_config_t* cfg) {
    if (cfg == NULL) return;

    EPD_MOSI_PIN = cfg->mosi_pin;
    EPD_SCLK_PIN = cfg->sclk_pin;
    EPD_CS_PIN = cfg->cs_pin;
    EPD_DC_PIN = cfg->dc_pin;
    EPD_RST_PIN = cfg->rst_pin;
    EPD_BUSY_PIN = cfg->busy_pin;
    EPD_BS_PIN = cfg->bs_pin;
    EPD_EN_PIN = cfg->en_pin;
    EPD_LED_PIN = cfg->led_pin;
}

/**
 * 初始化EPD的GPIO和SPI
 *
 * 配置所有必需的GPIO引脚和SPI接口
 *
 * 使用引用计数机制,支持多次调用而不重复初始化
 */
void EPD_GPIO_Init(void) {
    if (m_driver_refs++ > 0) return;  // 如果已初始化则直接返回

    // 配置基本GPIO引脚
    pinMode(EPD_DC_PIN, OUTPUT);  // 数据/命令选择引脚设为输出
    pinMode(EPD_RST_PIN, OUTPUT);  // 复位引脚设为输出
    pinMode(EPD_BUSY_PIN, INPUT);  // 忙状态引脚设为输入

    // 配置SPI接口
    nrf_drv_spi_config_t spi_config = NRF_DRV_SPI_DEFAULT_CONFIG;
    spi_config.sck_pin = EPD_SCLK_PIN;  // SPI时钟引脚
    spi_config.mosi_pin = EPD_MOSI_PIN;  // SPI数据输出引脚
    spi_config.ss_pin = EPD_CS_PIN;  // SPI片选引脚
    APP_ERROR_CHECK(nrf_drv_spi_init(&spi, &spi_config, NULL, NULL));

    // 配置可选的BS引脚
    if (EPD_BS_PIN != 0xFF) {
        pinMode(EPD_BS_PIN, OUTPUT);
        digitalWrite(EPD_BS_PIN, LOW);  // 设置为低电平
    }
    // 配置可选的使能引脚
    if (EPD_EN_PIN != 0xFF) {
        pinMode(EPD_EN_PIN, OUTPUT);
        digitalWrite(EPD_EN_PIN, HIGH);  // 设置为高电平(使能)
        delay(50);  // 等待电压轨稳定
    }

    // 设置初始状态
    digitalWrite(EPD_DC_PIN, LOW);  // DC引脚设为低电平
    digitalWrite(EPD_RST_PIN, HIGH);  // RST引脚设为高电平(未复位)

    // 配置LED引脚(如果存在)
    if (EPD_LED_PIN != 0xFF) {
        // 在配置为输出前先设为高电平(LED熄灭),防止短暂闪烁
        nrf_gpio_pin_set(EPD_LED_PIN);
        nrf_gpio_cfg_output(EPD_LED_PIN);
    }
}

void EPD_GPIO_Ensure(void) /* refs==0 时才 Init */
{
    if (m_driver_refs == 0) {
        EPD_GPIO_Init();
    }
}

/**
 * 反初始化EPD的GPIO和SPI
 *
 * 关闭SPI、复位所有GPIO引脚状态
 *
 * 使用引用计数机制,只有在所有引用都释放后才真正反初始化
 */
void EPD_GPIO_Uninit(void) {
    if (m_driver_refs == 0U) return;
    if (--m_driver_refs > 0U) return;  // 如果还有其他引用则直接返回

    EPD_LED_OFF();  // 关闭LED

    nrf_drv_spi_uninit(&spi);  // 反初始化SPI

    // 将所有控制引脚设为低电平
    digitalWrite(EPD_DC_PIN, LOW);
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_RST_PIN, LOW);
    if (EPD_EN_PIN != 0xFF) digitalWrite(EPD_EN_PIN, LOW);

    // 恢复所有引脚为默认状态(断开连接)
    pinMode(EPD_MOSI_PIN, DEFAULT);
    pinMode(EPD_SCLK_PIN, DEFAULT);
    pinMode(EPD_CS_PIN, DEFAULT);
    pinMode(EPD_DC_PIN, DEFAULT);
    pinMode(EPD_RST_PIN, DEFAULT);
    pinMode(EPD_BUSY_PIN, DEFAULT);
    /* 可选引脚不单独判断 0xFF：pinMode() 内部有 nrf_gpio_pin_present_check()，
     * 对无效 pin 直接返回；C-06 实测加判断后反而 +12 B（pin 是运行期变量）。 */
    pinMode(EPD_BS_PIN, DEFAULT);
    pinMode(EPD_EN_PIN, DEFAULT);
    pinMode(EPD_LED_PIN, DEFAULT);
}

// SPI通信函数
/**
 * SPI写入数据
 *
 * @param value 要写入的数据指针(必须在 RAM 中，EasyDMA 限制)
 * @param len 数据长度
 *
 * 自动检测并配置MOSI引脚为输出模式
 */
void EPD_SPI_Write(uint8_t* value, uint8_t len) {
    // 检查MOSI引脚方向,如果不是输出则重新配置
    nrf_gpio_pin_dir_t dir = nrf_gpio_pin_dir_get(EPD_MOSI_PIN);
    if (dir != NRF_GPIO_PIN_DIR_OUTPUT) {
        pinMode(EPD_MOSI_PIN, OUTPUT);
        // 设置SPI引脚: SCK, MOSI, 禁用MISO
        nrf_spi_pins_set(HAL_SPI_INSTANCE, EPD_SCLK_PIN, EPD_MOSI_PIN, NRF_SPI_PIN_NOT_CONNECTED);
    }

    /* EasyDMA 只能直接访问 RAM：所有传进来的 buffer 必须在 RAM 中，
     * 不得传入 Flash（const 全局/字符串常量等）指针。
     * 现有调用点均为 RAM：EPD_WriteCmd 的 &cmd、EPD_Write 宏的栈数组、
     * EPD_FillRAM 的栈缓冲区、UC8179 的 LUT/栈/GFX 页缓冲、BLE payload。
     * 原先按地址 < 0x20000000 判定 Flash 再 memcpy 到栈缓冲的分支没有
     * 任何调用者，已删除。 */
    APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi, value, len, NULL, 0));
}

/**
 * SPI读取数据
 *
 * @param value 存储读取数据的缓冲区指针
 * @param len 要读取的数据长度
 *
 * 将MOSI引脚切换为输入模式作为MISO使用(半双工SPI)
 */
void EPD_SPI_Read(uint8_t* value, uint8_t len) {
    // 检查MOSI引脚方向,如果不是输入则重新配置
    nrf_gpio_pin_dir_t dir = nrf_gpio_pin_dir_get(EPD_MOSI_PIN);
    if (dir != NRF_GPIO_PIN_DIR_INPUT) {
        pinMode(EPD_MOSI_PIN, INPUT);
        // 设置SPI引脚: SCK, 禁用MOSI, MOSI作为MISO使用
        nrf_spi_pins_set(HAL_SPI_INSTANCE, EPD_SCLK_PIN, NRF_SPI_PIN_NOT_CONNECTED, EPD_MOSI_PIN);
    }
    APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi, NULL, 0, value, len));
}

// EPD基础操作函数
/**
 * 写入命令到EPD
 *
 * @param cmd 命令字节
 *
 * DC引脚拉低表示传输的是命令
 */
void EPD_WriteCmd(uint8_t cmd) {
    digitalWrite(EPD_DC_PIN, LOW);  // DC=LOW: 命令模式
    EPD_SPI_Write(&cmd, 1);
}

/**
 * 写入数据到EPD
 *
 * @param value 数据指针
 * @param len 数据长度
 *
 * DC引脚拉高表示传输的是数据
 */
void EPD_WriteData(uint8_t* value, uint8_t len) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式
    EPD_SPI_Write(value, len);
}

/**
 * 从EPD读取数据
 *
 * @param value 存储读取数据的缓冲区
 * @param len 要读取的数据长度
 */
void EPD_ReadData(uint8_t* value, uint8_t len) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式
    EPD_SPI_Read(value, len);
}

/**
 * 写入单字节数据到EPD
 *
 * @param value 要写入的字节
 */
void EPD_WriteByte(uint8_t value) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式
    EPD_SPI_Write(&value, 1);
}

/**
 * 从EPD读取单字节数据
 *
 * @return 读取到的字节
 */
uint8_t EPD_ReadByte(void) {
    uint8_t value;
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式
    EPD_SPI_Read(&value, 1);
    return value;
}

/**
 * 填充EPD显存
 *
 * @param cmd 要发送的命令
 * @param value 填充值
 * @param len 填充长度
 *
 * 使用缓冲区分块填充,提高效率
 */
void EPD_FillRAM(uint8_t cmd, uint8_t value, uint32_t len) {
    uint8_t buffer[BUFFER_SIZE];
    // 预填充缓冲区
    for (uint8_t i = 0; i < BUFFER_SIZE; i++) buffer[i] = value;

    EPD_WriteCmd(cmd);  // 发送命令
    uint16_t remaining = len;
    // 分块发送数据
    while (remaining > 0) {
        uint16_t chunk_size = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        EPD_WriteData(buffer, chunk_size);
        remaining -= chunk_size;
    }
}

/**
 * 复位EPD
 *
 * @param value 复位引脚的初始电平
 * @param duration 每个状态持续时间(毫秒)
 *
 * 执行标准的复位时序: 初始电平 -> 反转 -> 初始电平
 */
void EPD_Reset(uint32_t value, uint16_t duration) {
    digitalWrite(EPD_RST_PIN, value);
    delay(duration);
    digitalWrite(EPD_RST_PIN, (value == LOW) ? HIGH : LOW);  // 反转电平
    delay(duration);
    digitalWrite(EPD_RST_PIN, value);
    delay(duration);
}

/**
 * 等待EPD忙状态结束（低功耗版，P4）
 *
 * @param value 忙状态时 BUSY 引脚的电平（本板 BUSY 低电平=忙）
 * @param timeout 超时时间(毫秒)
 *
 * 刷新波形期间 UC8179 可能持续数秒。循环内 delay(1) 每 1ms 轮询一次，
 * 并每 500ms 喂狗一次。
 */
static volatile bool m_busy_abort;

void EPD_RequestBusyAbort(void) /* 传图打断 WaitBusy */
{
    m_busy_abort = true;
}

void EPD_ClearBusyAbort(void)
{
    m_busy_abort = false;
}

bool EPD_BusyAbortRequested(void)
{
    return m_busy_abort;
}

/* 面板驱动 BUSY 超时标志（全局，历史语义）。
 * 具体驱动（如 UC8179）在 WaitBusy 超时时置位；服务层在初始化/刷新前后
 * 清理并查询。面板实例尚未创建（睡眠后）时同样可读写，避免服务层依赖
 * 具体驱动头文件；驱动的 busy_error 虚表项也读取同一标志，语义等价。 */
static volatile bool m_panel_busy_error;

void EPD_PanelBusyErrorSet(void)
{
    m_panel_busy_error = true;
}

void EPD_PanelBusyErrorClear(void)
{
    m_panel_busy_error = false;
}

bool EPD_PanelBusyError(void)
{
    return m_panel_busy_error;
}

bool EPD_WaitBusy(uint32_t value, uint16_t timeout) {
    /* Multiply before dividing: APP_TIMER_CLOCK_FREQ/(prescaler+1) is 16384
     * ticks/s, so 16 ticks/ms would truncate 16.384 and shorten every timeout
     * by 2.34 % (1000 ms -> 976.6 ms, 30 s -> 29.3 s). The 32-bit product is
     * safe for every uint16_t timeout because 65535 * 16384 < 2^32. */
    const uint32_t ticks_per_second = APP_TIMER_CLOCK_FREQ / (APP_TIMER_CONFIG_RTC_FREQUENCY + 1);

    NRF_LOG_DEBUG("[EPD]: check busy\n");
    if (m_busy_abort) {
        return false;
    }
    if (digitalRead(EPD_BUSY_PIN) != value) {
        NRF_LOG_DEBUG("[EPD]: busy release\n");
        return true;
    }

    uint32_t start_ticks    = app_timer_cnt_get();
    uint32_t last_wdt_ticks = start_ticks;
    uint32_t timeout_ticks  = ((uint32_t)timeout * ticks_per_second) / 1000UL;
    const uint32_t wdt_feed_ticks = (500UL * ticks_per_second) / 1000UL;

    while (digitalRead(EPD_BUSY_PIN) == value) {
        if (m_busy_abort) {
            NRF_LOG_INFO("[EPD]: busy wait aborted for picture transfer\n");
            return false;
        }
        delay(1);

        uint32_t now_ticks = app_timer_cnt_get();
        if (app_timer_cnt_diff_compute(now_ticks, start_ticks) >= timeout_ticks) {
            if (digitalRead(EPD_BUSY_PIN) != value) {
                break;  // BUSY 在最后一次 delay 期间释放，非真超时
            }
            NRF_LOG_WARNING("[EPD]: busy timeout!\n");
            return false;
        }
        if (app_timer_cnt_diff_compute(now_ticks, last_wdt_ticks) >= wdt_feed_ticks) {
            app_feed_wdt();
            last_wdt_ticks = now_ticks;
        }
    }

    NRF_LOG_DEBUG("[EPD]: busy release\n");
    return true;
}

/*
 * LED控制(低电平有效: LOW=点亮, HIGH=熄灭)
 *
 * LED行为说明:
 * 屏幕刷新、低电量和唤醒期间LED保持熄灭
 * BLE连接/断开: LED闪烁一次
 */

/**
 * 点亮LED
 *
 * 保留给显式/手动LED控制使用。正常刷新路径保持LED熄灭。
 */
void EPD_LED_ON(void) {
    if (EPD_LED_PIN != 0xFF) digitalWrite(EPD_LED_PIN, LOW);  // LOW=点亮 - LOW=ON
}

/**
 * 熄灭LED
 *
 * 刷新路径防御性调用此函数以保持LED熄灭。
 */
void EPD_LED_OFF(void) {
    if (EPD_LED_PIN != 0xFF) digitalWrite(EPD_LED_PIN, HIGH);  // HIGH=熄灭 - HIGH=OFF
}

/**
 * LED闪烁一次
 *
 * 用于BLE连接/断开，以及低电量保持时每 30 秒闪一次。
 *
 * 闪烁时序: 点亮100ms -> 熄灭100ms -> 恢复默认状态
 */
void EPD_LED_BLINK(void) {
    if (EPD_LED_PIN != 0xFF) {
        pinMode(EPD_LED_PIN, OUTPUT);  // 配置为输出
        digitalWrite(EPD_LED_PIN, LOW);  // 点亮
        delay(100);
        digitalWrite(EPD_LED_PIN, HIGH);  // 熄灭
        delay(100);
        pinMode(EPD_LED_PIN, DEFAULT);  // 恢复默认状态
    }
}

/**
 * 读取VDD电压
 *
 * @return 电压值(单位:毫伏)
 *
 * 使用ADC测量VDD电压,根据不同芯片型号使用不同的ADC外设
 *
 * S112芯片: 使用SAADC(逐次逼近型ADC)
 * - 分辨率: 10位
 * - 增益: 1/6
 * - 参考电压: 内部0.6V
 *
 * 其他芯片: 使用传统ADC
 * - 分辨率: 10位
 * - 输入: VDD的1/3分压
 * - 参考电压: VBG(1.2V)
 */
/* Also used before app_timer is initialized at boot. Bound the wait with a
 * microsecond delay rather than depending on an RTC that may not be running. */
static bool epd_adc_wait(volatile uint32_t *event) {
    for (uint32_t remaining = 5000U; remaining != 0U; --remaining) {
        if (*event) return true;
        nrf_delay_us(1);
    }
    return *event != 0U;
}

uint16_t EPD_ReadVoltage(void) {
    /* SAADC and its EasyDMA buffer belong exclusively to main context. */
    if (__get_IPSR() != 0U) return 0U;
    // 使用SAADC
    volatile int16_t value = 0;
    bool sampled = false;
    // 设置10位分辨率
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
    // 使能SAADC
    NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos);
    // 配置通道0
    NRF_SAADC->CH[0].CONFIG =
        ((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) & SAADC_CH_CONFIG_RESP_Msk) |  // 正端旁路
        ((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESN_Pos) & SAADC_CH_CONFIG_RESN_Msk) |  // 负端旁路
        ((SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) & SAADC_CH_CONFIG_GAIN_Msk) |  // 增益1/6
        ((SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) & SAADC_CH_CONFIG_REFSEL_Msk) |  // 内部参考电压
        ((SAADC_CH_CONFIG_TACQ_3us << SAADC_CH_CONFIG_TACQ_Pos) & SAADC_CH_CONFIG_TACQ_Msk) |  // 采集时间3us
        ((SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) & SAADC_CH_CONFIG_MODE_Msk);  // 单端模式
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;  // 负端未连接
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDD;  // 正端连接VDD
    NRF_SAADC->RESULT.PTR = (uint32_t)&value;  // 结果存储地址
    NRF_SAADC->RESULT.MAXCNT = 1;  // 最大采样数
    NRF_SAADC->EVENTS_STARTED = 0U;
    NRF_SAADC->EVENTS_END = 0U;
    NRF_SAADC->EVENTS_STOPPED = 0U;
    NRF_SAADC->TASKS_START = 0x01UL;  // 启动ADC
    if (epd_adc_wait(&NRF_SAADC->EVENTS_STARTED)) {
        NRF_SAADC->TASKS_SAMPLE = 0x01UL;  // 开始采样
        sampled = epd_adc_wait(&NRF_SAADC->EVENTS_END);
    }
    NRF_SAADC->TASKS_STOP = 0x01UL;  // 停止ADC
    bool stopped = epd_adc_wait(&NRF_SAADC->EVENTS_STOPPED);
    NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos);  // 禁用SAADC
    NRF_SAADC->EVENTS_STARTED = 0U;
    NRF_SAADC->EVENTS_END = 0U;
    NRF_SAADC->EVENTS_STOPPED = 0U;
    if (!sampled || !stopped || value <= 0) return 0U;
    NRF_LOG_DEBUG("ADC value: %d\n", value);
    // 返回电压值(单位:毫伏)
    // 原公式: (value * 3.6V) / 1024 返回伏特
    // 新公式: (value * 3600mV) / 1024 返回毫伏
    return (uint16_t)((value * 3600) / 1024);
}

// 缓存的电压值(单位:毫伏,在屏幕刷新时更新)
static uint16_t m_cached_voltage = 3300;  // 默认为正常电压(3.3V = 3300mV)

/**
 * 读取电压并缓存
 *
 * @return 电压值(单位:毫伏)
 *
 * 该函数在每次屏幕刷新时被调用,更新缓存的电压值
 *
 * 缓存电压值的好处:
 * 1. 避免频繁的ADC操作,减少功耗
 * 2. 低电量检测可以快速判断,无需等待ADC转换
 * 3. 电压值基于最近一次屏幕刷新,具有时效性
 */
uint16_t EPD_ReadVoltageAndCache(void) {
    uint16_t voltage = EPD_ReadVoltage();
    if (voltage != 0U) m_cached_voltage = voltage;
    return voltage;
}
