/*
 * 主机端资源 HAL：把 epd_resources.bin 映射为与真机相同的 type/id/offset 读接口。
 * 供 tools/test/test_lunar_db.c 及后续 PC 渲染回归共用。
 */
#include "res_hal.h"

#include <stdio.h>
#include <string.h>

#include "res_hal_host.h"
#include "sdk_errors.h"

#define RES_HAL_IMG_BASE      0x1000UL
#define RES_HAL_IMG_END       0x7F000UL
#define RES_HAL_IMG_SIZE      (RES_HAL_IMG_END - RES_HAL_IMG_BASE)
#define RES_HAL_MANIFEST_ADDR 0x7E000UL
#define RES_HAL_ENTRY_STRIDE  16U
#define RES_HAL_ENTRY_MAX     24U

typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} res_hal_manifest_entry_t;

static uint8_t g_img[RES_HAL_IMG_SIZE];
static res_hal_manifest_entry_t g_entries[RES_HAL_ENTRY_MAX];
static uint16_t g_entry_count;
static bool g_loaded;

static uint16_t res_hal_rd_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t res_hal_rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const res_hal_manifest_entry_t* res_hal_find_entry(uint16_t type, uint16_t id) {
    for (uint16_t i = 0; i < g_entry_count; i++) {
        if (g_entries[i].type == type && g_entries[i].id == id) {
            return &g_entries[i];
        }
    }
    return NULL;
}

int res_hal_host_init(const char* bin_path) {
    FILE* f;
    size_t n;
    const uint8_t* manifest;
    uint32_t magic;
    const uint8_t* entry_ptr;

    g_loaded = false;
    g_entry_count = 0;

    f = fopen(bin_path, "rb");
    if (f == NULL) {
        return -1;
    }
    n = fread(g_img, 1, RES_HAL_IMG_SIZE, f);
    fclose(f);
    if (n < (RES_HAL_MANIFEST_ADDR - RES_HAL_IMG_BASE) + 8U) {
        return -1;
    }

    manifest = g_img + (RES_HAL_MANIFEST_ADDR - RES_HAL_IMG_BASE);
    magic = res_hal_rd_u32(manifest);
    if (magic != EPD_EXT_RES_MAGIC) {
        return -1;
    }

    g_entry_count = res_hal_rd_u16(manifest + 6);
    if (g_entry_count > RES_HAL_ENTRY_MAX) {
        g_entry_count = RES_HAL_ENTRY_MAX;
    }

    entry_ptr = manifest + 8;
    for (uint16_t i = 0; i < g_entry_count; i++) {
        g_entries[i].type = res_hal_rd_u16(entry_ptr + 0);
        g_entries[i].id = res_hal_rd_u16(entry_ptr + 2);
        g_entries[i].offset = res_hal_rd_u32(entry_ptr + 4);
        g_entries[i].length = res_hal_rd_u32(entry_ptr + 8);
        g_entries[i].crc32 = res_hal_rd_u32(entry_ptr + 12);
        entry_ptr += RES_HAL_ENTRY_STRIDE;
    }

    g_loaded = true;
    return 0;
}

void res_hal_host_shutdown(void) {
    g_loaded = false;
    g_entry_count = 0;
}

ret_code_t res_hal_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry) {
    const res_hal_manifest_entry_t* manifest_entry;

    if (!g_loaded) {
        return NRF_ERROR_INVALID_STATE;
    }

    manifest_entry = res_hal_find_entry(type, id);
    if (manifest_entry == NULL) {
        return NRF_ERROR_NOT_FOUND;
    }
    if (entry != NULL) {
        entry->type = manifest_entry->type;
        entry->id = manifest_entry->id;
        entry->offset = manifest_entry->offset;
        entry->length = manifest_entry->length;
        entry->crc32 = manifest_entry->crc32;
    }
    return NRF_SUCCESS;
}

ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len) {
    const res_hal_manifest_entry_t* manifest_entry;
    size_t avail;
    size_t n;

    if (!g_loaded) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (buf == NULL && len != 0) {
        return NRF_ERROR_NULL;
    }

    manifest_entry = res_hal_find_entry(type, id);
    if (manifest_entry == NULL) {
        return NRF_ERROR_NOT_FOUND;
    }
    if (offset >= manifest_entry->length) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    avail = manifest_entry->length - offset;
    n = (len < avail) ? len : avail;
    memcpy(buf, g_img + (manifest_entry->offset - RES_HAL_IMG_BASE) + offset, n);
    if (n < len) {
        memset((uint8_t*)buf + n, 0, len - n);
    }
    return NRF_SUCCESS;
}
