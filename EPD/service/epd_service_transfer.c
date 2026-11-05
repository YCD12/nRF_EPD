/*!
 * @file epd_service_transfer.c
 * @brief EPD 服务的图片分块传输：CRC16、块 ACK/状态、WRITE_BLOCK(RLE)、查询/复位
 *
 * 由 EPD/service/EPD_service.c 按职责拆出；跨模块接口声明见
 * epd_service_internal.h。只搬代码，不改行为/协议/时序。
 */

#include "EPD_service.h"
#include "epd_service_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "main.h"
#include "nrf_log.h"
#include "epd_clock_refresh.h"
#include "epd_cmd_policy.h"
#include "epd_refresh_policy.h"
#include "epd_rle.h"

/* RLE 块解压输出缓冲。cmd_write_block 运行在 SoftDevice 事件中断（ISR）里：
 * 236 B 放静态区而不是栈（中断栈余量小）；ISR 内写块单线程顺序执行、
 * 不会重入，主循环也不触碰该缓冲。 */
static uint8_t s_rle_out[EPD_RLE_MAX_BLOCK];

// ============================================================================
// CRC16计算和传输响应函数
// ============================================================================

/**@brief 计算CRC16-CCITT校验值
 *
 * @details 使用多项式0x8408 (反转的0x1021)
 * 用于验证BLE传输数据的完整性
 *
 * @param[in] data  数据指针
 * @param[in] len  数据长度
 *
 * @return CRC16校验值
 */
uint16_t crc16_compute(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return crc;
}

/**@brief 发送 0xA0/0xA1 实时应答，HVX 满时留一份最新副本给主循环重发
 *
 * @details 块 ACK 与状态查询是请求-应答类通知：SoftDevice 的 HVX 队列满时
 * 返回 NRF_ERROR_RESOURCES/NRF_ERROR_BUSY，此时不能丢应答，只保留最新一条
 * （latest wins：重复的 ACK/状态可容忍）交给 epd_control_notifications_process
 * 在主循环（含 HVN_TX_COMPLETE 唤醒后的下一轮）补发。其它错误直接丢弃：
 * 断连/参数错误重发也不会成功，而 on_disconnect/CCCD 关闭会清 pending。
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] data   应答数据（含命令字节，最大 7B）
 * @param[in] length 应答长度
 */
static void epd_bulk_reply_send(ble_epd_t* p_epd, uint8_t* data, uint16_t length) {
    if (length > sizeof(p_epd->bulk_reply)) return;
    uint32_t err = ble_epd_notification_try_send(p_epd, data, length);
    if (err == NRF_ERROR_RESOURCES || err == NRF_ERROR_BUSY) {
        CRITICAL_REGION_ENTER();
        memcpy(p_epd->bulk_reply, data, length);
        p_epd->bulk_reply_len = (uint8_t)length;
        p_epd->bulk_reply_pending = true;
        CRITICAL_REGION_EXIT();
    } else if (err == NRF_SUCCESS && p_epd->bulk_reply_pending) {
        /* 本次已入 HVX 队列，旧的待发副本已被更新，作废。 */
        CRITICAL_REGION_ENTER();
        p_epd->bulk_reply_pending = false;
        CRITICAL_REGION_EXIT();
    }
}

/**@brief 发送块响应(ACK/NACK)到APP
 *
 * @details 响应格式: [0xA0, block_id_L, block_id_H, status]
 * status: 0x00 = 成功(ACK), 0x01 = 失败(NACK,需重传)
 *
 * @param[in] p_epd  EPD服务结构指针
 * @param[in] block_id  块ID
 * @param[in] status  状态码(0x00=成功, 0x01=CRC错误)
 */
static void send_block_response(ble_epd_t* p_epd, uint16_t block_id, uint8_t status) {
    uint8_t rsp[] = {EPD_RSP_BLOCK_ACK, block_id & 0xFF, block_id >> 8, status};
    epd_bulk_reply_send(p_epd, rsp, sizeof(rsp));
}

/**@brief 发送传输状态响应到APP
 *
 * @details 响应格式: [0xA1, total_L, total_H, received_L, received_H, session_id, active]
 *
 * @param[in] p_epd  EPD服务结构指针
 */
static void send_transfer_status(ble_epd_t* p_epd) {
    /* [0xA1, total_L, total_H, received_L, received_H, session_id, active] */
    uint8_t rsp[7];
    rsp[0] = EPD_RSP_STATUS;
    rsp[1] = p_epd->transfer_ctx.total_blocks & 0xFF;
    rsp[2] = p_epd->transfer_ctx.total_blocks >> 8;
    rsp[3] = p_epd->transfer_ctx.received_blocks & 0xFF;
    rsp[4] = p_epd->transfer_ctx.received_blocks >> 8;
    rsp[5] = p_epd->transfer_ctx.session_id;
    rsp[6] = p_epd->transfer_ctx.transfer_active ? 1 : 0;

    epd_bulk_reply_send(p_epd, rsp, sizeof(rsp));
    NRF_LOG_INFO("[EPD]: Transfer status sent: total=%d, received=%d, session=%d, active=%d\n",
                 p_epd->transfer_ctx.total_blocks, p_epd->transfer_ctx.received_blocks,
                 p_epd->transfer_ctx.session_id, p_epd->transfer_ctx.transfer_active);
}

void cmd_write_block(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* CRC 分块顺序写入 */
{
    uint16_t block_id;

    if (length < 8) return;
    block_id = p_data[1] | (p_data[2] << 8);
    if (epd_picture_write_blocked(p_epd->is_refreshing) ||
        p_epd->disconnect_pending || !p_epd->gpio_connection_ref || p_epd->cmd_queue.is_processing ||
        p_epd->init_pending || p_epd->init_running || p_epd->init_ack_pending) {
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
        return;
    }
    /* S-1：本函数运行在 SoftDevice 事件中断里。面板尚未初始化、或仍处于
     * 时钟模式时，首次进入图片模式必须由主循环完成（UC8179 复位 30 ms +
     * 最长 1000 ms BUSY，还会二次 init），这里只登记请求并回 RESTART，
     * 让网页已有的“从块 0 重传”兜底；中断内绝不调用 epd_init/驱动 init。 */
    if (p_epd->picture_entry_pending) {
        /* 入口尚未执行：任何块（含重发的块 0）都让网页继续等/重传。 */
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
        return;
    }
    if (p_epd->epd == NULL ||
        epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
        /* 块 0 才登记入口。注意 RESET_TRANSFER 会先把 transfer_active 置 1 而
         * display_mode 仍为时钟模式（这是网页每次传图的正常起点），所以这里
         * 不能要求“无进行中传输”，否则 block 0 永远回 RESTART、反复重传。 */
        if (block_id == 0U) {
            p_epd->picture_entry_pending = true;
        }
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
        return;
    }
    if (!epd_ensure_display(p_epd)) return;
    /* total 字段 bit15 = 本块 payload 为 RLE 压缩流（网页只在压缩更小时置位），
     * 低 15 位是真实块数；0xA1 状态必须回未置标志的真实块数。 */
    uint16_t total_field  = p_data[3] | (p_data[4] << 8);
    bool     rle          = (total_field & 0x8000U) != 0U;
    uint16_t total_blocks = total_field & 0x7FFFU;
    uint8_t  cfg         = p_data[5];
    uint16_t payload_len = length - 8;
    uint8_t *payload     = &p_data[6];
    uint16_t recv_crc    = p_data[length - 2] | (p_data[length - 1] << 8);
    uint16_t calc_crc    = crc16_compute(payload, payload_len);

    if (calc_crc == recv_crc) {
        p_epd->image_last_activity = app_uptime_seconds();
        if (block_id >= EPD_MAX_BLOCKS || block_id >= total_blocks) {
            NRF_LOG_WARNING("[EPD]: Block %d invalid (max=%d, total=%d)\n",
                            block_id, EPD_MAX_BLOCKS, total_blocks);
            send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_INVALID);
            return;
        }
        if (!epd_block_total_valid(total_blocks)) {
            NRF_LOG_WARNING("[EPD]: Invalid total_blocks=%d\n", total_blocks);
            send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_INVALID);
            return;
        }
        {
            epd_block_accept_t accept = epd_block_accept(
                block_id, p_epd->transfer_ctx.received_blocks,
                p_epd->transfer_ctx.transfer_active);

            if (accept == EPD_BLOCK_REJECT) {
                uint8_t session = p_epd->transfer_ctx.session_id;
                NRF_LOG_WARNING("[EPD]: Block %d gap (have %d), restart from 0\n",
                                block_id, p_epd->transfer_ctx.received_blocks);
                memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
                p_epd->transfer_ctx.session_id = session;
                p_epd->transfer_ctx.transfer_active = true;
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
                return;
            }

            if (block_id == 0 || !p_epd->transfer_ctx.transfer_active) {
                p_epd->transfer_ctx.total_blocks    = total_blocks;
                p_epd->transfer_ctx.received_blocks = 0;
                p_epd->transfer_ctx.transfer_active = true;
                if (epd_is_clock_mode((display_mode_t)p_epd->config.display_mode)) {
                    NRF_LOG_INFO("Switch mode: block\n");
                }
                EPD_ClearBusyAbort();
                epd_enter_picture_mode(p_epd);
            } else if (p_epd->transfer_ctx.total_blocks != total_blocks) {
                NRF_LOG_WARNING("[EPD]: total_blocks changed mid-transfer: %d -> %d, resetting\n",
                                p_epd->transfer_ctx.total_blocks, total_blocks);
                memset(&p_epd->transfer_ctx, 0, sizeof(p_epd->transfer_ctx));
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_RESTART);
                return;
            }

            if (accept == EPD_BLOCK_DUP) {
                send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_OK);
                return;
            }

            /* RLE 块：CRC 仍按收到的压缩 payload 校验；写面板前解压到静态缓冲。
             * 码流不完整/越界按坏块处理：回 CRC 状态，不写 RAM、不计已收块，
             * 网页会用同一块号重传。 */
            uint16_t write_len = payload_len;
            const uint8_t *write_data = payload;
            if (rle) {
                if (!epd_rle_decompress(payload, payload_len, s_rle_out,
                                        sizeof(s_rle_out), &write_len)) {
                    NRF_LOG_WARNING("[EPD]: Block %d RLE decode failed\n", block_id);
                    send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_CRC);
                    return;
                }
                write_data = s_rle_out;
            }

            p_epd->epd->drv->write_ram(p_epd->epd, cfg, (uint8_t *)write_data,
                                       (uint8_t)write_len);
            p_epd->transfer_ctx.received_blocks++;
            if (block_id == 0 || block_id + 1U == total_blocks) {
                NRF_LOG_INFO("[LEGACY] block=%u/%u cfg=0x%02x rle=%u CRC OK",
                             block_id, total_blocks, cfg, rle ? 1U : 0U);
            }
        }
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_OK);
    } else {
        NRF_LOG_WARNING("[EPD]: Block %d CRC mismatch! recv=0x%04X, calc=0x%04X\n",
                        block_id, recv_crc, calc_crc);
        send_block_response(p_epd, block_id, EPD_BLOCK_STATUS_CRC);
    }
}

void cmd_query_status(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 回传传输进度 */
{
    (void)p_data; (void)length;
    send_transfer_status(p_epd);
}

void cmd_reset_transfer(ble_epd_t *p_epd, uint8_t *p_data, uint16_t length) /* 清传输状态并打断时钟忙等 */
{
    if (p_epd->disconnect_pending) return;
    p_epd->image_last_activity = app_uptime_seconds();
    memset(&p_epd->transfer_ctx, 0, sizeof(image_transfer_ctx_t));
    if (length > 1) {
        p_epd->transfer_ctx.session_id = p_data[1];
    }
    /* 卡住时钟节拍但不改 display_mode。首块 WRITE_BLOCK 仍视为时钟模式，
     * 走全刷 Init，从而退出局刷窗口。 */
    p_epd->transfer_ctx.transfer_active = true;
    EPD_RequestBusyAbort();
    epd_clock_refresh_invalidate();
    NRF_LOG_INFO("[EPD]: Transfer state reset, session_id=%d\n", p_epd->transfer_ctx.session_id);
}

/* S-3：0xA0/0xA1 在 HVX 满时由中断存入单槽，主循环每轮（含
 * HVN_TX_COMPLETE 唤醒后的下一轮）补发一次；成功且槽未被更新才清 pending，
 * 避免与中断里刚写入的更新副本竞争。由 ble_epd_process 紧跟
 * epd_control_notifications_process 调用，位于 initial/mode_ack 重试之后，
 * 不抢占既有关键通知的重试机会。 */
void epd_bulk_reply_process(ble_epd_t *p_epd)
{
    if (!p_epd->bulk_reply_pending) return;
    if (!p_epd->is_notification_enabled || p_epd->disconnect_pending ||
        p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) {
        p_epd->bulk_reply_pending = false;
        return;
    }
    uint8_t bulk[sizeof(p_epd->bulk_reply)];
    uint8_t bulk_len;
    CRITICAL_REGION_ENTER();
    bulk_len = p_epd->bulk_reply_len;
    if (bulk_len > (uint8_t)sizeof(bulk)) bulk_len = 0U;
    else if (bulk_len > 0U) memcpy(bulk, p_epd->bulk_reply, bulk_len);
    CRITICAL_REGION_EXIT();
    if (bulk_len > 0U &&
        ble_epd_notification_try_send(p_epd, bulk, bulk_len) == NRF_SUCCESS) {
        CRITICAL_REGION_ENTER();
        if (bulk_len == p_epd->bulk_reply_len &&
            memcmp(bulk, p_epd->bulk_reply, bulk_len) == 0)
            p_epd->bulk_reply_pending = false;
        CRITICAL_REGION_EXIT();
    }
}
