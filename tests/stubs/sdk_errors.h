#ifndef TEST_SDK_ERRORS_H
#define TEST_SDK_ERRORS_H

#include <stdint.h>

typedef uint32_t ret_code_t;

#define NRF_SUCCESS 0U
/* Values from the SDK nrf_error.h used by the resource transport retry rules. */
#define NRF_ERROR_INVALID_STATE 8U
#define NRF_ERROR_BUSY 17U
#define NRF_ERROR_RESOURCES 19U

#endif
