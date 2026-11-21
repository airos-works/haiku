/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Bosch accelerometers on I2C (BMA222, BMA250E, BMA253/254/255, BMA280, the
	accelerometer of the BMC150 and BMI055), as the firmware of laptops and
	tablets describes them (ACPI BOSC0200 and its relatives).

	2-in-1 laptops have two of them under one BOSC0200 device: one behind the
	screen and one under the keyboard. Nothing else in such a machine knows
	how far its screen is folded back: the firmware waits for the operating
	system to work that out from the two and to tell it (Lenovo's Yoga 11e:
	the method ANGL, which hands the mode to the embedded controller). So this
	driver does: it publishes the hinge next to the accelerometers, see
	<accelerometer.h>.
*/


#include <ACPI.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include <accelerometer.h>
#include <fs/select_sync_pool.h>
#include <i2c.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

extern "C" {
#	include "acpi.h"
}

#include "HingeTracker.h"


#define BOSCH_ACCEL_DRIVER_NAME "drivers/sensor/bosch_accel/driver_v1"
#define BOSCH_ACCEL_DEVICE_NAME "drivers/sensor/bosch_accel/device_v1"

#define ACCELEROMETER_BASENAME "sensor/accelerometer/%d"
#define ACCELEROMETER_PATHID_GENERATOR "accelerometer/path_id"
#define HINGE_BASENAME "sensor/hinge/%d"
#define HINGE_PATHID_GENERATOR "hinge/path_id"

//#define TRACE_BOSCH_ACCEL
#ifdef TRACE_BOSCH_ACCEL
#	define TRACE(x...) dprintf("bosch_accel: " x)
#else
#	define TRACE(x...)
#endif
#define INFO(x...) dprintf("bosch_accel: " x)
#define ERROR(x...) dprintf("bosch_accel: " x)


// registers
#define BMA_CHIP_ID			0x00
#define BMA_DATA			0x02
	// x, y, z: low byte (the lowest bits unused), high byte
#define BMA_RANGE			0x0f
#define BMA_BANDWIDTH		0x10
#define BMA_POWER_MODE		0x11
#define BMA_SOFT_RESET		0x14

#define BMA_RANGE_4G		0x05
#define BMA_BANDWIDTH_62HZ	0x0b
	// what it measures is 8 ms old at most, and the hum of a fan is gone
#define BMA_POWER_NORMAL	0x00
#define BMA_POWER_SUSPEND	0x80

// the largest force with BMA_RANGE_4G
static const int32 kRange = 4000;

// how often the hinge is looked at
static const bigtime_t kPollInterval = 250000;
// and when the sensors don't answer
static const bigtime_t kFailedPollInterval = 5000000;
static const int32 kMaxFailures = 4;

static const uint32 kMaxSensors = 2;


struct chip_info {
	uint8		id;
	const char*	name;
	uint8		bits;
};

static const chip_info kChips[] = {
	{ 0x03, "BMA222", 8 },
	{ 0xf8, "BMA222E", 8 },
	{ 0xf9, "BMA250E", 10 },
	{ 0xfa, "BMA255", 12 },
		// and BMA253, BMA254, the accelerometers of the BMC150 and BMI055
	{ 0xfb, "BMA280", 14 }
};

static const char* const kSupportedDevices[] = {
	"BOSC0200", "BMA0255", "BMA0280", "BMA222", "BMA222E", "BMA250E",
	"BMC150A", "BMC156A", "BMI055A", "BSBA0150", "DUAL250E"
};

/*!	What the firmware of Lenovo's Yoga 11e wants to hear (its method ANGL,
	which hands the value to the embedded controller). Found out by trying on
	a Yoga 11e (5th generation): 0 to 3 all make it a laptop, 7 and above are
	ignored. The controller takes a moment to follow, then raises its event
	and reports the mode as the ThinkPad hotkey device's GMMS does.
*/
static const uint8 kLenovoModes[] = {
	0,	// HINGE_MODE_UNKNOWN: as good as a laptop
	0,	// HINGE_MODE_LAPTOP
	4,	// HINGE_MODE_STAND
	5,	// HINGE_MODE_TENT
	6	// HINGE_MODE_TABLET
};

static const char* const kModeNames[] = {
	"unknown", "laptop", "stand", "tent", "tablet"
};


struct accel_sensor {
	i2c_addr			address;
	const chip_info*	chip;
	uint32				location;
	int8				matrix[3][3];
		// from the axes of the chip to those of <accelerometer.h>
	accelerometer_sample last;
	char				path[B_DEV_NAME_LENGTH];
};


struct accel_driver {
	device_node*		node;
	i2c_bus_interface*	i2c;
	i2c_bus				i2c_cookie;
	acpi_handle			handle;

	mutex				lock;
		// everything below, and the sensors' registers

	accel_sensor		sensors[kMaxSensors];
	uint32				sensor_count;
	bool				configured;
	bool				suspended;
	int32				failures;

	// the hinge of a 2-in-1
	bool				has_hinge;
	char				hinge_path[B_DEV_NAME_LENGTH];
	HingeTracker		tracker;
	char				lid_method[256];
		// the lid switch's _LID, empty without one
	bool				has_mode_method;
		// the firmware wants to be told the mode
	uint32				override;
	uint32				mode;
	bigtime_t			mode_changed;
	uint32				generation;
		// counts the mode changes
	bool				tell_firmware;
	select_sync_pool*	select_pool;

	thread_id			poll_thread;
	sem_id				poll_sem;
	bool				quit;
};


struct accel_open_cookie {
	accel_driver*		driver;
	accel_sensor*		sensor;
		// NULL: the hinge
	uint32				seen_generation;
};


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;


// #pragma mark - registers


static status_t
read_registers(accel_driver* driver, i2c_addr address, uint8 reg,
	uint8* buffer, size_t length)
{
	status_t status = driver->i2c->acquire_bus(driver->i2c_cookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2c_cookie, I2C_OP_READ_STOP,
		address, &reg, 1, buffer, length);
	driver->i2c->release_bus(driver->i2c_cookie);
	return status;
}


static status_t
write_register(accel_driver* driver, i2c_addr address, uint8 reg, uint8 value)
{
	uint8 command[2] = { reg, value };
	status_t status = driver->i2c->acquire_bus(driver->i2c_cookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2c_cookie, I2C_OP_WRITE_STOP,
		address, command, sizeof(command), NULL, 0);
	driver->i2c->release_bus(driver->i2c_cookie);
	return status;
}


static const chip_info*
identify_chip(accel_driver* driver, i2c_addr address)
{
	uint8 id = 0;
	status_t status = read_registers(driver, address, BMA_CHIP_ID, &id, 1);
	if (status != B_OK) {
		// once more: a chip that just woke up may miss the first one
		snooze(2000);
		status = read_registers(driver, address, BMA_CHIP_ID, &id, 1);
	}
	if (status != B_OK) {
		ERROR("nothing answers at %#x: %s\n", address, strerror(status));
		return NULL;
	}

	for (size_t i = 0; i < B_COUNT_OF(kChips); i++) {
		if (kChips[i].id == id)
			return &kChips[i];
	}
	ERROR("unknown chip %#x at %#x\n", id, address);
	return NULL;
}


/*!	Sets the sensors up to measure. They forget it when they lose power, as
	in a suspend to RAM.
*/
static status_t
configure_sensors(accel_driver* driver)
{
	for (uint32 i = 0; i < driver->sensor_count; i++) {
		i2c_addr address = driver->sensors[i].address;
		status_t status = write_register(driver, address, BMA_POWER_MODE,
			BMA_POWER_NORMAL);
		if (status != B_OK)
			return status;
		// (waking up takes it 1.8 ms at most)
		snooze(2000);
		status = write_register(driver, address, BMA_RANGE, BMA_RANGE_4G);
		if (status == B_OK) {
			status = write_register(driver, address, BMA_BANDWIDTH,
				BMA_BANDWIDTH_62HZ);
		}
		if (status != B_OK)
			return status;
	}
	// the first filtered sample
	snooze(10000);
	driver->configured = true;
	return B_OK;
}


static status_t
read_sample(accel_driver* driver, accel_sensor* sensor)
{
	uint8 data[6];
	status_t status = read_registers(driver, sensor->address, BMA_DATA, data,
		sizeof(data));
	if (status != B_OK)
		return status;

	// in mg: the whole range of the chip is twice kRange
	int32 raw[3];
	int32 shift = 16 - sensor->chip->bits;
	for (int i = 0; i < 3; i++) {
		int16 value = (int16)(data[2 * i] | (data[2 * i + 1] << 8));
		raw[i] = ((value >> shift) * 2 * kRange) >> sensor->chip->bits;
	}

	int32 turned[3];
	for (int i = 0; i < 3; i++) {
		turned[i] = sensor->matrix[i][0] * raw[0]
			+ sensor->matrix[i][1] * raw[1] + sensor->matrix[i][2] * raw[2];
	}

	sensor->last.time = system_time();
	sensor->last.x = turned[0];
	sensor->last.y = turned[1];
	sensor->last.z = turned[2];
	sensor->last._reserved = 0;
	return B_OK;
}


// #pragma mark - firmware


struct i2c_addresses {
	i2c_addr	address[kMaxSensors];
	uint32		count;
};


static acpi_status
collect_addresses(ACPI_RESOURCE* resource, void* context)
{
	i2c_addresses* addresses = (i2c_addresses*)context;
	if (resource->Type == ACPI_RESOURCE_TYPE_SERIAL_BUS
		&& resource->Data.CommonSerialBus.Type
			== ACPI_RESOURCE_SERIAL_TYPE_I2C
		&& addresses->count < kMaxSensors) {
		addresses->address[addresses->count++]
			= resource->Data.I2cSerialBus.SlaveAddress;
	}
	return AE_OK;
}


/*!	How the chip is mounted, from a method that returns three strings of
	three numbers each ("0 -1 0"): the rows of the matrix that turns what the
	chip measures into the axes Windows expects.
*/
static bool
read_mount_matrix(accel_driver* driver, const char* method, int8 matrix[3][3])
{
	acpi_data buffer;
	buffer.pointer = NULL;
	buffer.length = ACPI_ALLOCATE_BUFFER;
	if (sACPI->evaluate_method(driver->handle, method, NULL, &buffer) != B_OK
		|| buffer.pointer == NULL) {
		return false;
	}

	bool valid = false;
	acpi_object_type* object = (acpi_object_type*)buffer.pointer;
	if (object->object_type == ACPI_TYPE_PACKAGE
		&& object->package.count == 3) {
		valid = true;
		for (int row = 0; row < 3 && valid; row++) {
			acpi_object_type* element = &object->package.objects[row];
			if (element->object_type != ACPI_TYPE_STRING
				|| element->string.string == NULL) {
				valid = false;
				break;
			}

			const char* string = element->string.string;
			for (int column = 0; column < 3; column++) {
				char* end;
				long value = strtol(string, &end, 10);
				if (end == string || value < -1 || value > 1) {
					valid = false;
					break;
				}
				matrix[row][column] = (int8)value;
				string = end;
			}
		}
	}

	free(buffer.pointer);
	return valid;
}


/*!	The firmware's matrices are made for Windows, which wants the direction
	things fall in: the opposite of ours. And the one of the base of a 2-in-1
	gives the axes the screen has when the machine is closed, which looks at
	the keyboard from below.
*/
static void
set_sensor_matrix(accel_sensor* sensor, const int8 firmware[3][3],
	bool baseOfHinge)
{
	for (int row = 0; row < 3; row++) {
		int8 sign = -1;
		if (baseOfHinge && row > 0)
			sign = 1;
		for (int column = 0; column < 3; column++)
			sensor->matrix[row][column] = sign * firmware[row][column];
	}
}


static HingeTracker::lid_state
lid_state_hook(void* cookie)
{
	accel_driver* driver = (accel_driver*)cookie;
	if (driver->lid_method[0] == '\0')
		return HingeTracker::kLidUnknown;

	acpi_object_type object;
	acpi_data buffer;
	buffer.pointer = &object;
	buffer.length = sizeof(object);
	if (sACPI->evaluate_method(NULL, driver->lid_method, NULL, &buffer)
			!= B_OK
		|| object.object_type != ACPI_TYPE_INTEGER) {
		return HingeTracker::kLidUnknown;
	}
	return object.integer.integer != 0
		? HingeTracker::kLidOpen : HingeTracker::kLidClosed;
}


/*!	Hands the mode to the firmware, which keeps it in the embedded
	controller: that one switches the keyboard and the touchpad off while
	they are folded away, and picks its fan curve by it.
*/
static void
tell_firmware(accel_driver* driver, uint32 mode)
{
	if (!driver->has_mode_method)
		return;

	acpi_object_type argument;
	argument.object_type = ACPI_TYPE_INTEGER;
	argument.integer.integer = kLenovoModes[mode];
	acpi_objects arguments = { 1, &argument };

	acpi_data buffer;
	buffer.pointer = NULL;
	buffer.length = ACPI_ALLOCATE_BUFFER;
	status_t status = sACPI->evaluate_method(driver->handle, "ANGL",
		&arguments, &buffer);
	free(buffer.pointer);
	if (status != B_OK)
		ERROR("telling the firmware the mode failed: %s\n", strerror(status));
}


// #pragma mark - the hinge


static void
set_mode(accel_driver* driver, uint32 mode)
{
	if (mode == driver->mode && !driver->tell_firmware)
		return;

	bool changed = mode != driver->mode;
	if (changed) {
		INFO("%s%s, the hinge at %" B_PRId32 " degrees\n", kModeNames[mode],
			driver->override != HINGE_MODE_UNKNOWN ? " (set by hand)" : "",
			driver->tracker.Angle());
		driver->mode = mode;
		driver->mode_changed = system_time();
		driver->generation++;
	}

	driver->tell_firmware = false;
	tell_firmware(driver, mode);

	if (changed && driver->select_pool != NULL)
		notify_select_event_pool(driver->select_pool, B_SELECT_READ);
}


static status_t
poll_sensors(accel_driver* driver)
{
	if (!driver->configured) {
		status_t status = configure_sensors(driver);
		if (status != B_OK)
			return status;
	}

	for (uint32 i = 0; i < driver->sensor_count; i++) {
		status_t status = read_sample(driver, &driver->sensors[i]);
		if (status != B_OK)
			return status;
	}

	accel_vector display = { driver->sensors[0].last.x,
		driver->sensors[0].last.y, driver->sensors[0].last.z };
	accel_vector base = { driver->sensors[1].last.x,
		driver->sensors[1].last.y, driver->sensors[1].last.z };
	driver->tracker.Update(display, base);

	uint32 mode = driver->override != HINGE_MODE_UNKNOWN
		? driver->override : driver->tracker.Mode();
	if (mode != HINGE_MODE_UNKNOWN)
		set_mode(driver, mode);
	return B_OK;
}


static status_t
poll_thread(void* cookie)
{
	accel_driver* driver = (accel_driver*)cookie;

	while (!driver->quit) {
		bigtime_t interval = kPollInterval;

		mutex_lock(&driver->lock);
		if (!driver->suspended) {
			status_t status = poll_sensors(driver);
			if (status == B_OK) {
				if (driver->failures >= kMaxFailures)
					INFO("the sensors answer again\n");
				driver->failures = 0;
			} else if (driver->failures < kMaxFailures) {
				if (++driver->failures == kMaxFailures) {
					ERROR("the sensors don't answer (%s), looking less "
						"often\n", strerror(status));
				}
			}

			if (driver->failures >= kMaxFailures) {
				interval = kFailedPollInterval;
				// whatever happened to them, they may have lost their setup
				driver->configured = false;
			}
		}
		mutex_unlock(&driver->lock);

		acquire_sem_etc(driver->poll_sem, 1, B_RELATIVE_TIMEOUT, interval);
	}

	return B_OK;
}


// #pragma mark - device module API


static status_t
bosch_accel_init_device(void* driverCookie, void** _cookie)
{
	*_cookie = driverCookie;
	return B_OK;
}


static void
bosch_accel_uninit_device(void* cookie)
{
}


static status_t
bosch_accel_open(void* initCookie, const char* path, int flags,
	void** _cookie)
{
	accel_driver* driver = (accel_driver*)initCookie;

	accel_sensor* sensor = NULL;
	if (!driver->has_hinge || strcmp(path, driver->hinge_path) != 0) {
		for (uint32 i = 0; i < driver->sensor_count; i++) {
			if (strcmp(path, driver->sensors[i].path) == 0)
				sensor = &driver->sensors[i];
		}
		if (sensor == NULL)
			return B_ENTRY_NOT_FOUND;
	}

	accel_open_cookie* cookie
		= (accel_open_cookie*)malloc(sizeof(accel_open_cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->driver = driver;
	cookie->sensor = sensor;
	cookie->seen_generation = 0;
		// (the mode it never asked for is a change)
	*_cookie = cookie;
	return B_OK;
}


static status_t
bosch_accel_close(void* cookie)
{
	return B_OK;
}


static status_t
bosch_accel_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


/*!	The current sample: a new one, unless the machine is going to sleep. */
static status_t
get_sample(accel_open_cookie* cookie, accelerometer_sample& sample)
{
	accel_driver* driver = cookie->driver;
	MutexLocker locker(driver->lock);

	if (!driver->suspended) {
		status_t status = B_OK;
		if (!driver->configured)
			status = configure_sensors(driver);
		if (status == B_OK)
			status = read_sample(driver, cookie->sensor);
		if (status != B_OK)
			return status;
	} else if (cookie->sensor->last.time == 0)
		return B_DEV_NOT_READY;

	sample = cookie->sensor->last;
	return B_OK;
}


static void
get_hinge_state(accel_open_cookie* cookie, hinge_state& state)
{
	accel_driver* driver = cookie->driver;
	MutexLocker locker(driver->lock);

	state.version = ACCELEROMETER_API_VERSION;
	state.mode = driver->mode;
	state.angle = driver->tracker.Angle();
	state.flags = 0;
	if (driver->override != HINGE_MODE_UNKNOWN)
		state.flags |= HINGE_STATE_OVERRIDDEN;
	if (driver->tracker.IsAngleValid() && !driver->suspended)
		state.flags |= HINGE_STATE_ANGLE_VALID;
	state.changed = driver->mode_changed;

	cookie->seen_generation = driver->generation;
}


static status_t
bosch_accel_read(void* _cookie, off_t position, void* buffer,
	size_t* _length)
{
	accel_open_cookie* cookie = (accel_open_cookie*)_cookie;

	if (position != 0) {
		*_length = 0;
		return B_OK;
	}

	char line[64];
	if (cookie->sensor != NULL) {
		accelerometer_sample sample;
		status_t status = get_sample(cookie, sample);
		if (status != B_OK) {
			*_length = 0;
			return status;
		}
		snprintf(line, sizeof(line), "%" B_PRId32 " %" B_PRId32 " %" B_PRId32
			"\n", sample.x, sample.y, sample.z);
	} else {
		hinge_state state;
		get_hinge_state(cookie, state);
		snprintf(line, sizeof(line), "%s %" B_PRId32 "\n",
			kModeNames[state.mode], state.angle);
	}

	size_t length = min_c(strlen(line), *_length);
	if (user_memcpy(buffer, line, length) != B_OK) {
		*_length = 0;
		return B_BAD_ADDRESS;
	}
	*_length = length;
	return B_OK;
}


static status_t
bosch_accel_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
bosch_accel_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	accel_open_cookie* cookie = (accel_open_cookie*)_cookie;
	accel_driver* driver = cookie->driver;

	if (cookie->sensor != NULL) {
		switch (op) {
			case ACCELEROMETER_GET_INFO:
			{
				accelerometer_info info;
				memset(&info, 0, sizeof(info));
				info.version = ACCELEROMETER_API_VERSION;
				info.location = cookie->sensor->location;
				strlcpy(info.name, cookie->sensor->chip->name,
					sizeof(info.name));
				info.range = kRange;
				info.resolution
					= (2 * kRange * 1000) >> cookie->sensor->chip->bits;
				return user_memcpy(buffer, &info, sizeof(info));
			}

			case ACCELEROMETER_GET_SAMPLE:
			{
				accelerometer_sample sample;
				status_t status = get_sample(cookie, sample);
				if (status != B_OK)
					return status;
				return user_memcpy(buffer, &sample, sizeof(sample));
			}
		}
		return B_DEV_INVALID_IOCTL;
	}

	switch (op) {
		case HINGE_GET_STATE:
		{
			hinge_state state;
			get_hinge_state(cookie, state);
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case HINGE_SET_OVERRIDE:
		{
			uint32 mode;
			if (user_memcpy(&mode, buffer, sizeof(mode)) != B_OK)
				return B_BAD_ADDRESS;
			if (mode > HINGE_MODE_TABLET)
				return B_BAD_VALUE;

			MutexLocker locker(driver->lock);
			driver->override = mode;
			if (mode != HINGE_MODE_UNKNOWN)
				set_mode(driver, mode);
			locker.Unlock();

			// (back to what the sensors say)
			release_sem_etc(driver->poll_sem, 1, B_DO_NOT_RESCHEDULE);
			return B_OK;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
bosch_accel_select(void* _cookie, uint8 event, selectsync* sync)
{
	accel_open_cookie* cookie = (accel_open_cookie*)_cookie;
	accel_driver* driver = cookie->driver;

	if (event != B_SELECT_READ || cookie->sensor != NULL)
		return B_BAD_VALUE;

	MutexLocker locker(driver->lock);
	status_t status = add_select_sync_pool_entry(&driver->select_pool, sync,
		event);
	if (status != B_OK)
		return status;

	if (cookie->seen_generation != driver->generation)
		notify_select_event(sync, event);
	return B_OK;
}


static status_t
bosch_accel_deselect(void* _cookie, uint8 event, selectsync* sync)
{
	accel_open_cookie* cookie = (accel_open_cookie*)_cookie;
	accel_driver* driver = cookie->driver;

	if (event != B_SELECT_READ || cookie->sensor != NULL)
		return B_BAD_VALUE;

	MutexLocker locker(driver->lock);
	return remove_select_sync_pool_entry(&driver->select_pool, sync, event);
}


// #pragma mark - driver module API


static float
bosch_accel_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1;
	}
	if (strcmp(bus, "i2c") != 0)
		return 0.0;

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || hid == NULL) {
		return 0.0;
	}

	for (size_t i = 0; i < B_COUNT_OF(kSupportedDevices); i++) {
		if (strcmp(hid, kSupportedDevices[i]) == 0) {
			// above i2c_hid: BOSC0200 says it is compatible with an HID
			// device, which it is not
			return 0.8;
		}
	}
	return 0.0;
}


static status_t
bosch_accel_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Bosch accelerometer" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, BOSCH_ACCEL_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
bosch_accel_init_driver(device_node* node, void** _driverCookie)
{
	accel_driver* driver = new(std::nothrow) accel_driver;
	if (driver == NULL)
		return B_NO_MEMORY;

	driver->node = node;
	driver->sensor_count = 0;
	driver->configured = false;
	driver->suspended = false;
	driver->failures = 0;
	driver->has_hinge = false;
	driver->hinge_path[0] = '\0';
	driver->lid_method[0] = '\0';
	driver->has_mode_method = false;
	driver->override = HINGE_MODE_UNKNOWN;
	driver->mode = HINGE_MODE_UNKNOWN;
	driver->mode_changed = 0;
	driver->generation = 1;
	driver->tell_firmware = true;
	driver->select_pool = NULL;
	driver->poll_thread = -1;
	driver->poll_sem = -1;
	driver->quit = false;
	memset(driver->sensors, 0, sizeof(driver->sensors));

	// The I2C device above us is the first address of the ACPI device only:
	// the second sensor is reached through the bus it sits on.
	device_node* parent = sDeviceManager->get_parent_node(node);
	device_node* bus = sDeviceManager->get_parent_node(parent);
	sDeviceManager->put_node(parent);
	status_t status = sDeviceManager->get_driver(bus,
		(driver_module_info**)&driver->i2c, (void**)&driver->i2c_cookie);
	sDeviceManager->put_node(bus);
	if (status != B_OK || driver->i2c == NULL) {
		delete driver;
		return status != B_OK ? status : B_ERROR;
	}

	uint16 firstAddress;
	if (sDeviceManager->get_attr_uint64(node, ACPI_DEVICE_HANDLE_ITEM,
			(uint64*)&driver->handle, true) != B_OK
		|| sDeviceManager->get_attr_uint16(node, I2C_DEVICE_SLAVE_ADDR_ITEM,
			&firstAddress, true) != B_OK) {
		delete driver;
		return B_DEVICE_NOT_FOUND;
	}

	i2c_addresses addresses;
	addresses.count = 0;
	sACPI->walk_resources(driver->handle, (char*)"_CRS",
		(acpi_walk_resources_callback)collect_addresses, &addresses);
	if (addresses.count == 0 || addresses.address[0] != firstAddress) {
		addresses.address[0] = firstAddress;
		addresses.count = 1;
	}

	mutex_init(&driver->lock, "bosch accelerometer");

	for (uint32 i = 0; i < addresses.count; i++) {
		const chip_info* chip = identify_chip(driver, addresses.address[i]);
		if (chip == NULL) {
			if (i == 0) {
				mutex_destroy(&driver->lock);
				delete driver;
				return B_DEVICE_NOT_FOUND;
			}
			break;
		}

		accel_sensor& sensor = driver->sensors[driver->sensor_count++];
		sensor.address = addresses.address[i];
		sensor.chip = chip;
		sensor.location = ACCELEROMETER_LOCATION_UNKNOWN;
		for (int row = 0; row < 3; row++)
			sensor.matrix[row][row] = 1;
	}

	// How they are mounted. The pair of a 2-in-1 comes with a matrix each:
	// ROMS for the first, behind the screen, ROMK for the one under the
	// keyboard. A single sensor has ROTM, if anything.
	int8 matrix[3][3];
	int8 baseMatrix[3][3];
	if (driver->sensor_count == 2 && read_mount_matrix(driver, "ROMS", matrix)
		&& read_mount_matrix(driver, "ROMK", baseMatrix)) {
		set_sensor_matrix(&driver->sensors[0], matrix, false);
		driver->sensors[0].location = ACCELEROMETER_LOCATION_DISPLAY;
		set_sensor_matrix(&driver->sensors[1], baseMatrix, true);
		driver->sensors[1].location = ACCELEROMETER_LOCATION_BASE;
		driver->has_hinge = true;
	} else if (read_mount_matrix(driver, "ROTM", matrix)) {
		for (uint32 i = 0; i < driver->sensor_count; i++)
			set_sensor_matrix(&driver->sensors[i], matrix, false);
	}

	for (uint32 i = 0; i < driver->sensor_count; i++) {
		INFO("%s at %#x%s\n", driver->sensors[i].chip->name,
			driver->sensors[i].address,
			driver->sensors[i].location == ACCELEROMETER_LOCATION_DISPLAY
				? ", behind the screen"
				: driver->sensors[i].location == ACCELEROMETER_LOCATION_BASE
					? ", under the keyboard" : "");
	}

	status = configure_sensors(driver);
	if (status != B_OK) {
		ERROR("setting up the sensors failed: %s\n", strerror(status));
		mutex_destroy(&driver->lock);
		delete driver;
		return status;
	}

	if (driver->has_hinge) {
		acpi_handle method;
		driver->has_mode_method = sACPI->get_handle(driver->handle, "ANGL",
			&method) == B_OK;

		char lid[200];
		if (sACPI->get_device("PNP0C0D", 0, lid, sizeof(lid)) == B_OK) {
			snprintf(driver->lid_method, sizeof(driver->lid_method),
				"%s._LID", lid);
		}
		driver->tracker.SetLidHook(lid_state_hook, driver);

		INFO("following the hinge%s\n", driver->has_mode_method
			? ", and telling the firmware" : "");

		driver->poll_sem = create_sem(0, "bosch accelerometer poll");
		driver->poll_thread = spawn_kernel_thread(poll_thread,
			"hinge sensor", B_LOW_PRIORITY, driver);
		if (driver->poll_sem < 0 || driver->poll_thread < 0) {
			ERROR("no thread to follow the hinge\n");
			if (driver->poll_sem >= 0)
				delete_sem(driver->poll_sem);
			driver->poll_sem = -1;
			driver->poll_thread = -1;
			driver->has_hinge = false;
		} else
			resume_thread(driver->poll_thread);
	}

	*_driverCookie = driver;
	return B_OK;
}


static void
bosch_accel_uninit_driver(void* driverCookie)
{
	accel_driver* driver = (accel_driver*)driverCookie;

	if (driver->poll_thread >= 0) {
		driver->quit = true;
		release_sem(driver->poll_sem);
		status_t result;
		wait_for_thread(driver->poll_thread, &result);
		delete_sem(driver->poll_sem);
	}

	mutex_destroy(&driver->lock);
	delete driver;
}


static status_t
bosch_accel_register_child_devices(void* driverCookie)
{
	accel_driver* driver = (accel_driver*)driverCookie;

	for (uint32 i = 0; i < driver->sensor_count; i++) {
		int id = sDeviceManager->create_id(ACCELEROMETER_PATHID_GENERATOR);
		if (id < 0) {
			ERROR("no id for an accelerometer\n");
			return B_ERROR;
		}

		accel_sensor& sensor = driver->sensors[i];
		snprintf(sensor.path, sizeof(sensor.path), ACCELEROMETER_BASENAME,
			id);
		status_t status = sDeviceManager->publish_device(driver->node,
			sensor.path, BOSCH_ACCEL_DEVICE_NAME);
		if (status != B_OK) {
			sDeviceManager->free_id(ACCELEROMETER_PATHID_GENERATOR, id);
			sensor.path[0] = '\0';
			return status;
		}
	}

	if (driver->has_hinge) {
		int id = sDeviceManager->create_id(HINGE_PATHID_GENERATOR);
		if (id < 0) {
			ERROR("no id for the hinge\n");
			return B_ERROR;
		}

		snprintf(driver->hinge_path, sizeof(driver->hinge_path),
			HINGE_BASENAME, id);
		status_t status = sDeviceManager->publish_device(driver->node,
			driver->hinge_path, BOSCH_ACCEL_DEVICE_NAME);
		if (status != B_OK) {
			sDeviceManager->free_id(HINGE_PATHID_GENERATOR, id);
			driver->hinge_path[0] = '\0';
			return status;
		}
	}

	return B_OK;
}


static status_t
bosch_accel_suspend(void* driverCookie, int32 state)
{
	accel_driver* driver = (accel_driver*)driverCookie;
	MutexLocker locker(driver->lock);

	driver->suspended = true;
	for (uint32 i = 0; i < driver->sensor_count; i++) {
		write_register(driver, driver->sensors[i].address, BMA_POWER_MODE,
			BMA_POWER_SUSPEND);
	}
	driver->configured = false;
	return B_OK;
}


static status_t
bosch_accel_resume(void* driverCookie)
{
	accel_driver* driver = (accel_driver*)driverCookie;
	MutexLocker locker(driver->lock);

	// The screen may have been folded while the machine slept, and the
	// firmware may have forgotten what it was told.
	driver->suspended = false;
	driver->tracker.ForgetAngle();
	driver->tell_firmware = true;
	locker.Unlock();

	if (driver->poll_sem >= 0)
		release_sem_etc(driver->poll_sem, 1, B_DO_NOT_RESCHEDULE);
	return B_OK;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sACPI },
	{}
};


driver_module_info bosch_accel_driver_module = {
	{
		BOSCH_ACCEL_DRIVER_NAME,
		0,
		NULL
	},

	bosch_accel_support,
	bosch_accel_register_device,
	bosch_accel_init_driver,
	bosch_accel_uninit_driver,
	bosch_accel_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
	bosch_accel_suspend,
	bosch_accel_resume
};


struct device_module_info bosch_accel_device_module = {
	{
		BOSCH_ACCEL_DEVICE_NAME,
		0,
		NULL
	},

	bosch_accel_init_device,
	bosch_accel_uninit_device,
	NULL,

	bosch_accel_open,
	bosch_accel_close,
	bosch_accel_free,
	bosch_accel_read,
	bosch_accel_write,
	NULL,
	bosch_accel_control,
	bosch_accel_select,
	bosch_accel_deselect
};


module_info* modules[] = {
	(module_info*)&bosch_accel_driver_module,
	(module_info*)&bosch_accel_device_module,
	NULL
};
