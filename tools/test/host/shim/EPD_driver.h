/* Host-test shim: shadows EPD/EPD_driver.h when -Itools/test/host/shim comes first. */
#ifndef __EPD_DRIVER_H
#define __EPD_DRIVER_H

#include <stdint.h>

#define LOW_BATTERY_THRESHOLD_MV 2800
#define CRITICAL_BATTERY_SHUTDOWN_MV 2700

#endif
