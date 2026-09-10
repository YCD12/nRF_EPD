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
 * @brief EPD BLE 服务接口与数据结构
 *
 * 包含：
 * - 命令队列（顺序执行）
 * - BLE 服务特性与 UUID
 * - 显示模式、刷新策略相关状态
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

/**@brief SoftDevice 观察者入口，转给 ble_epd_on_ble_evt。 */
void ble_epd_evt_handler(ble_evt_t const* p_ble_evt, void* p_context);

#define BLE_EPD_BLE_OBSERVER_PRIO 2  // BLE 事件观察者优先级
/**@brief 定义 BLE EPD 服务实例。
 *
 * @param   _name   实例名称
 * @hideinitializer
 */
#define BLE_EPD_DEF(_name)  \
    static ble_epd_t _name; \
    NRF_SDH_BLE_OBSERVER(_name##_obs, BLE_EPD_BLE_OBSERVER_PRIO, ble_epd_evt_handler, &_name)

#define APP_VERSION 0x2B  // 右上角与下方相同的 9 字节上限

// EPD 服务的 128 位基础 UUID（自定义服务）
#define BLE_UUID_EPD_SVC_BASE \
    {{0XEC, 0X5A, 0X67, 0X1C, 0XC1, 0XB6, 0X46, 0XFB, 0X8D, 0X91, 0X28, 0XD8, 0X22, 0X36, 0X75, 0X62}}
#define BLE_UUID_EPD_SVC 0x0001   // EPD 服务 UUID
#define BLE_UUID_EPD_CHAR 0x0002  // EPD 特性 UUID（命令和数据）
#define BLE_UUID_APP_VER 0x0003   // 应用版本特性 UUID

#define EPD_SVC_UUID_TYPE BLE_UUID_TYPE_VENDOR_BEGIN  // 自定义 UUID 类型

#define BLE_EPD_MAX_DATA_LEN (NRF_SDH_BLE_GATT_MAX_MTU_SIZE - 3)

/** EPD 服务命令 ID */
enum EPD_CMDS {
    // 基础硬件控制 (0x00-0x0F)
    EPD_CMD_SET_PINS = 0x00,     /**< 设置 EPD 引脚映射 */
    EPD_CMD_INIT = 0x01,         /**< 初始化 EPD 显示驱动 */
    EPD_CMD_CLEAR = 0x02,        /**< 清除屏幕 */
    EPD_CMD_SEND_COMMAND = 0x03, /**< 向 EPD 发送命令 */
    EPD_CMD_SEND_DATA = 0x04,    /**< 向 EPD 发送数据 */
    EPD_CMD_REFRESH = 0x05,      /**< 将显存刷新到屏幕 */
    EPD_CMD_SLEEP = 0x06,        /**< EPD 进入睡眠 */

    // 时间和显示模式 (0x20-0x2F)
    EPD_CMD_SET_TIME = 0x20,         /**< 用 Unix 时间戳设置时间 */
    EPD_CMD_SET_WEEK_START = 0x21,   /**< 设置周起始日（0:周日, 1:周一...） */
    EPD_CMD_SET_MODE = 0x22,         /**< 设置显示模式（0:图片, 1:日历, 2:时钟+日历, 3:全屏时钟） */
    EPD_CMD_SYNC_TIME_SILENT = 0x23, /**< 静默校时（不立即刷新） */
    EPD_CMD_SET_ANNOTATIONS = 0x24,  /**< 写入/清除日历日期短标签 */

    // 图像写入 (0x30-0x3F)
    EPD_CMD_WRITE_IMAGE = 0x30,    /**< 写入图像到 RAM（旧版，无 CRC） */
    EPD_CMD_WRITE_BLOCK = 0x31,    /**< 带 CRC16 的分块写入 */
    EPD_CMD_QUERY_STATUS = 0x32,   /**< 查询传输状态，用于断点续传 */
    EPD_CMD_RESET_TRANSFER = 0x33, /**< 重置传输状态 */

    // 系统配置和控制 (0x90-0x9F)
    // SYS_RESET / SYS_SLEEP / CFG_ERASE 为紧急命令；INIT / WRITE_BLOCK 同样紧急
    EPD_CMD_SET_CONFIG = 0x90, /**< 设置完整 EPD 配置 */
    EPD_CMD_SYS_RESET = 0x91,  /**< MCU 复位（紧急） */
    EPD_CMD_SYS_SLEEP = 0x92,  /**< MCU 进入睡眠（紧急） */
    EPD_CMD_CFG_ERASE = 0x99,  /**< 擦除配置并复位（紧急） */
};

/**
 * 响应类型（MCU -> APP）
 */
#define EPD_RSP_BLOCK_ACK    0xA0  /**< 块 ACK/NACK */
#define EPD_RSP_STATUS       0xA1  /**< 传输状态 */
#define EPD_RSP_ANNOTATIONS  0xA2  /**< 日历短标签分片 */

/**
 * 传输配置
 */
#define EPD_MAX_BLOCKS  512  /**< 最大块数（96KB / 192B） */
#define EPD_MAX_RETRIES 3    /**< 失败块最大重传次数 */

/**
 * 命令队列配置
 *
 * 作用：
 * 1. 防止命令并发导致驱动冲突（EPD 必须顺序执行）
 * 2. 缓冲 BLE 快速下发的多个命令
 * 3. 刷新等长操作期间仍可接收新命令
 */
#define EPD_CMD_QUEUE_SIZE 2                       /**< 普通命令队列容量（最多 2 条待处理） */
#define EPD_CMD_MAX_DATA_LEN BLE_EPD_MAX_DATA_LEN  /**< 单条命令最大数据长度 */

/**@brief 命令队列项 */
typedef struct {
    uint8_t cmd;                        /**< 命令 ID */
    uint8_t data[EPD_CMD_MAX_DATA_LEN]; /**< 命令数据（含命令字节） */
    uint16_t length;                    /**< 数据长度 */
} epd_cmd_queue_item_t;

/**@brief 命令队列（环形缓冲）
 *
 * - head：下一个取出位置
 * - tail：下一个插入位置
 * - count：当前条数
 * - is_processing：防止重复启动处理
 */
typedef struct {
    epd_cmd_queue_item_t items[EPD_CMD_QUEUE_SIZE]; /**< 队列项 */
    uint8_t head;                                   /**< 头索引（取出） */
    uint8_t tail;                                   /**< 尾索引（插入） */
    uint8_t count;                                  /**< 当前项数 */
    bool is_processing;                             /**< 是否正在处理 */
} epd_cmd_queue_t;

/**@brief 图像传输上下文（断点续传）
 *
 * - 每块 CRC 校验
 * - 顺序流：出现空洞则从 0 重传
 */
typedef struct {
    uint8_t  session_id;      /**< 会话 ID */
    uint16_t total_blocks;    /**< 总块数 */
    uint16_t received_blocks; /**< 已成功接收块数 */
    bool     transfer_active; /**< 是否有进行中的传输 */
} image_transfer_ctx_t;

/**@brief EPD 服务状态 */
typedef struct {
    // BLE 句柄
    uint16_t service_handle; /**< EPD 服务句柄（S112 SoftDevice） */
    ble_gatts_char_handles_t char_handles;     /**< EPD 特性句柄 */
    ble_gatts_char_handles_t app_ver_handles;  /**< APP 版本特性句柄 */

    // BLE 连接
    uint16_t conn_handle; /**< 当前连接句柄；未连接为 BLE_CONN_HANDLE_INVALID */
    uint16_t max_data_len;        /**< 可发给对端的最大数据长度（字节） */
    bool is_notification_enabled; /**< 对端是否已开启 RX 特性通知 */

    // EPD 硬件与配置
    epd_model_t* epd;    /**< 当前 EPD 型号 */
    epd_config_t config; /**< EPD 配置 */

    // 刷新控制
    bool force_full_refresh;        /**< 下次 GUI 更新强制全刷（模式切换，避免残影） */
    uint32_t last_full_clear;       /**< 上次全屏清除/刷新时间戳（秒）。0 表示从未全刷 */
    bool is_refreshing;             /**< 屏幕刷新进行中（防止 BLE 断开时中断刷新） */
    bool low_battery_screen_active; /**< 低电量保持：暂停周期刷新，仍显示月历/时钟 */

    // 刷新消抖与校时保护
    uint32_t last_refresh_start; /**< 上一次物理刷新开始时间戳（秒），用于消抖 */
    uint32_t last_set_time_ts;   /**< 上一次接受 SET_TIME 的时间戳（秒），用于频率限制 */

    // 命令队列
    epd_cmd_queue_t cmd_queue; /**< 顺序执行命令队列 */

    // 图像传输
    image_transfer_ctx_t transfer_ctx; /**< CRC 校验与断点续传上下文 */
} ble_epd_t;

/**@brief GUI 更新事件（经 app_scheduler 传递实例和时间戳） */
typedef struct {
    ble_epd_t* p_epd;   /**< EPD 服务指针 */
    uint32_t timestamp; /**< Unix 时间戳 */
} epd_gui_update_event_t;

#define EPD_GUI_SCHD_EVENT_DATA_SIZE sizeof(epd_gui_update_event_t)  /**< 调度器事件数据大小 */

/**@brief 进入睡眠前准备：关 LED、配置唤醒脚
 *
 * @param[in] p_epd  EPD 服务结构
 */
void ble_epd_sleep_prepare(ble_epd_t* p_epd);

/**@brief 初始化 EPD 服务（BLE、配置、驱动）
 *
 * @param[out] p_epd  由应用提供的服务结构，本函数初始化后用于标识该实例
 *
 * @retval NRF_SUCCESS     初始化成功
 * @retval NRF_ERROR_NULL  p_epd 为空
 */
uint32_t ble_epd_init(ble_epd_t* p_epd);

/**@brief 处理 EPD 服务的 BLE 事件
 *
 * 每次从 S112 SoftDevice 收到事件时由应用调用；相关事件在此处理。
 *
 * @param[in] p_epd      EPD 服务结构
 * @param[in] p_ble_evt  SoftDevice 事件
 */
void ble_epd_on_ble_evt(ble_epd_t* p_epd, ble_evt_t* p_ble_evt);

/**@brief 将对端字符串作为 RX 特性通知发出
 *
 * @param[in] p_epd     EPD 服务指针
 * @param[in] p_string  要发送的数据
 * @param[in] length    长度
 *
 * @retval NRF_SUCCESS  发送成功，否则为错误码
 */
uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length);

/**@brief 定时器回调：按模式触发 GUI 更新
 *
 * - 日历：每天 00:00:00
 * - 时钟 / 时钟+日历：每分钟
 *
 * @param[in] p_epd         EPD 服务指针
 * @param[in] timestamp     当前 Unix 时间戳
 * @param[in] force_update  为 true 时无视模式强制更新
 */
void ble_epd_on_timer(ble_epd_t* p_epd, uint32_t timestamp, bool force_update);

#endif  // EPD_BLE_H__

/** @} */
