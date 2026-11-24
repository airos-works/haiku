#ifndef _TEST_HID_I2C_H
#define _TEST_HID_I2C_H
#include <SupportDefs.h>
typedef void* i2c_device;
typedef enum {
	I2C_OP_READ = 0,
	I2C_OP_READ_STOP = 1,
	I2C_OP_WRITE = 2,
	I2C_OP_WRITE_STOP = 3,
	I2C_OP_READ_BLOCK = 5,
	I2C_OP_WRITE_BLOCK = 7,
} i2c_op;
struct i2c_device_interface {
	status_t (*exec_command)(i2c_device cookie, i2c_op op,
		const void* cmdBuffer, size_t cmdLength, void* dataBuffer,
		size_t dataLength);
	status_t (*acquire_bus)(i2c_device cookie);
	void (*release_bus)(i2c_device cookie);
};
#endif
