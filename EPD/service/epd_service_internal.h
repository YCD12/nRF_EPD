/*!
 * @file epd_service_internal.h
 * @brief EPD service 模块间共享声明（不对外公开；外部只用 EPD_service.h）
 *
 * 拆分后的 service 层由 5 个 .c 组成：
 *   EPD_service.c            公开 API/事件路由/生命周期/主循环/INIT/连接清理/资源事件
 *   epd_service_display.c    显示请求/重试/全刷/时钟局刷/低电量守卫/GUI 数据
 *   epd_service_cmd.c        命令处理/配置校验与应用/命令队列
 *   epd_service_control.c    通知字符串/模式与显示 ACK/记录回执/配置回读
 *   epd_service_transfer.c   CRC16/块 ACK 与状态/WRITE_BLOCK(RLE)/QUERY/RESET/bulk reply
 *
 * 仅跨模块使用的函数在这里声明（定义处去掉 static）；模块内独占的 helper 保持 static。
 */

#ifndef EPD_SERVICE_INTERNAL_H
#define EPD_SERVICE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "EPD_service.h"

/* ---- EPD_service.c 与其他模块共享 ---- */
uint32_t ble_epd_notification_try_send(ble_epd_t* p_epd, uint8_t* data, uint16_t length);
bool epd_ensure_display(ble_epd_t *p_epd);
/* 面板 BUSY 错误状态：实例存在走驱动虚表，未实例化回退 HAL 全局标志。 */
void epd_service_busy_error_clear(ble_epd_t *p_epd);
bool epd_service_busy_error(const ble_epd_t *p_epd);
bool is_bulk_data_command(uint8_t cmd);
bool resource_blocks_display_command(uint8_t cmd);
/* 两个队列调度统一入口：拆分后调用点在多个 TU，若不 noinline 统一，
 * app_sched_event_put/epd_work_post 会被 LTO 按调用点克隆多份（实测 +200B）。 */
void epd_service_schedule_cmd(ble_epd_t *p_epd);
void epd_service_schedule_display(ble_epd_t *p_epd);

/* ---- epd_service_display.c ---- */
void epd_display_retry_reset(void);
bool epd_display_retry_scheduled(void);
bool epd_display_retry_waiting(void);
void epd_display_request(ble_epd_t *p_epd, uint32_t requested_timestamp);
void epd_gui_update(void* p_event_data, uint16_t event_size);
void epd_battery_process(ble_epd_t *p_epd);

/* ---- epd_service_cmd.c ---- */
bool epd_config_validate(const epd_config_t *cfg);
void epd_config_defaults(epd_config_t *cfg);
bool epd_config_apply(ble_epd_t *p_epd, epd_config_t *candidate);
bool epd_todo_font_valid(void);
/* 图片模式入口：prepare 在主循环完成可能阻塞的面板 init；只有它返回 true 且
 * 连接 generation/入口归属仍有效时，才允许 commit 提交 MODE_PICTURE。 */
bool epd_prepare_picture_mode(ble_epd_t *p_epd);
void epd_commit_picture_mode(ble_epd_t *p_epd);
bool epd_enter_picture_mode(ble_epd_t *p_epd);
void epd_execute_command(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length);
void epd_process_next_command(void* p_event_data, uint16_t event_size);
void annotations_store_applied(void);
void epd_annotations_set_service(ble_epd_t *p_epd);
calendar_annotations_tx_result_t annotations_tx_send(void *context, uint8_t *data,
                                                     uint16_t length);

/* ---- epd_service_control.c ---- */
void epd_send_time(ble_epd_t* p_epd);
void epd_send_voltage(ble_epd_t* p_epd);
void epd_send_mtu(ble_epd_t* p_epd);
void epd_command_ack(ble_epd_t *p_epd, const uint8_t *data, uint16_t length, uint8_t result);
uint32_t resource_notification_send(void *context, uint32_t generation,
                                    uint8_t *data, uint16_t length);
void epd_record_ack_process(ble_epd_t *p_epd);
void epd_config_readback_process(ble_epd_t *p_epd);
void epd_control_notifications_process(ble_epd_t *p_epd);

/* ---- epd_service_transfer.c ---- */
uint16_t crc16_compute(const uint8_t* data, uint16_t len);
void cmd_write_block(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length);
void cmd_query_status(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length);
void cmd_reset_transfer(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length);
void epd_bulk_reply_process(ble_epd_t *p_epd);

#endif /* EPD_SERVICE_INTERNAL_H */
