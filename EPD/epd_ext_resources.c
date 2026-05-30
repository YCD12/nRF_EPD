#include "epd_ext_resources.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "nrf_log.h"

#define EPD_EXT_RES_CRC_CHUNK_SIZE 64U

static epd_ext_resource_manifest_t m_manifest;
static bool m_flash_ready;
static bool m_manifest_valid;
static fm25q04b_jedec_id_t m_jedec_id;

static ret_code_t epd_ext_resources_reload(void);

static uint32_t epd_ext_resource_used_bytes(void) {
    uint32_t used_end = EPD_EXT_RES_DATA_START_ADDR;

    if (!m_manifest_valid) {
        return 0;
    }

    for (uint16_t i = 0; i < m_manifest.resource_count; i++) {
        uint32_t entry_end = m_manifest.entries[i].offset + m_manifest.entries[i].length;
        if (entry_end > used_end) {
            used_end = entry_end;
        }
    }

    return (used_end > EPD_EXT_RES_DATA_START_ADDR) ? (used_end - EPD_EXT_RES_DATA_START_ADDR) : 0;
}

static uint32_t epd_ext_flash_total_bytes(void) {
    if (m_jedec_id.capacity < 31U && m_jedec_id.capacity >= 16U) {
        return 1UL << m_jedec_id.capacity;
    }
    return FM25Q04B_SIZE_BYTES;
}

static bool epd_ext_range_is_writable(uint32_t address, size_t length) {
    if (length == 0) {
        return true;
    }
    if (address < EPD_EXT_RES_DATA_START_ADDR) {
        return false;
    }
    if (address >= EPD_EXT_RES_RESERVED_START_ADDR) {
        return false;
    }
    return length <= (size_t)(EPD_EXT_RES_RESERVED_START_ADDR - address);
}

static bool epd_ext_resource_range_is_valid(uint32_t address, size_t length) {
    if (length == 0) {
        return true;
    }
    if (address < EPD_EXT_RES_DATA_START_ADDR || address > EPD_EXT_RES_DATA_END_ADDR) {
        return false;
    }
    return length <= (size_t)(EPD_EXT_RES_DATA_END_ADDR + 1UL - address);
}

static uint32_t epd_ext_crc32_init(void) {
    return 0xFFFFFFFFUL;
}

static uint32_t epd_ext_crc32_update(uint32_t crc, const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;

    while (length-- > 0) {
        crc ^= *bytes++;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 1UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }

    return crc;
}

static uint32_t epd_ext_crc32_finalize(uint32_t crc) {
    return ~crc;
}

static ret_code_t epd_ext_crc32_flash(uint32_t address, size_t length, uint32_t* crc_out) {
    uint8_t buffer[EPD_EXT_RES_CRC_CHUNK_SIZE];
    uint32_t crc = epd_ext_crc32_init();
    ret_code_t ret;

    if (crc_out == NULL) {
        return NRF_ERROR_NULL;
    }

    while (length > 0) {
        size_t chunk = (length > sizeof(buffer)) ? sizeof(buffer) : length;

        ret = fm25q04b_read(address, buffer, chunk);
        if (ret != NRF_SUCCESS) {
            return ret;
        }

        crc = epd_ext_crc32_update(crc, buffer, chunk);
        address += (uint32_t)chunk;
        length -= chunk;
        app_feed_wdt();
    }

    *crc_out = epd_ext_crc32_finalize(crc);
    return NRF_SUCCESS;
}

static ret_code_t epd_ext_validate_manifest(epd_ext_resource_manifest_t* manifest) {
    uint32_t stored_crc;
    uint32_t computed_crc;

    if (manifest == NULL) {
        return NRF_ERROR_NULL;
    }
    if (manifest->magic != EPD_EXT_RES_MAGIC || manifest->version != EPD_EXT_RES_VERSION) {
        return NRF_ERROR_INVALID_DATA;
    }
    if (manifest->resource_count > EPD_EXT_RES_MAX_ENTRIES) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    stored_crc = manifest->crc32;
    manifest->crc32 = 0;
    computed_crc = epd_ext_crc32_finalize(
        epd_ext_crc32_update(epd_ext_crc32_init(), manifest, sizeof(*manifest)));
    manifest->crc32 = stored_crc;
    if (computed_crc != stored_crc) {
        return NRF_ERROR_INVALID_DATA;
    }

    for (uint16_t i = 0; i < manifest->resource_count; i++) {
        const epd_ext_resource_entry_t* entry = &manifest->entries[i];
        uint32_t resource_crc = 0;
        ret_code_t ret;

        if (!epd_ext_resource_range_is_valid(entry->offset, entry->length)) {
            return NRF_ERROR_INVALID_ADDR;
        }

        ret = epd_ext_crc32_flash(entry->offset, entry->length, &resource_crc);
        if (ret != NRF_SUCCESS) {
            return ret;
        }
        if (resource_crc != entry->crc32) {
            NRF_LOG_WARNING("External resource CRC mismatch type=%u id=%u", entry->type, entry->id);
            return NRF_ERROR_INVALID_DATA;
        }
        app_feed_wdt();
    }

    return NRF_SUCCESS;
}

ret_code_t epd_ext_resources_init(void) {
    ret_code_t ret;

    if (!m_flash_ready) {
        ret = fm25q04b_init();
        if (ret != NRF_SUCCESS) {
            return ret;
        }

        ret = fm25q04b_probe(&m_jedec_id);
        if (ret != NRF_SUCCESS) {
            return ret;
        }
        m_flash_ready = true;
    }

    return epd_ext_resources_reload();
}

ret_code_t epd_ext_resources_wake(void) {
    if (!m_flash_ready) {
        return epd_ext_resources_init();
    }
    return fm25q04b_wake();
}

ret_code_t epd_ext_resources_sleep(void) {
    if (!m_flash_ready) {
        return NRF_SUCCESS;
    }
    return fm25q04b_sleep();
}

static ret_code_t epd_ext_resources_reload(void) {
    ret_code_t ret;

    if (!m_flash_ready) {
        return NRF_ERROR_INVALID_STATE;
    }

    ret = fm25q04b_read(EPD_EXT_RES_MAIN_MANIFEST_ADDR, &m_manifest, sizeof(m_manifest));
    if (ret != NRF_SUCCESS) {
        m_manifest_valid = false;
        return ret;
    }

    ret = epd_ext_validate_manifest(&m_manifest);
    m_manifest_valid = (ret == NRF_SUCCESS);
    return ret;
}

ret_code_t epd_ext_resources_get_status(epd_ext_resource_status_t* status) {
    if (status == NULL) {
        return NRF_ERROR_NULL;
    }

    status->flash_ready = m_flash_ready;
    status->manifest_valid = m_manifest_valid;
    status->resource_count = m_manifest_valid ? m_manifest.resource_count : 0;
    status->manifest_crc32 = m_manifest_valid ? m_manifest.crc32 : 0;
    status->flash_total_bytes = epd_ext_flash_total_bytes();
    status->resource_area_bytes = EPD_EXT_RES_DATA_END_ADDR + 1UL - EPD_EXT_RES_DATA_START_ADDR;
    status->resource_used_bytes = epd_ext_resource_used_bytes();
    status->resource_free_bytes = status->resource_area_bytes - status->resource_used_bytes;
    status->jedec_id = m_jedec_id;
    return NRF_SUCCESS;
}

ret_code_t epd_ext_resource_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry) {
    if (entry == NULL) {
        return NRF_ERROR_NULL;
    }
    if (!m_manifest_valid) {
        return NRF_ERROR_INVALID_STATE;
    }

    for (uint16_t i = 0; i < m_manifest.resource_count; i++) {
        if (m_manifest.entries[i].type == type && m_manifest.entries[i].id == id) {
            *entry = m_manifest.entries[i];
            return NRF_SUCCESS;
        }
    }

    return NRF_ERROR_NOT_FOUND;
}

ret_code_t epd_ext_resource_read(uint16_t type, uint16_t id, uint32_t offset, void* data, size_t length) {
    epd_ext_resource_entry_t entry;
    ret_code_t ret;

    if (data == NULL && length != 0) {
        return NRF_ERROR_NULL;
    }

    ret = epd_ext_resource_get_info(type, id, &entry);
    if (ret != NRF_SUCCESS) {
        return ret;
    }
    if (offset > entry.length || length > (size_t)(entry.length - offset)) {
        return NRF_ERROR_INVALID_LENGTH;
    }

    return fm25q04b_read(entry.offset + offset, data, length);
}

ret_code_t epd_ext_resource_read_raw(uint32_t address, void* data, size_t length) {
    if (!m_flash_ready) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (data == NULL && length != 0) {
        return NRF_ERROR_NULL;
    }
    if (length != 0 && (address >= EPD_EXT_RES_RESERVED_START_ADDR ||
        length > (size_t)(EPD_EXT_RES_RESERVED_START_ADDR - address))) {
        return NRF_ERROR_INVALID_ADDR;
    }

    return fm25q04b_read(address, data, length);
}

ret_code_t epd_ext_resource_erase(uint32_t address, size_t length) {
    ret_code_t ret;

    if (!m_flash_ready) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (address == 0 && length == FM25Q04B_SIZE_BYTES) {
        for (uint32_t sector = 0; sector < FM25Q04B_SIZE_BYTES; sector += FM25Q04B_SECTOR_SIZE) {
            ret = fm25q04b_erase_sector(sector);
            app_feed_wdt();
            if (ret != NRF_SUCCESS) {
                return ret;
            }
        }
        memset(&m_manifest, 0, sizeof(m_manifest));
        m_manifest_valid = false;
        NRF_LOG_INFO("External resource flash fully erased");
        return NRF_SUCCESS;
    }
    if (!epd_ext_range_is_writable(address, length)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    ret = fm25q04b_erase_range(address, length);
    if (ret == NRF_SUCCESS && address <= EPD_EXT_RES_STAGING_MANIFEST_ADDR &&
        address + length > EPD_EXT_RES_STAGING_MANIFEST_ADDR) {
        memset(&m_manifest, 0, sizeof(m_manifest));
        m_manifest_valid = false;
        NRF_LOG_INFO("External resource manifest erased");
    }
    return ret;
}

ret_code_t epd_ext_resource_write(uint32_t address, const void* data, size_t length) {
    if (!m_flash_ready) {
        return NRF_ERROR_INVALID_STATE;
    }
    if (data == NULL && length != 0) {
        return NRF_ERROR_NULL;
    }
    if (!epd_ext_range_is_writable(address, length)) {
        return NRF_ERROR_INVALID_ADDR;
    }

    return fm25q04b_write(address, data, length);
}

ret_code_t epd_ext_resource_commit_staging(void) {
    epd_ext_resource_manifest_t staging;
    ret_code_t ret;

    if (!m_flash_ready) {
        return NRF_ERROR_INVALID_STATE;
    }

    ret = fm25q04b_read(EPD_EXT_RES_STAGING_MANIFEST_ADDR, &staging, sizeof(staging));
    if (ret != NRF_SUCCESS) {
        return ret;
    }

    ret = epd_ext_validate_manifest(&staging);
    if (ret != NRF_SUCCESS) {
        NRF_LOG_WARNING("External resource staging manifest validation failed: %u", ret);
        return ret;
    }

    m_manifest = staging;
    m_manifest_valid = true;
    return NRF_SUCCESS;
}
