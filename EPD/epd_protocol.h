#ifndef EPD_PROTOCOL_H
#define EPD_PROTOCOL_H

enum EPD_CMDS {
    EPD_CMD_SET_PINS = 0x00,
    EPD_CMD_INIT = 0x01,
    EPD_CMD_CLEAR = 0x02,
    EPD_CMD_REFRESH = 0x03,
    EPD_CMD_SLEEP = 0x04,
    EPD_CMD_SET_TIME = 0x20,
    EPD_CMD_SET_WEEK_START = 0x21,
    EPD_CMD_SET_MODE = 0x22,
    EPD_CMD_SYNC_TIME_SILENT = 0x23,
    EPD_CMD_SET_ANNOTATIONS = 0x24,
    EPD_CMD_RESOURCE = 0x26,
    EPD_CMD_WRITE_BLOCK = 0x30,
    EPD_CMD_QUERY_STATUS = 0x31,
    EPD_CMD_RESET_TRANSFER = 0x32,
    EPD_CMD_SET_CONFIG = 0x90,
    EPD_CMD_SYS_RESET = 0x91,
    EPD_CMD_SYS_SLEEP = 0x92,
    EPD_CMD_GET_CONFIG = 0x93,
    EPD_CMD_CFG_ERASE = 0x99,
};

#define EPD_RSP_BLOCK_ACK       0xA0U
#define EPD_RSP_STATUS          0xA1U
#define EPD_RSP_ANNOTATIONS     0xA2U
#define EPD_RSP_INIT            0xA6U /* token, status (0=ready, 1=failed), MTU uint16 LE */
#define EPD_RSP_RESOURCE        0xA7U
#define EPD_RSP_RECORD_COMMIT   0xA8U /* partition, result, reserved, revision, data CRC */
#define EPD_RSP_CONFIG          0xA9U /* token, result, epd_config_t (14 bytes) */

#endif
