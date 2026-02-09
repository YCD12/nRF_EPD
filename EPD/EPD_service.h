/* Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

/**
 * @file EPD_service.h
 * @brief EPD BLE服务头文件 - 定义墨水屏蓝牙服务的接口和数据结构
 *
 * 本文件定义了墨水屏(EPD)的BLE服务接口,包括:
 * - 命令队列机制,用于管理多个命令的顺序执行
 * - BLE服务特性和UUID定义
 * - 显示模式、刷新策略等配置选项
 */

#ifndef __EPD_SERVICE_H
#define __EPD_SERVICE_H

#include <inttypes.h>
#include <stdbool.h>

#include "ble.h"
#include "ble_srv_common.h"
#include "nrf_sdh_ble.h"
#include "EPD_config.h"
#include "EPD_driver.h"
#include "GUI.h"
#include "sdk_config.h"

/**@brief   Macro for defining a ble_hts instance.
 *          定义BLE EPD服务实例的宏
 *
 * @param   _name   Name of the instance. 实例名称
 * @hideinitializer
 */
void ble_epd_evt_handler(ble_evt_t const* p_ble_evt, void* p_context);

#define BLE_EPD_BLE_OBSERVER_PRIO 2  // BLE事件观察者优先级
#define BLE_EPD_DEF(_name)  \
    static ble_epd_t _name; \
    NRF_SDH_BLE_OBSERVER(_name##_obs, BLE_EPD_BLE_OBSERVER_PRIO, ble_epd_evt_handler, &_name)

#define APP_VERSION 0x20  // 应用程序版本号

// EPD服务的128位基础UUID (自定义服务)
#define BLE_UUID_EPD_SVC_BASE \
    {{0XEC, 0X5A, 0X67, 0X1C, 0XC1, 0XB6, 0X46, 0XFB, 0X8D, 0X91, 0X28, 0XD8, 0X22, 0X36, 0X75, 0X62}}
#define BLE_UUID_EPD_SVC 0x0001   // EPD服务UUID
#define BLE_UUID_EPD_CHAR 0x0002  // EPD特性UUID (用于命令和数据传输)
#define BLE_UUID_APP_VER 0x0003   // 应用版本特性UUID

#define EPD_SVC_UUID_TYPE BLE_UUID_TYPE_VENDOR_BEGIN  // 自定义UUID类型

#define BLE_EPD_MAX_DATA_LEN (NRF_SDH_BLE_GATT_MAX_MTU_SIZE - 3)

/**< EPD Service command IDs. */
/**< EPD服务命令ID定义 */
enum EPD_CMDS {
    // 基础EPD硬件控制命令 (0x00-0x0F)
    EPD_CMD_SET_PINS = 0x00,     /**< set EPD pin mapping. 设置EPD引脚映射 */
    EPD_CMD_INIT = 0x01,         /**< init EPD display driver. 初始化EPD显示驱动 */
    EPD_CMD_CLEAR = 0x02,        /**< clear EPD screen. 清除EPD屏幕 */
    EPD_CMD_SEND_COMMAND = 0x03, /**< send command to EPD. 发送命令到EPD */
    EPD_CMD_SEND_DATA = 0x04,    /**< send data to EPD. 发送数据到EPD */
    EPD_CMD_REFRESH = 0x05,      /**< display EPD ram on screen. 刷新EPD显示RAM到屏幕 */
    EPD_CMD_SLEEP = 0x06,        /**< EPD enter sleep mode. EPD进入睡眠模式 */

    // 时间和显示模式设置命令 (0x20-0x2F)
    EPD_CMD_SET_TIME = 0x20,       /** < set time with unix timestamp. 使用Unix时间戳设置时间 */
    EPD_CMD_SET_WEEK_START = 0x21, /** < set week start day (0: Sunday, 1: Monday, ...). 设置周起始日(0:周日, 1:周一...) */
    EPD_CMD_SET_MODE = 0x22,       /** < set display mode (0: picture, 1: calendar, 2: clock, 3: clock+calendar). */
                                   /** < 设置显示模式 (0:图片, 1:日历, 2:时钟, 3:时钟+日历) */

    // 图像写入命令 (0x30-0x3F)
    EPD_CMD_WRITE_IMAGE = 0x30,      /**< write image data to EPD ram (legacy, no CRC). 写入图像数据到EPD RAM (旧版,无CRC校验) */
    EPD_CMD_WRITE_BLOCK = 0x31,      /**< write image block with CRC16 checksum. 带CRC16校验的分块写入 */
    EPD_CMD_QUERY_STATUS = 0x32,     /**< query transfer status for resume. 查询传输状态用于断点续传 */
    EPD_CMD_RESET_TRANSFER = 0x33,   /**< reset transfer state. 重置传输状态 */

    // 系统配置和控制命令 (0x90-0x9F) - 这些是紧急命令,会绕过队列立即执行
    EPD_CMD_SET_CONFIG = 0x90, /**< set full EPD config. 设置完整EPD配置 */
    EPD_CMD_SYS_RESET = 0x91,  /**< MCU reset. MCU复位 (紧急命令) */
    EPD_CMD_SYS_SLEEP = 0x92,  /**< MCU enter sleep mode. MCU进入睡眠模式 (紧急命令) */
    EPD_CMD_CFG_ERASE = 0x99,  /**< Erase config and reset. 擦除配置并复位 (紧急命令) */
};

/**
 * 响应类型定义 (MCU -> APP)
 * Response Type Definitions
 */
#define EPD_RSP_BLOCK_ACK       0xA0  /**< Block ACK/NACK response. 块确认/否认响应 */
#define EPD_RSP_STATUS          0xA1  /**< Transfer status response. 传输状态响应 */

/**
 * 传输配置常量
 * Transfer Configuration Constants
 */
#define EPD_MAX_BLOCKS          512   /**< Maximum number of blocks (96KB / 192B). 最大块数 */
#define EPD_BLOCK_BITMAP_SIZE   64    /**< Block bitmap size in bytes (512 bits). 块位图大小(字节) */
#define EPD_MAX_RETRIES         3     /**< Maximum retry count for failed blocks. 失败块的最大重传次数 */

/**
 * 命令队列配置
 * Command Queue Configuration
 *
 * 命令队列的作用:
 * 1. 防止命令并发执行导致的冲突 - EPD操作需要顺序执行
 * 2. 缓冲BLE快速发送的多个命令
 * 3. 支持长时间操作(如刷新)期间接收新命令
 */
#define EPD_CMD_QUEUE_SIZE 8  /**< Command queue size (max 8 pending commands). 命令队列大小(最多8个待处理命令) */
#define EPD_CMD_MAX_DATA_LEN 256  /**< Maximum command data length. 单个命令的最大数据长度 */

/**@brief Command queue item structure.
 *        命令队列项结构
 *
 * @details This structure holds a single command in the queue.
 *          此结构保存队列中的单个命令。
 */
typedef struct {
    uint8_t cmd;                       /**< Command ID. 命令ID */
    uint8_t data[EPD_CMD_MAX_DATA_LEN]; /**< Command data (including cmd byte). 命令数据(包含命令字节) */
    uint16_t length;                   /**< Data length. 数据长度 */
} epd_cmd_queue_item_t;

/**@brief Command queue structure.
 *        命令队列结构
 *
 * @details This structure manages the command queue for sequential execution.
 *          此结构管理命令队列以实现顺序执行。
 *
 * 队列工作原理:
 * - 使用环形缓冲区(circular buffer)实现
 * - head指向下一个要取出的命令
 * - tail指向下一个要插入命令的位置
 * - count记录当前队列中的命令数量
 * - is_processing标志防止重复启动处理流程
 */
typedef struct {
    epd_cmd_queue_item_t items[EPD_CMD_QUEUE_SIZE]; /**< Queue items. 队列项数组 */
    uint8_t head;          /**< Queue head index. 队列头索引(取出位置) */
    uint8_t tail;          /**< Queue tail index. 队列尾索引(插入位置) */
    uint8_t count;         /**< Current number of items in queue. 队列中当前项目数 */
    bool is_processing;    /**< Flag indicating if a command is being processed. 标志是否正在处理命令 */
} epd_cmd_queue_t;

/**@brief Image transfer context structure for resumable transfers.
 *        图像传输上下文结构(用于断点续传)
 *
 * @details Tracks the state of an ongoing image transfer to support:
 *          - CRC validation per block
 *          - Resume after disconnect
 *          - Missing block identification
 *          跟踪正在进行的图像传输状态,支持:
 *          - 每块CRC验证
 *          - 断开后恢复传输
 *          - 识别缺失块
 */
typedef struct {
    uint8_t  session_id;                           /**< Session ID for transfer validation. 传输验证的会话ID */
    uint16_t total_blocks;                         /**< Total number of blocks in transfer. 传输中的总块数 */
    uint16_t received_blocks;                      /**< Number of successfully received blocks. 成功接收的块数 */
    uint8_t  block_bitmap[EPD_BLOCK_BITMAP_SIZE];  /**< Bitmap tracking received blocks. 跟踪已接收块的位图 */
    bool     transfer_active;                      /**< Flag indicating active transfer. 标志是否有活跃传输 */
} image_transfer_ctx_t;

/**@brief EPD Service structure.
 *        EPD服务结构
 *
 * @details This structure contains status information related to the service.
 *          此结构包含与服务相关的状态信息。
 */
typedef struct {
    // BLE服务相关句柄
    uint16_t service_handle; /**< Handle of EPD Service (as provided by the S110 SoftDevice). */
                             /**< EPD服务句柄(由S110 SoftDevice提供) */
    ble_gatts_char_handles_t
        char_handles; /**< Handles related to the EPD characteristic (as provided by the SoftDevice). */
                      /**< EPD特性相关句柄(由SoftDevice提供) */
    ble_gatts_char_handles_t
        app_ver_handles;  /**< Handles related to the APP version characteristic (as provided by the SoftDevice). */
                          /**< APP版本特性相关句柄(由SoftDevice提供) */

    // BLE连接状态
    uint16_t conn_handle; /**< Handle of the current connection (as provided by the SoftDevice). BLE_CONN_HANDLE_INVALID
                             if not in a connection. */
                          /**< 当前连接句柄(由SoftDevice提供),如果未连接则为BLE_CONN_HANDLE_INVALID */
    uint16_t max_data_len;        /**< Maximum length of data (in bytes) that can be transmitted to the peer */
                                  /**< 可传输给对端的最大数据长度(字节) */
    bool is_notification_enabled; /**< Variable to indicate if the peer has enabled notification of the RX characteristic.*/
                                  /**< 标志对端是否启用了RX特性的通知 */

    // EPD硬件和配置
    epd_model_t* epd;             /**< current EPD model. 当前EPD型号 */
    epd_config_t config;          /**< EPD config. EPD配置 */

    // 刷新控制标志
    bool force_full_refresh;      /**< Force full refresh on next GUI update (used for mode switching). */
                                  /**< 下次GUI更新时强制全刷(用于模式切换)。
                                   *   当显示模式切换时设置此标志,确保清除残影 */
    bool is_refreshing;           /**< Flag to indicate screen refresh is in progress. */
                                  /**< 标志屏幕刷新正在进行中。
                                   *   防止BLE断开时中断刷新过程 */

    // 命令队列
    epd_cmd_queue_t cmd_queue;    /**< Command queue for sequential command execution. */
                                  /**< 用于顺序执行命令的命令队列 */

    // 图像传输上下文(用于CRC校验和断点续传)
    image_transfer_ctx_t transfer_ctx;  /**< Image transfer context for CRC validation and resume. */
                                        /**< 用于CRC校验和断点续传的图像传输上下文 */
} ble_epd_t;

/**@brief GUI update event structure.
 *        GUI更新事件结构
 *
 * @details Used to pass EPD instance and timestamp to GUI update function via app_scheduler.
 *          用于通过app_scheduler将EPD实例和时间戳传递给GUI更新函数。
 */
typedef struct {
    ble_epd_t* p_epd;    /**< Pointer to EPD service structure. EPD服务结构指针 */
    uint32_t timestamp;  /**< Unix timestamp for GUI update. GUI更新的Unix时间戳 */
} epd_gui_update_event_t;

#define EPD_GUI_SCHD_EVENT_DATA_SIZE sizeof(epd_gui_update_event_t)  /**< Scheduler event data size. 调度器事件数据大小 */

/**@brief Function for preparing sleep mode.
 *        准备进入睡眠模式的函数
 *
 * @details Turns off LED and configures wakeup pin before entering sleep mode.
 *          进入睡眠模式前关闭LED并配置唤醒引脚。
 *
 * @param[in] p_epd       EPD Service structure. EPD服务结构
 */
void ble_epd_sleep_prepare(ble_epd_t* p_epd);

/**@brief Function for initializing the EPD Service.
 *        初始化EPD服务的函数
 *
 * @details Initializes the BLE service, loads configuration, and sets up the EPD driver.
 *          初始化BLE服务、加载配置并设置EPD驱动。
 *
 * @param[out] p_epd      EPD Service structure. This structure must be supplied
 *                        by the application. It is initialized by this function and will
 *                        later be used to identify this particular service instance.
 *                        EPD服务结构。此结构必须由应用程序提供,由本函数初始化,
 *                        之后用于标识此特定服务实例。
 *
 * @retval NRF_SUCCESS If the service was successfully initialized. Otherwise, an error code is returned.
 *                     如果服务初始化成功则返回NRF_SUCCESS,否则返回错误代码。
 * @retval NRF_ERROR_NULL If either of the pointers p_epd or p_epd_init is NULL.
 *                        如果p_epd或p_epd_init指针为NULL则返回此值。
 */
uint32_t ble_epd_init(ble_epd_t* p_epd);

/**@brief Function for handling the EPD Service's BLE events.
 *        处理EPD服务BLE事件的函数
 *
 * @details The EPD Service expects the application to call this function each time an
 * event is received from the S110 SoftDevice. This function processes the event if it
 * is relevant and calls the EPD Service event handler of the
 * application if necessary.
 * EPD服务期望应用程序在每次从S110 SoftDevice接收到事件时调用此函数。
 * 如果事件相关,此函数会处理该事件,必要时调用应用程序的EPD服务事件处理程序。
 *
 * @param[in] p_epd       EPD Service structure. EPD服务结构
 * @param[in] p_ble_evt   Event received from the S110 SoftDevice. 从S110 SoftDevice接收的事件
 */
void ble_epd_on_ble_evt(ble_epd_t* p_epd, ble_evt_t* p_ble_evt);

/**@brief Function for sending a string to the peer.
 *        向对端发送字符串的函数
 *
 * @details This function sends the input string as an RX characteristic notification to the peer.
 *          此函数将输入字符串作为RX特性通知发送给对端。
 *
 * @param[in] p_epd       Pointer to the EPD Service structure. EPD服务结构指针
 * @param[in] p_string    String to be sent. 要发送的字符串
 * @param[in] length      Length of the string. 字符串长度
 *
 * @retval NRF_SUCCESS If the string was sent successfully. Otherwise, an error code is returned.
 *                     如果字符串发送成功则返回NRF_SUCCESS,否则返回错误代码。
 */
uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length);

/**@brief Function for handling timer events and triggering GUI updates.
 *        处理定时器事件并触发GUI更新的函数
 *
 * @details Called periodically to update the display based on current mode:
 *          - MODE_CALENDAR: Updates daily at 00:00:00
 *          - MODE_CLOCK: Updates every minute
 *          - MODE_CLOCK_CALENDAR: Updates every minute
 *          定期调用以根据当前模式更新显示:
 *          - 日历模式: 每天00:00:00更新
 *          - 时钟模式: 每分钟更新
 *          - 时钟+日历模式: 每分钟更新
 *
 * @param[in] p_epd         Pointer to the EPD Service structure. EPD服务结构指针
 * @param[in] timestamp     Current Unix timestamp. 当前Unix时间戳
 * @param[in] force_update  If true, force update regardless of mode. 如果为true,则强制更新而不管模式
 */
void ble_epd_on_timer(ble_epd_t* p_epd, uint32_t timestamp, bool force_update);

#endif  // EPD_BLE_H__

/** @} */
