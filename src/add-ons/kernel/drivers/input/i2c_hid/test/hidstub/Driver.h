/* the driver's Driver.h for the resume test: no device list, tracing to the
   test's log */
#ifndef _TEST_HID_DRIVER_H
#define _TEST_HID_DRIVER_H
#include <KernelExport.h>
#include <OS.h>
#define DRIVER_NAME "i2c_hid"
#define DEVICE_PATH_SUFFIX "i2c"
#define DEVICE_NAME "I2C"
#define TRACE(x...)
#define ERROR(x...) dprintf(DRIVER_NAME ": " x)
#define TRACE_ALWAYS(x...) dprintf(DRIVER_NAME ": " x)
#define CALLED()
#endif
