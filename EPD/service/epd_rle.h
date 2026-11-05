/**
 * @file epd_rle.h
 * @brief 传图块的自包含 RLE 解码（与网页 html/js/rle.js 编码格式一致）
 *
 * 格式（每个 WRITE_BLOCK 块独立解码，不跨块）：
 * - repeat run：control = 0x80 | (count - 3)，count 3..130，随后 1 字节 value；
 * - literal run：control = count - 1，count 1..128，随后 count 字节原文。
 *
 * 网页仅在压缩后长度更小时才对该块启用 RLE，并把块头 total 字段最高位
 * （bit15）置 1；固件解压后按原 cfg（begin/continue、层选择）写面板 RAM。
 */

#ifndef EPD_RLE_H
#define EPD_RLE_H

#include <stdbool.h>
#include <stdint.h>

/** 单块解压输出上限：ATT MTU 247 - 3 - 8 字节协议头 = 236 B。 */
#define EPD_RLE_MAX_BLOCK 236U

/**
 * @brief 解压一个自包含 RLE 块。
 *
 * @param[in]  src          压缩数据
 * @param[in]  src_len      压缩数据长度
 * @param[out] dst          输出缓冲
 * @param[in]  dst_capacity 输出缓冲容量
 * @param[out] out_len      实际解压长度
 *
 * @retval true  成功
 * @retval false 码流不完整/非法，或输出超过容量（调用方应视为坏块）
 */
bool epd_rle_decompress(const uint8_t *src, uint16_t src_len, uint8_t *dst,
                        uint16_t dst_capacity, uint16_t *out_len);

#endif /* EPD_RLE_H */
