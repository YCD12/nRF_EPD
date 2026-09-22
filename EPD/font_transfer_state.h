#ifndef FONT_TRANSFER_STATE_H
#define FONT_TRANSFER_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifndef FONT_STORE_MUTABLE
#define FONT_STORE_MUTABLE 0
#endif

/* Font stream packets bypass the general EPD command queue.  The BLE write
 * event only copies packets into this small FIFO; Flash programming runs from
 * the scheduler so a burst of writeWithoutResponse packets cannot block BLE. */
#define FONT_STREAM_PACKET_OVERHEAD 10U /* op + seq:u16 + offset:u32 + crc16 */
#define FONT_STREAM_DATA_MAX_LEN (BLE_EPD_MAX_DATA_LEN - FONT_STREAM_PACKET_OVERHEAD)
#ifndef FONT_STREAM_FIFO_SIZE
#define FONT_STREAM_FIFO_SIZE 3U
#endif
#ifndef FONT_STREAM_ACK_EVERY
#define FONT_STREAM_ACK_EVERY FONT_STREAM_FIFO_SIZE
#endif
#define FONT_STORE_ACK_QUEUE_SIZE 8U

#if FONT_STREAM_DATA_MAX_LEN > 255U || FONT_STREAM_FIFO_SIZE < 1U || FONT_STREAM_FIFO_SIZE > 255U
#error "Font packet length and FIFO indices must fit uint8_t"
#endif

/* All external-font work is serialized through one scheduler task.  BLE
 * callbacks only set bits in font_task_pending and never run Flash/SPI work
 * directly when the app scheduler is full. */
#define FONT_TASK_CONTROL       0x01U
#define FONT_TASK_LIST          0x02U
#define FONT_TASK_OPERATION_ACK 0x04U
#define FONT_TASK_STREAM        0x08U
#define FONT_TASK_STREAM_ACK    0x10U
#define FONT_TASK_TIMEOUT_ABORT 0x20U

/**@brief 命令队列项 */
typedef struct {
    uint8_t cmd;                        /**< 命令 ID */
    uint8_t data[EPD_CMD_MAX_DATA_LEN]; /**< 命令数据（含命令字节） */
    uint16_t length;                    /**< 数据长度 */
} epd_cmd_queue_item_t;

#if FONT_STORE_MUTABLE

typedef struct {
    uint32_t offset;
    uint8_t length;
    uint8_t data[FONT_STREAM_DATA_MAX_LEN];
} font_stream_packet_t;

typedef struct {
    volatile bool active;
    volatile uint16_t next_sequence;
    volatile uint32_t next_offset;
    volatile uint32_t total_length;
    volatile uint8_t pending_ack_packets;
    volatile uint8_t error;
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile uint8_t count;
    volatile bool ack_pending;
    volatile bool force_ack_each_packet;
    volatile uint8_t ack_status;
    font_stream_packet_t packets[FONT_STREAM_FIFO_SIZE];
} font_stream_rx_t;

typedef struct {
    uint8_t operation;
    uint8_t status;
    uint8_t slot;
} font_store_ack_item_t;

typedef struct {
    volatile bool operation_pending;     /**< 字库命令已收到，阻止新的显示刷新 */
    volatile bool refresh_pending;      /**< 字库变更后等待合并刷新 */
    volatile uint32_t refresh_due;      /**< 合并刷新最早允许启动的 uptime 秒数 */
    volatile uint32_t last_activity_ts; /**< 字库事务最后活动时间，用于空闲超时 */
    volatile uint32_t last_begin_ts;    /**< 上一次 BEGIN 时间，用于更新频率限制 */
    volatile uint8_t task_pending;      /**< 待执行的字体任务位图 */
    volatile uint8_t task_wait_tx;      /**< 等待 BLE TX_COMPLETE 唤醒的任务位图 */
    volatile bool task_scheduled;       /**< 统一字体任务已入调度器 */
    volatile bool task_retry_pending;   /**< 调度器满，等待下一次主循环重试 */
    volatile bool abort_requested;      /**< BLE断开后等待调度器安全取消字体事务 */
    volatile bool list_pending;         /**< 字库列表通知尚未全部发送 */
    volatile uint8_t list_slot;         /**< 下一个待发送的字库槽位 */
    volatile uint8_t ack_head;
    volatile uint8_t ack_tail;
    volatile uint8_t ack_count;
    volatile bool ack_overflow_pending;
    bool ack_inflight_valid; /* Owned by scheduler; IRQ never replaces it. */
    font_store_ack_item_t ack_inflight;
    volatile bool list_restart;
    font_store_ack_item_t ack_overflow;
    font_store_ack_item_t ack_queue[FONT_STORE_ACK_QUEUE_SIZE];

    /* Mutating font control commands use a private scheduler slot instead
     * of competing with the two-slot display command queue. */
    volatile bool cmd_pending;
    epd_cmd_queue_item_t cmd;

    font_stream_rx_t stream;
} font_transfer_state_t;

#else

typedef struct { uint8_t unused; } font_transfer_state_t;

#endif

#endif
