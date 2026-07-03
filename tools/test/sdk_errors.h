/* Host-test stub for the nRF SDK sdk_errors.h.
 * Only the symbols used by Lunar.c / epd_ext_resources.h are provided so the
 * firmware sources can be compiled and unit-tested natively on a PC. */
#ifndef SDK_ERRORS_H_HOST_STUB
#define SDK_ERRORS_H_HOST_STUB

#include <stdint.h>

typedef uint32_t ret_code_t;

#define NRF_SUCCESS                 0UL
#define NRF_ERROR_NOT_FOUND         0x05UL
#define NRF_ERROR_INVALID_PARAM     0x07UL
#define NRF_ERROR_INVALID_LENGTH    0x0CUL

#endif
