#ifndef RES_HAL_H
#define RES_HAL_H

#include <stddef.h>
#include <stdint.h>

#include "epd_ext_resources.h"
#include "sdk_errors.h"

/**
 * L0 资源读取 HAL（优化 15）
 * ------------------------------------------------------------------
 * GUI / Lunar / 字模渲染等上层只依赖本接口读外置资源，不直接绑 SPI Flash。
 *   - nRF 真机：res_hal.c 转发 epd_ext_resource_read/get_info
 *   - PC 主机：res_hal_host.c 从 epd_resources.bin 镜像 fread
 */

ret_code_t res_hal_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry);
ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len);

#endif
