#include "res_hal.h"

#include "epd_ext_resources.h"

ret_code_t res_hal_get_info(uint16_t type, uint16_t id, epd_ext_resource_entry_t* entry) {
    return epd_ext_resource_get_info(type, id, entry);
}

ret_code_t res_hal_read(uint16_t type, uint16_t id, uint32_t offset, void* buf, size_t len) {
    return epd_ext_resource_read(type, id, offset, buf, len);
}
