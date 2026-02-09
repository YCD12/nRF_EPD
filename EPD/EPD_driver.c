/**
 * EPD驱动程序实现文件
 * EPD Driver Implementation File
 *
 * 实现电子墨水屏的底层驱动功能,包括GPIO、SPI、电压检测等
 * Implements low-level driver functions for E-Paper Display, including GPIO, SPI, voltage detection, etc.
 */
#include "EPD_driver.h"

#include "app_error.h"
#include "nrf_drv_spi.h"
#include "nrf_log.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))  // 计算数组元素个数 - Calculate array size
#define BUFFER_SIZE 128  // 缓冲区大小(用于填充显存) - Buffer size for RAM filling

// GPIO Pins
// GPIO引脚定义(这些值会从配置文件中加载)
static uint32_t EPD_MOSI_PIN = 5;   // SPI主出从入引脚 - SPI Master Out Slave In
static uint32_t EPD_SCLK_PIN = 8;   // SPI时钟引脚 - SPI Clock
static uint32_t EPD_CS_PIN = 9;     // SPI片选引脚 - SPI Chip Select
static uint32_t EPD_DC_PIN = 10;    // 数据/命令选择引脚 - Data/Command selection
static uint32_t EPD_RST_PIN = 11;   // 复位引脚 - Reset pin
static uint32_t EPD_BUSY_PIN = 12;  // 忙状态引脚 - Busy status pin
static uint32_t EPD_BS_PIN = 13;    // 字节选择引脚 - Byte Select pin
static uint32_t EPD_EN_PIN = 0xFF;  // 使能引脚(0xFF表示未使用) - Enable pin (0xFF = not used)
static uint32_t EPD_LED_PIN = 0xFF; // LED引脚(0xFF表示未使用) - LED pin (0xFF = not used)

#define SPI_INSTANCE 0                                               /**< SPI instance index. */ /**< SPI实例索引 */
static const nrf_drv_spi_t spi = NRF_DRV_SPI_INSTANCE(SPI_INSTANCE); /**< SPI instance. */ /**< SPI实例 */

// 根据不同的芯片型号定义SPI寄存器访问方式
// Define SPI register access method based on different chip models
#define HAL_SPI_INSTANCE spi.u.spi.p_reg

// Arduino like function wrappers
// 类Arduino风格的函数封装
/**
 * 设置GPIO引脚模式
 * Set GPIO pin mode
 *
 * @param pin GPIO引脚号 - GPIO pin number
 * @param mode 引脚模式(INPUT/OUTPUT/INPUT_PULLUP/INPUT_PULLDOWN/DEFAULT)
 *             Pin mode (INPUT/OUTPUT/INPUT_PULLUP/INPUT_PULLDOWN/DEFAULT)
 */
void pinMode(uint32_t pin, uint32_t mode) {
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

// GPIO
// GPIO相关函数
static uint16_t m_driver_refs = 0;  // 驱动引用计数(用于支持多次初始化/反初始化) - Driver reference count

/**
 * 加载EPD的GPIO配置
 * Load EPD GPIO configuration
 *
 * @param cfg EPD配置结构体指针 - Pointer to EPD configuration structure
 *
 * 从配置结构体中加载所有GPIO引脚定义
 * Load all GPIO pin definitions from configuration structure
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
 * Initialize EPD GPIO and SPI
 *
 * 配置所有必需的GPIO引脚和SPI接口
 * Configure all required GPIO pins and SPI interface
 *
 * 使用引用计数机制,支持多次调用而不重复初始化
 * Uses reference counting to support multiple calls without re-initialization
 */
void EPD_GPIO_Init(void) {
    if (m_driver_refs++ > 0) return;  // 如果已初始化则直接返回 - Return if already initialized

    // 配置基本GPIO引脚 - Configure basic GPIO pins
    pinMode(EPD_DC_PIN, OUTPUT);    // 数据/命令选择引脚设为输出
    pinMode(EPD_RST_PIN, OUTPUT);   // 复位引脚设为输出
    pinMode(EPD_BUSY_PIN, INPUT);   // 忙状态引脚设为输入

    // 配置SPI接口 - Configure SPI interface
    nrf_drv_spi_config_t spi_config = NRF_DRV_SPI_DEFAULT_CONFIG;
    spi_config.sck_pin = EPD_SCLK_PIN;   // SPI时钟引脚
    spi_config.mosi_pin = EPD_MOSI_PIN;  // SPI数据输出引脚
    spi_config.ss_pin = EPD_CS_PIN;      // SPI片选引脚
    APP_ERROR_CHECK(nrf_drv_spi_init(&spi, &spi_config, NULL, NULL));

    // 配置可选的BS引脚 - Configure optional BS pin
    if (EPD_BS_PIN != 0xFF) {
        pinMode(EPD_BS_PIN, OUTPUT);
        digitalWrite(EPD_BS_PIN, LOW);  // 设置为低电平
    }
    // 配置可选的使能引脚 - Configure optional enable pin
    if (EPD_EN_PIN != 0xFF) {
        pinMode(EPD_EN_PIN, OUTPUT);
        digitalWrite(EPD_EN_PIN, HIGH);  // 设置为高电平(使能)
    }

    // 设置初始状态 - Set initial state
    digitalWrite(EPD_DC_PIN, LOW);   // DC引脚设为低电平
    digitalWrite(EPD_RST_PIN, HIGH); // RST引脚设为高电平(未复位)

    // 配置LED引脚(如果存在) - Configure LED pin if present
    if (EPD_LED_PIN != 0xFF) {
        // Set pin HIGH (LED OFF) before configuring as output to prevent brief flash
        // 在配置为输出前先设为高电平(LED熄灭),防止短暂闪烁
        nrf_gpio_pin_set(EPD_LED_PIN);
        nrf_gpio_cfg_output(EPD_LED_PIN);
    }
}

/**
 * 反初始化EPD的GPIO和SPI
 * Uninitialize EPD GPIO and SPI
 *
 * 关闭SPI、复位所有GPIO引脚状态
 * Shutdown SPI and reset all GPIO pins state
 *
 * 使用引用计数机制,只有在所有引用都释放后才真正反初始化
 * Uses reference counting, only uninitializes when all references are released
 */
void EPD_GPIO_Uninit(void) {
    if (--m_driver_refs > 0) return;  // 如果还有其他引用则直接返回 - Return if other references exist

    EPD_LED_OFF();  // 关闭LED - Turn off LED

    nrf_drv_spi_uninit(&spi);  // 反初始化SPI - Uninitialize SPI

    // 将所有控制引脚设为低电平 - Set all control pins to LOW
    digitalWrite(EPD_DC_PIN, LOW);
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_RST_PIN, LOW);
    if (EPD_EN_PIN != 0xFF) digitalWrite(EPD_EN_PIN, LOW);

    // reset pin state
    // 恢复所有引脚为默认状态(断开连接)
    pinMode(EPD_MOSI_PIN, DEFAULT);
    pinMode(EPD_SCLK_PIN, DEFAULT);
    pinMode(EPD_CS_PIN, DEFAULT);
    pinMode(EPD_DC_PIN, DEFAULT);
    pinMode(EPD_RST_PIN, DEFAULT);
    pinMode(EPD_BUSY_PIN, DEFAULT);
    pinMode(EPD_BS_PIN, DEFAULT);
    pinMode(EPD_EN_PIN, DEFAULT);
    pinMode(EPD_LED_PIN, DEFAULT);
}

// SPI
// SPI通信函数
/**
 * SPI写入数据
 * SPI Write Data
 *
 * @param value 要写入的数据指针 - Pointer to data to write
 * @param len 数据长度 - Data length
 *
 * 自动检测并配置MOSI引脚为输出模式
 * Automatically detects and configures MOSI pin as output mode
 */
void EPD_SPI_Write(uint8_t* value, uint8_t len) {
    // 检查MOSI引脚方向,如果不是输出则重新配置
    // Check MOSI pin direction, reconfigure if not output
    nrf_gpio_pin_dir_t dir = nrf_gpio_pin_dir_get(EPD_MOSI_PIN);
    if (dir != NRF_GPIO_PIN_DIR_OUTPUT) {
        pinMode(EPD_MOSI_PIN, OUTPUT);
        // 设置SPI引脚: SCK, MOSI, 禁用MISO
        nrf_spi_pins_set(HAL_SPI_INSTANCE, EPD_SCLK_PIN, EPD_MOSI_PIN, NRF_SPI_PIN_NOT_CONNECTED);
    }
    APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi, value, len, NULL, 0));
}

/**
 * SPI读取数据
 * SPI Read Data
 *
 * @param value 存储读取数据的缓冲区指针 - Pointer to buffer for read data
 * @param len 要读取的数据长度 - Length of data to read
 *
 * 将MOSI引脚切换为输入模式作为MISO使用(半双工SPI)
 * Switches MOSI pin to input mode to use as MISO (half-duplex SPI)
 */
void EPD_SPI_Read(uint8_t* value, uint8_t len) {
    // 检查MOSI引脚方向,如果不是输入则重新配置
    // Check MOSI pin direction, reconfigure if not input
    nrf_gpio_pin_dir_t dir = nrf_gpio_pin_dir_get(EPD_MOSI_PIN);
    if (dir != NRF_GPIO_PIN_DIR_INPUT) {
        pinMode(EPD_MOSI_PIN, INPUT);
        // 设置SPI引脚: SCK, 禁用MOSI, MOSI作为MISO使用
        nrf_spi_pins_set(HAL_SPI_INSTANCE, EPD_SCLK_PIN, NRF_SPI_PIN_NOT_CONNECTED, EPD_MOSI_PIN);
    }
    APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi, NULL, 0, value, len));
}

// EPD
// EPD基础操作函数
/**
 * 写入命令到EPD
 * Write command to EPD
 *
 * @param cmd 命令字节 - Command byte
 *
 * DC引脚拉低表示传输的是命令
 * DC pin LOW indicates command transmission
 */
void EPD_WriteCmd(uint8_t cmd) {
    digitalWrite(EPD_DC_PIN, LOW);  // DC=LOW: 命令模式 - Command mode
    EPD_SPI_Write(&cmd, 1);
}

/**
 * 写入数据到EPD
 * Write data to EPD
 *
 * @param value 数据指针 - Pointer to data
 * @param len 数据长度 - Data length
 *
 * DC引脚拉高表示传输的是数据
 * DC pin HIGH indicates data transmission
 */
void EPD_WriteData(uint8_t* value, uint8_t len) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式 - Data mode
    EPD_SPI_Write(value, len);
}

/**
 * 从EPD读取数据
 * Read data from EPD
 *
 * @param value 存储读取数据的缓冲区 - Buffer to store read data
 * @param len 要读取的数据长度 - Length of data to read
 */
void EPD_ReadData(uint8_t* value, uint8_t len) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式 - Data mode
    EPD_SPI_Read(value, len);
}

/**
 * 写入单字节数据到EPD
 * Write single byte data to EPD
 *
 * @param value 要写入的字节 - Byte to write
 */
void EPD_WriteByte(uint8_t value) {
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式 - Data mode
    EPD_SPI_Write(&value, 1);
}

/**
 * 从EPD读取单字节数据
 * Read single byte data from EPD
 *
 * @return 读取到的字节 - Read byte
 */
uint8_t EPD_ReadByte(void) {
    uint8_t value;
    digitalWrite(EPD_DC_PIN, HIGH);  // DC=HIGH: 数据模式 - Data mode
    EPD_SPI_Read(&value, 1);
    return value;
}

/**
 * 填充EPD显存
 * Fill EPD RAM
 *
 * @param cmd 要发送的命令 - Command to send
 * @param value 填充值 - Fill value
 * @param len 填充长度 - Fill length
 *
 * 使用缓冲区分块填充,提高效率
 * Uses buffer chunking for efficient filling
 */
void EPD_FillRAM(uint8_t cmd, uint8_t value, uint32_t len) {
    uint8_t buffer[BUFFER_SIZE];
    // 预填充缓冲区 - Pre-fill buffer
    for (uint8_t i = 0; i < BUFFER_SIZE; i++) buffer[i] = value;

    EPD_WriteCmd(cmd);  // 发送命令 - Send command
    uint16_t remaining = len;
    // 分块发送数据 - Send data in chunks
    while (remaining > 0) {
        uint16_t chunk_size = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        EPD_WriteData(buffer, chunk_size);
        remaining -= chunk_size;
    }
}

/**
 * 复位EPD
 * Reset EPD
 *
 * @param value 复位引脚的初始电平 - Initial level of reset pin
 * @param duration 每个状态持续时间(毫秒) - Duration of each state in milliseconds
 *
 * 执行标准的复位时序: 初始电平 -> 反转 -> 初始电平
 * Performs standard reset sequence: initial level -> inverted -> initial level
 */
void EPD_Reset(uint32_t value, uint16_t duration) {
    digitalWrite(EPD_RST_PIN, value);
    delay(duration);
    digitalWrite(EPD_RST_PIN, (value == LOW) ? HIGH : LOW);  // 反转电平 - Invert level
    delay(duration);
    digitalWrite(EPD_RST_PIN, value);
    delay(duration);
}

/**
 * 等待EPD忙状态结束
 * Wait for EPD busy state to end
 *
 * @param value 忙状态时BUSY引脚的电平 - BUSY pin level during busy state
 * @param timeout 超时时间(毫秒) - Timeout in milliseconds
 *
 * 轮询BUSY引脚直到状态改变或超时
 * Polls BUSY pin until state changes or timeout occurs
 */
void EPD_WaitBusy(uint32_t value, uint16_t timeout) {
    NRF_LOG_DEBUG("[EPD]: check busy\n");
    while (digitalRead(EPD_BUSY_PIN) == value) {
        delay(1);
        timeout--;
        if (timeout == 0) {
            NRF_LOG_DEBUG("[EPD]: busy timeout!\n");  // 超时警告 - Timeout warning
            break;
        }
    }
    NRF_LOG_DEBUG("[EPD]: busy release\n");  // 忙状态释放 - Busy released
}

/*
 * LED Control (active low: LOW = ON, HIGH = OFF)
 * LED控制(低电平有效: LOW=点亮, HIGH=熄灭)
 *
 * LED behavior:
 * LED行为说明:
 * - Full refresh: LED stays ON during refresh, OFF after complete
 *   全刷新: LED在刷新过程中保持点亮,完成后熄灭
 * - Partial refresh: LED stays OFF (no blinking)
 *   部分刷新: LED保持熄灭状态(不闪烁)
 * - BLE connect/disconnect: LED blinks once
 *   BLE连接/断开: LED闪烁一次
 * - Low battery (<2.8V): LED blinks every 2 seconds
 *   低电量(<2.8V): LED每2秒闪烁一次
 */

/**
 * 点亮LED
 * Turn LED on
 *
 * 主要用于全刷新期间指示屏幕正在更新
 * Mainly used during full refresh to indicate screen is updating
 */
void EPD_LED_ON(void) {
    if (EPD_LED_PIN != 0xFF) digitalWrite(EPD_LED_PIN, LOW);  // LOW=点亮 - LOW=ON
}

/**
 * 熄灭LED
 * Turn LED off
 *
 * 部分刷新期间LED保持熄灭状态
 * LED stays off during partial refresh
 */
void EPD_LED_OFF(void) {
    if (EPD_LED_PIN != 0xFF) digitalWrite(EPD_LED_PIN, HIGH);  // HIGH=熄灭 - HIGH=OFF
}

/**
 * LED闪烁一次
 * Blink LED once
 *
 * 用于BLE事件和低电量提示
 * Used for BLE events and low battery indication
 *
 * 闪烁时序: 点亮100ms -> 熄灭100ms -> 恢复默认状态
 * Blink sequence: ON 100ms -> OFF 100ms -> restore default state
 */
void EPD_LED_BLINK(void) {
    if (EPD_LED_PIN != 0xFF) {
        pinMode(EPD_LED_PIN, OUTPUT);        // 配置为输出 - Configure as output
        digitalWrite(EPD_LED_PIN, LOW);      // 点亮 - Turn on
        delay(100);
        digitalWrite(EPD_LED_PIN, HIGH);     // 熄灭 - Turn off
        delay(100);
        pinMode(EPD_LED_PIN, DEFAULT);       // 恢复默认状态 - Restore default state
    }
}

/**
 * 读取VDD电压
 * Read VDD voltage
 *
 * @return 电压值(单位:毫伏) - Voltage in millivolts (e.g., 3300 for 3.3V)
 *
 * 使用ADC测量VDD电压,根据不同芯片型号使用不同的ADC外设
 * Uses ADC to measure VDD voltage, different ADC peripherals for different chip models
 *
 * S112芯片: 使用SAADC(逐次逼近型ADC)
 * S112 chip: Uses SAADC (Successive Approximation ADC)
 * - 分辨率: 10位 - Resolution: 10-bit
 * - 增益: 1/6 - Gain: 1/6
 * - 参考电压: 内部0.6V - Reference: Internal 0.6V
 *
 * 其他芯片: 使用传统ADC
 * Other chips: Uses legacy ADC
 * - 分辨率: 10位 - Resolution: 10-bit
 * - 输入: VDD的1/3分压 - Input: VDD with 1/3 prescaling
 * - 参考电压: VBG(1.2V) - Reference: VBG (1.2V)
 */
uint16_t EPD_ReadVoltage(void) {
    // 使用SAADC - Uses SAADC
    volatile int16_t value = 0;
    // 设置10位分辨率 - Set 10-bit resolution
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
    // 使能SAADC - Enable SAADC
    NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos);
    // 配置通道0 - Configure channel 0
    NRF_SAADC->CH[0].CONFIG =
        ((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) & SAADC_CH_CONFIG_RESP_Msk) |      // 正端旁路 - Positive bypass
        ((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESN_Pos) & SAADC_CH_CONFIG_RESN_Msk) |      // 负端旁路 - Negative bypass
        ((SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) & SAADC_CH_CONFIG_GAIN_Msk) |     // 增益1/6 - Gain 1/6
        ((SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) & SAADC_CH_CONFIG_REFSEL_Msk) | // 内部参考电压 - Internal reference
        ((SAADC_CH_CONFIG_TACQ_3us << SAADC_CH_CONFIG_TACQ_Pos) & SAADC_CH_CONFIG_TACQ_Msk) |         // 采集时间3us - Acquisition time 3us
        ((SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) & SAADC_CH_CONFIG_MODE_Msk);           // 单端模式 - Single-ended mode
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;        // 负端未连接 - Negative not connected
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDD;       // 正端连接VDD - Positive connected to VDD
    NRF_SAADC->RESULT.PTR = (uint32_t)&value;                // 结果存储地址 - Result storage address
    NRF_SAADC->RESULT.MAXCNT = 1;                            // 最大采样数 - Max sample count
    NRF_SAADC->TASKS_START = 0x01UL;                         // 启动ADC - Start ADC
    while (!NRF_SAADC->EVENTS_STARTED);                      // 等待启动完成 - Wait for start
    NRF_SAADC->EVENTS_STARTED = 0x00UL;
    NRF_SAADC->TASKS_SAMPLE = 0x01UL;                        // 开始采样 - Start sampling
    while (!NRF_SAADC->EVENTS_END);                          // 等待采样完成 - Wait for completion
    NRF_SAADC->EVENTS_END = 0x00UL;
    NRF_SAADC->TASKS_STOP = 0x01UL;                          // 停止ADC - Stop ADC
    while (!NRF_SAADC->EVENTS_STOPPED);                      // 等待停止完成 - Wait for stop
    NRF_SAADC->EVENTS_STOPPED = 0x00UL;
    if (value < 0) value = 0;                                // 处理负值 - Handle negative values
    NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos); // 禁用SAADC - Disable SAADC
    NRF_LOG_DEBUG("ADC value: %d\n", value);
    // Return voltage in millivolts (mV)
    // 返回电压值(单位:毫伏)
    // Original: (value * 3.6V) / 1024 returns volts
    // 原公式: (value * 3.6V) / 1024 返回伏特
    // New: (value * 3600mV) / 1024 returns millivolts
    // 新公式: (value * 3600mV) / 1024 返回毫伏
    return (uint16_t)((value * 3600) / 1024);
}

// Cached voltage value in mV (updated during screen refresh)
// 缓存的电压值(单位:毫伏,在屏幕刷新时更新)
static uint16_t m_cached_voltage = 3300;  // Default to normal voltage (3.3V = 3300mV) - 默认为正常电压(3.3V = 3300mV)

/**
 * 读取电压并缓存
 * Read voltage and cache
 *
 * @return 电压值(单位:毫伏) - Voltage in millivolts
 *
 * 该函数在每次屏幕刷新时被调用,更新缓存的电压值
 * This function is called during each screen refresh to update the cached voltage
 *
 * 缓存电压值的好处:
 * Benefits of caching voltage:
 * 1. 避免频繁的ADC操作,减少功耗 - Avoid frequent ADC operations, reduce power consumption
 * 2. 低电量检测可以快速判断,无需等待ADC转换 - Fast low battery detection without ADC conversion
 * 3. 电压值基于最近一次屏幕刷新,具有时效性 - Voltage based on recent screen refresh, time-relevant
 */
uint16_t EPD_ReadVoltageAndCache(void) {
    m_cached_voltage = EPD_ReadVoltage();
    return m_cached_voltage;
}

/**
 * 检测是否低电量
 * Check if battery is low
 *
 * @return true=低电量, false=电量正常 - true=low battery, false=normal
 *
 * 使用缓存的电压值进行判断,避免ADC操作
 * Uses cached voltage for checking, avoiding ADC operation
 *
 * 低电量阈值: 2800mV (2.8V)
 * Low battery threshold: 2800mV (2.8V)
 *
 * 重要提示:
 * Important notes:
 * - 此函数不执行ADC测量,而是使用最近一次刷新时缓存的电压值
 *   This function does not perform ADC measurement, but uses the voltage cached during the last refresh
 * - 要更新电压值,需要调用 EPD_ReadVoltageAndCache()
 *   To update voltage, call EPD_ReadVoltageAndCache()
 * - 在屏幕刷新函数中会自动更新电压缓存
 *   Voltage cache is automatically updated in screen refresh functions
 */
bool EPD_IsLowBattery(void) {
    // Use cached voltage to avoid ADC measurement
    // 使用缓存电压避免ADC测量
    // Voltage is updated during each screen refresh
    // 电压在每次屏幕刷新时更新
    return m_cached_voltage < LOW_BATTERY_THRESHOLD_MV;
}

// EPD models (Partial refresh version MUST be first for clock mode)
// EPD型号数组(部分刷新版本必须放在第一位,用于时钟模式)
/**
 * 重要说明 - IMPORTANT NOTES:
 *
 * 数组顺序的重要性 - Array Order Importance:
 * ========================================
 * 1. epd_uc8179_750_Partial_bwr 必须放在第一位(索引0)
 *    epd_uc8179_750_Partial_bwr MUST be first (index 0)
 *
 * 2. 时钟模式默认使用数组的第一个型号(epd_models[0])
 *    Clock mode uses the first model in array by default (epd_models[0])
 *
 * 3. 时钟模式需要部分刷新功能,因此必须使用支持部分刷新的版本
 *    Clock mode requires partial refresh, so must use the partial refresh version
 *
 * 4. 如果改变数组顺序,时钟模式将无法正常工作
 *    If array order is changed, clock mode will not work properly
 *
 * 型号说明 - Model Description:
 * ========================================
 * - epd_uc8179_750_Partial_bwr: 支持部分刷新的版本(用于时钟模式)
 *   epd_uc8179_750_Partial_bwr: Partial refresh version (for clock mode)
 *
 * - epd_uc8179_750_bwr: 仅支持全刷新的版本(用于图像显示)
 *   epd_uc8179_750_bwr: Full refresh only version (for image display)
 */
extern epd_model_t epd_uc8179_750_bwr;          // 全刷新版本 - Full refresh version
extern epd_model_t epd_uc8179_750_Partial_bwr;  // 部分刷新版本 - Partial refresh version

static epd_model_t* epd_models[] = {
    &epd_uc8179_750_Partial_bwr,  // Partial refresh version (for clock mode) - must be first!
                                   // 部分刷新版本(用于时钟模式) - 必须放在第一位!
    &epd_uc8179_750_bwr,           // Full refresh only version
                                   // 仅全刷新版本
};

/**
 * 初始化EPD型号
 * Initialize EPD model
 *
 * @param id EPD型号ID - EPD model ID
 * @return EPD型号结构体指针 - Pointer to EPD model structure
 *
 * 功能说明 - Function Description:
 * ========================================
 * 1. 根据给定的型号ID在epd_models数组中查找对应的型号
 *    Search for the corresponding model in epd_models array based on the given ID
 *
 * 2. 如果找到匹配的型号,使用该型号并初始化
 *    If a matching model is found, use it and initialize
 *
 * 3. 如果未找到匹配的型号,默认使用epd_models[0](部分刷新版本)
 *    If no match found, default to epd_models[0] (partial refresh version)
 *
 * 4. 调用该型号的init函数完成硬件初始化
 *    Call the model's init function to complete hardware initialization
 *
 * 默认型号选择的重要性 - Importance of Default Model Selection:
 * ========================================
 * - 默认型号是epd_models[0],即部分刷新版本
 *   Default model is epd_models[0], which is the partial refresh version
 *
 * - 这确保了即使型号ID不匹配,系统仍能以时钟模式运行
 *   This ensures the system can still run in clock mode even if model ID doesn't match
 *
 * - 时钟模式是最常用的模式,需要部分刷新功能
 *   Clock mode is the most common mode and requires partial refresh capability
 */
epd_model_t* epd_init(epd_model_id_t id) {
    epd_model_t* epd = NULL;
    // 遍历所有支持的型号,查找匹配的ID
    // Iterate through all supported models to find matching ID
    for (uint8_t i = 0; i < ARRAY_SIZE(epd_models); i++) {
        if (epd_models[i]->id == id) {
            epd = epd_models[i];
            break;  // Found matching model, no need to continue
                    // 找到匹配型号,无需继续查找
        }
    }
    // 如果未找到匹配型号,使用默认型号(数组第一个,即部分刷新版本)
    // If no match found, use default model (first in array, partial refresh version)
    if (epd == NULL) epd = epd_models[0];

    // 调用该型号的初始化函数
    // Call the model's initialization function
    epd->drv->init(epd);
    return epd;
}
