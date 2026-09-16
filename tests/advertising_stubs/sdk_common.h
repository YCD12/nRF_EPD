#ifndef TEST_ADVERTISING_SDK_COMMON_H
#define TEST_ADVERTISING_SDK_COMMON_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdk_errors.h"
#include "app_util.h"
#define NRF_MODULE_ENABLED(module) 1
#define VERIFY_SUCCESS(result) do { if ((result) != NRF_SUCCESS) return (result); } while (0)
#define VERIFY_PARAM_NOT_NULL(ptr) do { if ((ptr) == NULL) return NRF_ERROR_NULL; } while (0)
#define VERIFY_TRUE(condition, error) do { if (!(condition)) return (error); } while (0)
#define ASSERT(condition) assert(condition)
#endif
