#ifndef EPD_CMD_POLICY_H
#define EPD_CMD_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include "epd_protocol.h"

/* 命令 ID 统一定义在纯数据头 epd_protocol.h，主机测试无需引入 BLE 协议栈。 */

/* 紧急命令绕过队列；INIT 入口只登记请求，实际初始化由主循环执行。 */
static inline bool epd_cmd_is_urgent(uint8_t cmd)
{
    return cmd == EPD_CMD_INIT ||
           cmd == EPD_CMD_SET_ANNOTATIONS ||
           cmd == EPD_CMD_WRITE_BLOCK ||
           cmd == EPD_CMD_QUERY_STATUS ||
           cmd == EPD_CMD_RESET_TRANSFER ||
           cmd == EPD_CMD_SYS_RESET ||
           cmd == EPD_CMD_SYS_SLEEP ||
           cmd == EPD_CMD_CFG_ERASE;
}

/* 仅改配置。不得占用 2 槽显示队列：首次切模式会连发 WEEK_START + SILENT + SET_MODE，
 * 否则 SET_MODE 会被挤掉。 */
static inline bool epd_cmd_is_immediate(uint8_t cmd)
{
    return cmd == EPD_CMD_SET_WEEK_START;
}

/* 切模式时占用的队列槽数（静默校时 + SET_MODE） */
static inline unsigned epd_cmd_mode_switch_queued_count(bool week_changed)
{
    (void)week_changed;
    return 2U; /* SYNC_TIME_SILENT + SET_MODE */
}

/* 需要点亮面板、走显示通路的命令 */
static inline bool epd_cmd_needs_display(uint8_t cmd)
{
    return cmd == EPD_CMD_CLEAR ||
           cmd == EPD_CMD_REFRESH ||
           cmd == EPD_CMD_WRITE_BLOCK;
}

typedef enum {
    EPD_BLOCK_WRITE = 0, /* 写入新块 */
    EPD_BLOCK_DUP,       /* 已收过，回 ACK */
    EPD_BLOCK_REJECT     /* 空洞或未开始，须从 0 重来 */
} epd_block_accept_t;

#define EPD_BLOCK_STATUS_OK 0x00U      /* ACK */
#define EPD_BLOCK_STATUS_CRC 0x01U     /* CRC 错 */
#define EPD_BLOCK_STATUS_INVALID 0x02U /* 块号/总数非法 */
#define EPD_BLOCK_STATUS_RESTART 0x03U /* 请从块 0 重传 */
#define EPD_BLOCK_MAX_COUNT 512U
#define EPD_TRANSFER_BITMAP_BYTES 0U   /* 顺序流，不用位图记缺块 */

static inline bool epd_transfer_status_complete(uint16_t received, uint16_t total)
{
    return total > 0U && received == total;
}

/* 顺序流：已收到块 0 .. received-1。 */
static inline uint16_t epd_transfer_missing_from(uint16_t received, uint16_t total)
{
    if (epd_transfer_status_complete(received, total)) {
        return total;
    }
    return received;
}

static inline bool epd_block_total_valid(uint16_t total_blocks) /* 1..512 */
{
    return total_blocks > 0U && total_blocks <= EPD_BLOCK_MAX_COUNT;
}

/* payload=0 无效；800x480/8=48000 时 payload 须 >= 94 才能落在 512 块内。 */
static inline bool epd_block_plane_fits(uint32_t plane_bytes, uint16_t payload)
{
    uint32_t blocks;

    if (payload == 0U) {
        return false;
    }
    blocks = (plane_bytes + (uint32_t)payload - 1U) / (uint32_t)payload;
    return blocks > 0U && blocks <= EPD_BLOCK_MAX_COUNT;
}

/* WRITE_BLOCK 是顺序 RAM 流。出现空洞必须从 0 重传。 */
static inline epd_block_accept_t epd_block_accept(uint16_t block_id,
                                                  uint16_t received_blocks,
                                                  bool transfer_active)
{
    if (!transfer_active || block_id == 0U) {
        return (block_id == 0U) ? EPD_BLOCK_WRITE : EPD_BLOCK_REJECT;
    }
    if (block_id == received_blocks) {
        return EPD_BLOCK_WRITE;
    }
    if (block_id < received_blocks) {
        return EPD_BLOCK_DUP;
    }
    return EPD_BLOCK_REJECT;
}

typedef struct {
    bool apply_time; /* 未触发频率限制则写时间并刷新 */
    bool apply_mode; /* 载荷带合法模式则改模式 */
    uint8_t mode;
} epd_set_time_action_t;

/* 0–3：图片 / 日历 / 分屏时钟 / 全屏时钟。 */
static inline bool epd_display_mode_valid(uint8_t mode)
{
    return mode <= 3U; /* MODE_CLOCK */
}

/* SET_TIME：限频只挡时间，不挡模式 */
static inline epd_set_time_action_t epd_set_time_action(bool rate_limited,
                                                        bool has_mode,
                                                        uint8_t mode)
{
    epd_set_time_action_t a = {false, false, 0};

    if (!rate_limited) {
        a.apply_time = true;
    }
    if (has_mode && epd_display_mode_valid(mode)) {
        a.apply_mode = true;
        a.mode = mode;
    }
    return a;
}

#endif
