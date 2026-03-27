/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 * 版权所有 (c) 2014 Nordic Semiconductor。保留所有权利。
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * 此处包含的信息为Nordic Semiconductor ASA的财产。
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 * 使用条款和条件在NORDIC SEMICONDUCTOR标准软件许可协议中有详细描述。
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 * 被许可方被授予免费的、不可转让的信息使用权。不提供任何形式的保证。
 * 此标题不得从文件中删除。
 *
 */

#include <stdint.h>
#include <string.h>

#include "ble.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "ble_dfu.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "nordic_common.h"
#include "nrf.h"
#include "nrf_ble_gatt.h"
#include "nrf_bootloader_info.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "EPD_service.h"
#include "app_error.h"
#include "app_scheduler.h"
#include "app_timer.h"
#include "main.h"
#include "nrf_drv_gpiote.h"
#include "nrf_drv_wdt.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_power.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_log_default_backends.h"

// clang-format off
#define CENTRAL_LINK_COUNT              0                                               /**< Number of central links used by the application. When changing this number remember to adjust the RAM settings*/
                                                                                        /**< 应用程序使用的中心链接数量。更改此数字时请记得调整RAM设置 */
#define PERIPHERAL_LINK_COUNT           1                                               /**< Number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/
                                                                                        /**< 应用程序使用的外设链接数量。更改此数字时请记得调整RAM设置 */

#define DEVICE_NAME                      "NRF_EPD"                                      /**< Name of device. Will be included in the advertising data. */
                                                                                        /**< 设备名称。将包含在广播数据中。 */
#define APP_ADV_INTERVAL                 1600                                           /**< The advertising interval (in units of 0.625 ms. This value corresponds to 1 s). */
                                                                                        /**< 广播间隔（以0.625毫秒为单位。此值对应1秒）。 */
#define APP_ADV_TIMEOUT_IN_SECONDS       120                                            /**< The advertising timeout (in units of seconds). */
                                                                                        /**< 广播超时时间（以秒为单位）。 */
#define APP_TIMER_PRESCALER              0                                              /**< Value of the RTC1 PRESCALER register. */
                                                                                        /**< RTC1预分频器寄存器的值。 */
#define APP_TIMER_OP_QUEUE_SIZE          4                                              /**< Size of timer operation queues. */
                                                                                        /**< 定时器操作队列的大小。 */

#define APP_BLE_CONN_CFG_TAG            1                                               /**< A tag identifying the SoftDevice BLE configuration. */
                                                                                        /**< 标识SoftDevice BLE配置的标签。 */
#define APP_BLE_OBSERVER_PRIO           3                                               /**< Application's BLE observer priority. You shouldn't need to modify this value. */
                                                                                        /**< 应用程序的BLE观察者优先级。通常不需要修改此值。 */
#define TIMER_TICKS(MS) APP_TIMER_TICKS(MS)

#define MIN_CONN_INTERVAL                MSEC_TO_UNITS(7.5, UNIT_1_25_MS)               /**< Minimum connection interval (7.5 ms) */
                                                                                        /**< 最小连接间隔（7.5毫秒） */
#define MAX_CONN_INTERVAL                MSEC_TO_UNITS(30, UNIT_1_25_MS)                /**< Maximum connection interval (30 ms). */
                                                                                        /**< 最大连接间隔（30毫秒） */
#define SLAVE_LATENCY                    6                                              /**< Slave latency. */
                                                                                        /**< 从机延迟。 */
#define CONN_SUP_TIMEOUT                 MSEC_TO_UNITS(4000, UNIT_10_MS)                /**< Connection supervisory timeout (~4000 ms). */
                                                                                        /**< 连接监督超时（约4000毫秒） */
#define FIRST_CONN_PARAMS_UPDATE_DELAY   TIMER_TICKS(5000)                              /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
                                                                                        /**< 从启动事件（连接或开始通知）到第一次调用sd_ble_gap_conn_param_update的时间（5秒） */
#define NEXT_CONN_PARAMS_UPDATE_DELAY    TIMER_TICKS(30000)                             /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
                                                                                        /**< 第一次调用后每次调用sd_ble_gap_conn_param_update之间的时间（30秒） */
#define MAX_CONN_PARAMS_UPDATE_COUNT     3                                              /**< Number of attempts before giving up the connection parameter negotiation. */
                                                                                        /**< 放弃连接参数协商前的尝试次数 */

#define SCHED_MAX_EVENT_DATA_SIZE       EPD_GUI_SCHD_EVENT_DATA_SIZE                    /**< Maximum size of scheduler events. */
                                                                                        /**< 调度器事件的最大大小 */
#define SCHED_QUEUE_SIZE                10                                              /**< Maximum number of events in the scheduler queue. */
                                                                                        /**< 调度器队列中的最大事件数 */

#define CLOCK_TIMER_INTERVAL             TIMER_TICKS(1000)                              /**< Clock timer interval (ticks). */
                                                                                        /**< 时钟定时器间隔（滴答数）每1000ms触发一次 */

#define DEAD_BEEF                        0xDEADBEEF                                     /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */
                                                                                        /**< 用作栈转储错误代码的值，可用于在栈展开时识别栈位置 */

NRF_BLE_GATT_DEF(m_gatt);                                                               /**< GATT module instance. */
                                                                                        /**< GATT模块实例 */
BLE_ADVERTISING_DEF(m_advertising);                                                     /**< Advertising module instance. */
                                                                                        /**< 广播模块实例 */
static uint16_t                          m_conn_handle = BLE_CONN_HANDLE_INVALID;       /**< Handle of the current connection. */
                                                                                        /**< 当前连接的句柄 */
static ble_uuid_t                        m_adv_uuids[] = {{BLE_UUID_EPD_SVC, \
                                                           EPD_SVC_UUID_TYPE}};         /**< Universally unique service identifier. */
                                                                                        /**< 通用唯一服务标识符 */

BLE_EPD_DEF(m_epd);                                                                     /**< Structure to identify the EPD Service. */
                                                                                        /**< 用于标识EPD服务的结构体 */
static uint32_t                          m_timestamp = 1735689600;                      /**< Current timestamp. */
                                                                                        /**< 当前时间戳，初始值为2025-01-01 00:00:00 UTC */
APP_TIMER_DEF(m_clock_timer_id);                                                        /**< Clock timer. */
                                                                                        /**< 时钟定时器 */
static nrf_drv_wdt_channel_id            m_wdt_channel_id;                              /**< 看门狗定时器通道ID */
static uint32_t                          m_wdt_last_feed_time = 0;                      /**< 上次喂狗时间戳 */
static uint32_t                          m_resetreas;                                   /**< 复位原因寄存器值 */
// clang-format on

/**@brief Callback function for asserts in the SoftDevice.
 * SoftDevice中断言的回调函数
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 * 当SoftDevice中发生断言时将调用此函数
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of Assert.
 * 警告：此处理程序仅为示例，不适用于最终产品。您需要分析产品在断言情况下应如何反应。
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 * 警告：当SoftDevice发生断言时，系统只能通过复位来恢复。
 *
 * @param[in] line_num   Line number of the failing ASSERT call.
 *                       失败的ASSERT调用的行号
 * @param[in] file_name  File name of the failing ASSERT call.
 *                       失败的ASSERT调用的文件名
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t* p_file_name) {
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}

// return current timestamp
// 返回当前时间戳
uint32_t timestamp(void) { return m_timestamp; }

// set the timestamp
// 设置时间戳
// 停止时钟定时器，更新时间戳，然后重新启动定时器
void set_timestamp(uint32_t timestamp) {
    app_timer_stop(m_clock_timer_id);      // 停止时钟定时器
    m_timestamp = timestamp;               // 更新时间戳
    app_timer_start(m_clock_timer_id, CLOCK_TIMER_INTERVAL, NULL);  // 重新启动定时器
}

// reload the wdt channel
// 重新加载看门狗定时器通道（喂狗）
// 每30秒喂一次狗，防止系统复位
void app_feed_wdt(void) {
    if (m_timestamp - m_wdt_last_feed_time >= 30) {  // 检查是否已过30秒
        NRF_LOG_DEBUG("Feed WDT\n");
        nrf_drv_wdt_channel_feed(m_wdt_channel_id);  // 喂狗
        m_wdt_last_feed_time = m_timestamp;           // 更新上次喂狗时间
    }
}

/**@brief 无按钮DFU的SoftDevice处理器状态观察者
 *
 * @details 在进入DFU模式前关闭SoftDevice时调用
 *
 * @param[in] state     SoftDevice状态事件
 * @param[in] p_context 上下文指针（未使用）
 */
static void buttonless_dfu_sdh_state_observer(nrf_sdh_state_evt_t state, void* p_context) {
    if (state == NRF_SDH_EVT_STATE_DISABLED) {
        // Softdevice was disabled before going into reset. Inform bootloader to skip CRC on next boot.
        // Softdevice在复位前被禁用。通知引导加载程序在下次启动时跳过CRC检查。
        nrf_power_gpregret2_set(BOOTLOADER_DFU_SKIP_CRC);

        // Go to system off.
        // 进入系统关闭模式
        nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_GOTO_SYSOFF);
    }
}

/* nrf_sdh state observer. */
/* nrf_sdh状态观察者 */
NRF_SDH_STATE_OBSERVER(m_buttonless_dfu_state_obs, 0) = {
    .handler = buttonless_dfu_sdh_state_observer,
};

/**@brief 获取广播配置
 *
 * @details 设置BLE广播模式配置参数
 *
 * @param[out] p_config 指向广播模式配置结构体的指针
 */
static void advertising_config_get(ble_adv_modes_config_t* p_config) {
    memset(p_config, 0, sizeof(ble_adv_modes_config_t));  // 清零配置结构体

    p_config->ble_adv_fast_enabled = true;                      // 启用快速广播
    p_config->ble_adv_fast_interval = APP_ADV_INTERVAL;         // 设置广播间隔
    p_config->ble_adv_fast_timeout = APP_ADV_TIMEOUT_IN_SECONDS * 100;  // 设置广播超时（单位：10ms）
}

/**@brief 无按钮DFU事件处理函数
 *
 * @details 处理DFU相关事件，包括进入引导加载程序模式的准备和执行
 *
 * @param[in] event DFU事件类型
 */
static void ble_dfu_evt_handler(ble_dfu_buttonless_evt_type_t event) {
    switch (event) {
        case BLE_DFU_EVT_BOOTLOADER_ENTER_PREPARE: {
            NRF_LOG_INFO("Device is preparing to enter bootloader mode.");
            // 设备正在准备进入引导加载程序模式

            // Prevent device from advertising on disconnect.
            // 防止设备在断开连接时广播
            ble_adv_modes_config_t config;
            advertising_config_get(&config);
            config.ble_adv_on_disconnect_disabled = true;  // 禁用断开连接时的广播
            ble_advertising_modes_config_set(&m_advertising, &config);

            // Disconnect all other bonded devices that currently are connected.
            // This is required to receive a service changed indication
            // on bootup after a successful (or aborted) Device Firmware Update.
            // 断开所有当前连接的已绑定设备。
            // 这是必需的，以便在成功（或中止）设备固件更新后在启动时接收服务更改指示。
            APP_ERROR_CHECK(sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;
        }

        case BLE_DFU_EVT_BOOTLOADER_ENTER:
            NRF_LOG_INFO("Device will enter bootloader mode.");
            // 设备将进入引导加载程序模式
            break;

        case BLE_DFU_EVT_BOOTLOADER_ENTER_FAILED:
            NRF_LOG_ERROR("Request to enter bootloader mode failed asynchroneously.");
            // 进入引导加载程序模式的请求异步失败
            APP_ERROR_CHECK(false);
            break;

        case BLE_DFU_EVT_RESPONSE_SEND_ERROR:
            NRF_LOG_ERROR("Request to send a response to client failed.");
            // 向客户端发送响应的请求失败
            APP_ERROR_CHECK(false);
            break;

        default:
            NRF_LOG_ERROR("Unknown event from ble_dfu_buttonless.");
            // 来自ble_dfu_buttonless的未知事件
            break;
    }
}

/**@brief 时钟定时器超时处理函数
 *
 * @details 每秒调用一次，用于更新时间戳、处理低电量LED闪烁和EPD定时事件
 *
 * @param[in] p_context 上下文指针（未使用）
 */
static void clock_timer_timeout_handler(void* p_context) {
    UNUSED_PARAMETER(p_context);

    m_timestamp++;  // 时间戳递增（每秒一次）

    // Low battery LED blinking (every 2 seconds)
    // Uses cached voltage from last screen refresh, no ADC operation here
    // 低电量LED闪烁（每2秒一次）
    // 使用上次屏幕刷新时缓存的电压值，此处不进行ADC操作
    if (m_timestamp % 2 == 0 && EPD_IsLowBattery()) {  // 每2秒检查一次
        EPD_LED_BLINK();  // 闪烁LED指示低电量
    }

    ble_epd_on_timer(&m_epd, m_timestamp, false);  // 通知EPD服务定时器事件
}

/**@brief Function for the Event Scheduler initialization.
 * 事件调度器初始化函数
 *
 * @details 初始化应用程序事件调度器，用于处理延迟执行的事件
 */
static void scheduler_init(void) { APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE); }

/**@brief Function for the Timer initialization.
 * 定时器初始化函数
 *
 * @details Initializes the timer module. This creates and starts application timers.
 * 初始化定时器模块。创建并启动应用程序定时器。
 */
static void timers_init(void) {
    // Initialize timer module.
    // 初始化定时器模块
    APP_ERROR_CHECK(app_timer_init());
    // Create timers.
    // 创建定时器
    // 创建时钟定时器，模式为重复触发，超时处理函数为clock_timer_timeout_handler
    APP_ERROR_CHECK(app_timer_create(&m_clock_timer_id, APP_TIMER_MODE_REPEATED, clock_timer_timeout_handler));
}

/**@brief Function for starting application timers.
 * 启动应用程序定时器函数
 *
 * @details 启动时钟定时器，开始定期更新时间戳
 */
static void application_timers_start(void) {
    // Start application timers.
    // 启动应用程序定时器
    APP_ERROR_CHECK(app_timer_start(m_clock_timer_id, CLOCK_TIMER_INTERVAL, NULL));  // 启动时钟定时器
}

/**@brief Function for putting the chip into sleep mode.
 * 使芯片进入睡眠模式的函数
 *
 * @note This function will not return.
 * 注意：此函数不会返回
 *
 * @details 进入深度睡眠模式以最小化功耗，在EPD准备完成后关闭系统
 */
void sleep_mode_enter(void) {
    NRF_LOG_DEBUG("Entering deep sleep mode\n");
    // 进入深度睡眠模式
    NRF_LOG_FINAL_FLUSH();  // 刷新日志缓冲区
    nrf_delay_ms(100);      // 延迟100ms确保日志输出完成

    ble_epd_sleep_prepare(&m_epd);  // EPD睡眠准备（保存状态等）
    nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_GOTO_SYSOFF);  // 关闭系统电源
}

/**@brief Function for initializing services that will be used by the application.
 * 初始化应用程序将使用的服务
 *
 * @details 初始化EPD服务和DFU服务
 */
static void services_init(void) {
    // Initialize EPD Service.
    // 初始化EPD服务
    memset(&m_epd, 0, sizeof(ble_epd_t));  // 清零EPD服务结构体
    APP_ERROR_CHECK(ble_epd_init(&m_epd));  // 初始化EPD服务

    // 初始化无按钮DFU服务
    ble_dfu_buttonless_init_t dfus_init = {0};
    dfus_init.evt_handler = ble_dfu_evt_handler;  // 设置DFU事件处理函数
    APP_ERROR_CHECK(ble_dfu_buttonless_init(&dfus_init));
}

/**@brief Function for the GAP initialization.
 * GAP初始化函数
 *
 * @details This function will set up all the necessary GAP (Generic Access Profile) parameters of
 *          the device. It also sets the permissions and appearance.
 * 此函数将设置设备所有必要的GAP（通用访问配置文件）参数。
 * 它还设置权限和外观。
 */
static void gap_params_init(void) {
    char device_name[20];
    ble_gap_addr_t addr;
    ble_gap_conn_params_t gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);  // 设置为开放安全模式（无加密）
    APP_ERROR_CHECK(sd_ble_gap_addr_get(&addr));  // 获取设备蓝牙地址

    // 打印蓝牙MAC地址
    NRF_LOG_INFO("Bluetooth MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", addr.addr[5], addr.addr[4], addr.addr[3],
                 addr.addr[2], addr.addr[1], addr.addr[0]);

    // 根据MAC地址后两位生成唯一设备名称，例如：NRF_EPD_1234
    snprintf(device_name, 20, "%s_%02X%02X", DEVICE_NAME, addr.addr[1], addr.addr[0]);
    APP_ERROR_CHECK(sd_ble_gap_device_name_set(&sec_mode, (const uint8_t*)device_name, strlen(device_name)));

    // 设置GAP连接参数
    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;  // 最小连接间隔
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;  // 最大连接间隔
    gap_conn_params.slave_latency = SLAVE_LATENCY;          // 从机延迟
    gap_conn_params.conn_sup_timeout = CONN_SUP_TIMEOUT;    // 连接监督超时

    APP_ERROR_CHECK(sd_ble_gap_ppcp_set(&gap_conn_params));  // 设置外围首选连接参数
}

/**@brief Function for handling an event from the Connection Parameters Module.
 * 连接参数模块事件处理函数
 *
 * @details This function will be called for all events in the Connection Parameters Module
 *          which are passed to the application.
 * 此函数将被所有传递给应用程序的连接参数模块事件调用。
 *
 * @note All this function does is to disconnect. This could have been done by simply setting
 *       the disconnect_on_fail config parameter, but instead we use the event handler
 *       mechanism to demonstrate its use.
 * 注意：此函数所做的只是断开连接。这可以通过简单地设置disconnect_on_fail配置参数来完成，
 * 但我们使用事件处理程序机制来演示其用法。
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 *                   从连接参数模块接收到的事件
 */
static void on_conn_params_evt(ble_conn_params_evt_t* p_evt) {
    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED) {
        // Do not force disconnect on parameter update failure; keep connection for better
        // compatibility and long transfers, just log the event.
        // 参数更新失败时不强制断开连接；保持连接以获得更好的兼容性和长时间传输，只记录事件。
        NRF_LOG_WARNING("Conn params update failed, keeping connection");
    }
}

/**@brief Function for handling errors from the Connection Parameters module.
 * 连接参数模块错误处理函数
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 *                       包含错误信息的错误代码
 */
static void conn_params_error_handler(uint32_t nrf_error) { APP_ERROR_HANDLER(nrf_error); }

/**@brief Function for initializing the Connection Parameters module.
 * 连接参数模块初始化函数
 *
 * @details 设置连接参数更新的时间和重试次数
 */
static void conn_params_init(void) {
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params = NULL;  // 使用GAP中设置的连接参数
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;  // 第一次更新延迟（5秒）
    cp_init.next_conn_params_update_delay = NEXT_CONN_PARAMS_UPDATE_DELAY;    // 后续更新延迟（30秒）
    cp_init.max_conn_params_update_count = MAX_CONN_PARAMS_UPDATE_COUNT;      // 最大尝试次数（3次）
    cp_init.start_on_notify_cccd_handle = BLE_GATT_HANDLE_INVALID;            // 不在通知启用时触发更新
    cp_init.disconnect_on_fail = false;  // 失败时不断开连接，以保持兼容性
    cp_init.evt_handler = on_conn_params_evt;          // 事件处理函数
    cp_init.error_handler = conn_params_error_handler;  // 错误处理函数

    APP_ERROR_CHECK(ble_conn_params_init(&cp_init));  // 初始化连接参数模块
}

/**@brief 启动BLE广播
 *
 * @details 以快速模式启动BLE广播，使设备可被发现和连接
 */
static void advertising_start(void) {
    NRF_LOG_INFO("advertising start\n");
    // 开始广播
    APP_ERROR_CHECK(ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST));
}

/**@brief GPIO事件处理函数
 *
 * @details 当唤醒引脚检测到低到高电平变化时调用，用于从睡眠模式唤醒设备
 *
 * @param[in] pin    触发事件的引脚号
 * @param[in] action 触发的动作类型（低到高或高到低）
 */
void gpiote_evt_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
    NRF_LOG_DEBUG("pin: %d, event: %d\n", pin, action);

    // 禁用并清理GPIO中断
    nrf_drv_gpiote_in_event_disable(pin);  // 禁用引脚事件
    nrf_drv_gpiote_in_uninit(pin);         // 取消初始化引脚
    nrf_drv_gpiote_uninit();               // 取消初始化GPIOTE模块

    // blink LED on wakeup
    // 唤醒时闪烁LED
    EPD_LED_BLINK();

    advertising_start();  // 开始广播
}

/**@brief 设置唤醒引脚
 *
 * @details 配置GPIO引脚为输入模式，检测低到高电平变化以唤醒设备
 *
 * @param[in] pin 要配置为唤醒引脚的GPIO引脚号
 */
static void setup_wakeup_pin(nrf_drv_gpiote_pin_t pin) {
    NRF_LOG_DEBUG("Setting up wakeup pin\n");

    APP_ERROR_CHECK(nrf_drv_gpiote_init());  // 初始化GPIOTE模块
    nrf_drv_gpiote_in_config_t config = GPIOTE_CONFIG_IN_SENSE_LOTOHI(false);  // 配置为低到高触发
    APP_ERROR_CHECK(nrf_drv_gpiote_in_init(pin, &config, gpiote_evt_handler));  // 初始化引脚并设置处理函数
    nrf_drv_gpiote_in_event_enable(pin, true);  // 启用引脚事件
}

/**@brief Function for handling advertising events.
 * 广播事件处理函数
 *
 * @details This function will be called for advertising events which are passed to the application.
 * 此函数将被传递给应用程序的广播事件调用。
 *
 * @param[in] ble_adv_evt  Advertising event.
 *                         广播事件
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt) {
    switch (ble_adv_evt) {
        case BLE_ADV_EVT_FAST:
            // 快速广播模式启动
            break;
        case BLE_ADV_EVT_IDLE:
            // 广播超时（120秒后）
            NRF_LOG_INFO("advertising timeout\n");
            if (m_epd.config.wakeup_pin != 0xFF) {  // 如果配置了唤醒引脚
                if (m_epd.config.display_mode == MODE_PICTURE)
                    sleep_mode_enter();  // 图片模式：进入深度睡眠
                else
                    setup_wakeup_pin(m_epd.config.wakeup_pin);  // 其他模式：设置唤醒引脚
            } else {
                advertising_start();  // 未配置唤醒引脚：继续广播
            }
            break;
        default:
            break;
    }
}

/**@brief Function for the application's SoftDevice event handler.
 * 应用程序的SoftDevice事件处理函数
 *
 * @param[in] p_ble_evt SoftDevice event.
 *                      SoftDevice事件
 */
static void on_ble_evt(ble_evt_t* p_ble_evt) {
    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            // BLE连接建立事件
            NRF_LOG_INFO("CONNECTED\n");
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;  // 保存连接句柄
            break;

        case BLE_GAP_EVT_DISCONNECTED: {
            // BLE断开连接事件
            uint8_t reason = p_ble_evt->evt.gap_evt.params.disconnected.reason;
            NRF_LOG_INFO("DISCONNECTED, reason=0x%02X\n", reason);
            m_conn_handle = BLE_CONN_HANDLE_INVALID;  // 清除连接句柄
            break;
        }
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            // PHY更新请求事件（用于支持2M PHY等）
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys = {
                .rx_phys = BLE_GAP_PHY_AUTO,  // 自动选择接收PHY
                .tx_phys = BLE_GAP_PHY_AUTO,  // 自动选择发送PHY
            };
            APP_ERROR_CHECK(sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys));
        } break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported
            // 不支持配对，拒绝配对请求
            APP_ERROR_CHECK(
                sd_ble_gap_sec_params_reply(m_conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL));
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            // No system attributes have been stored.
            // 没有存储系统属性，设置为空
            APP_ERROR_CHECK(sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0));
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            // GATT客户端超时，断开连接
            APP_ERROR_CHECK(
                sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            // GATT服务器超时，断开连接
            APP_ERROR_CHECK(
                sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;

        default:
            // No implementation needed.
            // 无需处理的其他事件
            break;
    }
}

/**@brief Function for handling BLE events.
 * BLE事件处理函数
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 *                          蓝牙协议栈事件
 * @param[in]   p_context   Unused.
 *                          未使用的上下文指针
 */
static void ble_evt_handler(ble_evt_t const* p_ble_evt, void* p_context) {
    UNUSED_PARAMETER(p_context);

    on_ble_evt((ble_evt_t*)p_ble_evt);  // 调用BLE事件处理函数
}

/**@brief Function for the SoftDevice initialization.
 * SoftDevice初始化函数
 *
 * @details This function initializes the SoftDevice and the BLE event interrupt.
 * 此函数初始化SoftDevice和BLE事件中断。
 */
static void ble_stack_init(void) {
    // 使用新的SoftDevice处理器接口
    APP_ERROR_CHECK(nrf_sdh_enable_request());  // 请求启用SoftDevice

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    // 使用默认设置配置BLE协议栈
    // 获取应用程序RAM的起始地址
    uint32_t ram_start = 0;
    APP_ERROR_CHECK(nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start));

    // Enable BLE stack.
    // 启用BLE协议栈
    APP_ERROR_CHECK(nrf_sdh_ble_enable(&ram_start));

    // Register a handler for BLE events.
    // 注册BLE事件处理程序
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}

/**@brief Function for handling events from the GATT library.
 * GATT库事件处理函数
 *
 * @details 处理MTU交换事件，更新最大数据长度
 *
 * @param[in] p_gatt 指向GATT实例的指针
 * @param[in] p_evt  GATT事件
 */
void gatt_evt_handler(nrf_ble_gatt_t* p_gatt, nrf_ble_gatt_evt_t const* p_evt) {
    if ((m_conn_handle == p_evt->conn_handle) && (p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED)) {
        // ATT MTU已更新，计算最大数据长度
        // MTU - 3字节的ATT操作码和句柄 = 可用数据长度
        m_epd.max_data_len = p_evt->params.att_mtu_effective - 3;
        NRF_LOG_INFO("Data len is set to 0x%X(%d)", m_epd.max_data_len, m_epd.max_data_len);
    }
    NRF_LOG_DEBUG("ATT MTU exchange completed. central 0x%x peripheral 0x%x", p_gatt->att_mtu_desired_central,
                  p_gatt->att_mtu_desired_periph);
}

/**@brief Function for initializing the GATT library.
 * GATT库初始化函数
 *
 * @details 初始化GATT模块并设置最大MTU大小
 */
void gatt_init(void) {
    APP_ERROR_CHECK(nrf_ble_gatt_init(&m_gatt, gatt_evt_handler));  // 初始化GATT并注册事件处理函数
    APP_ERROR_CHECK(nrf_ble_gatt_att_mtu_periph_set(&m_gatt, NRF_SDH_BLE_GATT_MAX_MTU_SIZE));  // 设置外设MTU大小
}

/**@brief Function for initializing the Advertising functionality.
 * 广播功能初始化函数
 *
 * @details 配置广播数据包，包括设备名称、服务UUID和广播参数
 */
static void advertising_init(void) {
    // 使用新的广播接口
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    init.advdata.name_type = BLE_ADVDATA_FULL_NAME;  // 包含完整设备名称
    init.advdata.include_appearance = false;         // 不包含外观
    init.advdata.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_LIMITED_DISC_MODE;  // 仅LE有限可发现模式

    // 扫描响应数据：包含服务UUID
    init.srdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    init.srdata.uuids_complete.p_uuids = m_adv_uuids;

    // 广播配置
    init.config.ble_adv_fast_enabled = true;                         // 启用快速广播
    init.config.ble_adv_fast_interval = APP_ADV_INTERVAL;            // 广播间隔1秒
    init.config.ble_adv_fast_timeout = APP_ADV_TIMEOUT_IN_SECONDS * 100;  // 广播超时120秒
    init.evt_handler = on_adv_evt;  // 设置广播事件处理函数

    APP_ERROR_CHECK(ble_advertising_init(&m_advertising, &init));

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);  // 设置连接配置标签
}

/**@brief Function for initializing the nrf log module.
 * nrf日志模块初始化函数
 *
 * @details 初始化日志系统，使用时间戳作为日志时间源
 */
static void log_init(void) {
    APP_ERROR_CHECK(NRF_LOG_INIT(timestamp));  // 使用timestamp函数作为日志时间源
    NRF_LOG_DEFAULT_BACKENDS_INIT();  // 初始化默认日志后端（RTT等）
}

/**@brief Function for initializing power management.
 * 电源管理初始化函数
 *
 * @details 初始化电源管理模块，用于低功耗模式切换
 */
static void power_management_init(void) {
    APP_ERROR_CHECK(nrf_pwr_mgmt_init());
}

/**@brief Function for handling the idle state (main loop).
 * 空闲状态处理函数（主循环）
 *
 * @details If there is no pending log operation, then sleep until next the next event occurs.
 * 如果没有挂起的日志操作，则睡眠直到下一个事件发生。
 *
 * 此函数在主循环中持续调用，负责：
 * 1. 定期喂看门狗
 * 2. 处理日志输出
 * 3. 进入低功耗模式
 */
static void idle_state_handle(void) {
    app_feed_wdt();  // 喂看门狗，防止系统复位

    if (NRF_LOG_PROCESS() == false)  // 处理日志缓冲区，如果没有待处理的日志
        nrf_pwr_mgmt_run();          // 进入低功耗模式（等待事件唤醒）
}

/**
 * @brief WDT events handler.
 * 看门狗事件处理函数
 *
 * @details 当看门狗即将触发复位时调用（通常是因为未及时喂狗）
 *
 * @note The max amount of time we can spend in WDT interrupt is two cycles of 32768[Hz] clock - after that, reset
 * occurs
 * 注意：在看门狗中断中最多只能花费两个32768Hz时钟周期的时间，之后将发生复位
 */
void wdt_event_handler(void) {
    // NOTE: The max amount of time we can spend in WDT interrupt is two cycles of 32768[Hz] clock - after that, reset
    // occurs
    NRF_LOG_ERROR("WDT Rest!\r\n");  // 记录看门狗复位错误
    NRF_LOG_FINAL_FLUSH();           // 刷新日志缓冲区
}

/**@brief Function for application main entry.
 * 应用程序主入口函数
 *
 * @details 执行系统初始化并进入主循环
 *
 * 初始化流程：
 * 1. 日志系统初始化
 * 2. 读取并记录复位原因
 * 3. 看门狗配置和启动
 * 4. 定时器初始化
 * 5. 电源管理初始化
 * 6. BLE协议栈初始化
 * 7. 事件调度器初始化
 * 8. GAP参数初始化
 * 9. GATT初始化 或BLE选项设置
 * 10. 服务初始化（EPD和DFU）
 * 11. 广播初始化
 * 12. 连接参数初始化
 * 13. 启动定时器和广播
 * 14. 根据复位原因刷新显示
 * 15. 进入主循环
 */
int main(void) {
    log_init();  // 初始化日志系统

    // Save reset reason.
    // 保存复位原因
    m_resetreas = NRF_POWER->RESETREAS;  // 读取复位原因寄存器
    NRF_POWER->RESETREAS |= NRF_POWER->RESETREAS;  // 清除复位原因标志
    NRF_LOG_DEBUG("== RESET REASON: %d ===\n", m_resetreas);

    NRF_LOG_DEBUG("init..\n");

    // Configure WDT.
    // 配置看门狗定时器
    // 看门狗用于在系统卡死时自动复位，超时时间约32秒
    nrf_drv_wdt_config_t config = NRF_DRV_WDT_DEAFULT_CONFIG;
    APP_ERROR_CHECK(nrf_drv_wdt_init(&config, wdt_event_handler));  // 初始化看门狗
    APP_ERROR_CHECK(nrf_drv_wdt_channel_alloc(&m_wdt_channel_id));  // 分配看门狗通道
    nrf_drv_wdt_enable();  // 启动看门狗

    // 系统初始化序列
    timers_init();            // 初始化定时器模块
    power_management_init();  // 初始化电源管理
    ble_stack_init();         // 初始化BLE协议栈
    scheduler_init();         // 初始化事件调度器
    gap_params_init();        // 初始化GAP参数（设备名称、连接参数等）
    gatt_init();              // 初始化GATT（MTU交换等）
    ble_dfu_buttonless_async_svci_init();  // 初始化无按钮DFU的异步SVCI接口
    services_init();          // 初始化服务（EPD服务和DFU服务）
    advertising_init();       // 初始化广播
    conn_params_init();       // 初始化连接参数

    NRF_LOG_DEBUG("start..\n");

    // Start execution.
    // 开始执行
    application_timers_start();  // 启动应用程序定时器（时钟定时器）

    advertising_start();  // 开始BLE广播

    NRF_LOG_DEBUG("done.\n");

    // 根据复位原因决定显示内容
    if (m_resetreas & NRF_POWER_RESETREAS_DOG_MASK) {
        // 如果是看门狗复位，显示日历模式
        m_epd.config.display_mode = MODE_CALENDAR;
        ble_epd_on_timer(&m_epd, 0, true);  // 触发EPD刷新
    } else {
        // 其他复位原因，使用正常时间戳
        ble_epd_on_timer(&m_epd, m_timestamp, true);  // 触发EPD刷新
    }

    // 主循环：永久执行事件调度和空闲处理
    for (;;) {
        app_sched_execute();  // 执行调度队列中的事件
        idle_state_handle();  // 处理空闲状态（喂狗、日志处理、进入低功耗）
    }
}
