/*
 * Copyright 2022, Jérôme Duval. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>

#include <kernel.h>

#include "ACPIPrivate.h"


#define	TRACE(x...)			//dprintf("acpi_call: " x)
#define TRACE_ALWAYS(x...)	dprintf("acpi_call: " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


typedef struct {
	device_node *node;
	acpi_root_info	*acpi;
	void	*acpi_cookie;
} acpi_call_device_info;


struct acpi_call_desc
{
	char*			path;
	acpi_objects	args;
	acpi_status	retval;
	acpi_data	result;
	acpi_size	reslen;
};


// A control method takes at most seven arguments (ACPI_METHOD_NUM_ARGS).
static const uint32 kMaxArguments = 7;
// The largest string or buffer argument that is copied into the kernel.
static const size_t kMaxArgumentSize = 64 * 1024;


//	#pragma mark - device module API


static status_t
acpi_call_init_device(void* _node, void** _cookie)
{
	CALLED();
	device_node *node = (device_node *)_node;

	acpi_call_device_info* device = (acpi_call_device_info*)calloc(1, sizeof(acpi_call_device_info));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	status_t err = gDeviceManager->get_driver(node, (driver_module_info **)&device->acpi,
		(void **)&device->acpi_cookie);
	if (err != B_OK) {
		free(device);
		return err;
	}

	*_cookie = device;
	return err;
}


static void
acpi_call_uninit_device(void* _cookie)
{
	CALLED();
	acpi_call_device_info* device = (acpi_call_device_info*)_cookie;
	free(device);
}


static status_t
acpi_call_open(void* _device, const char* path, int openMode, void** _cookie)
{
	CALLED();
	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	*_cookie = device;
	return B_OK;
}


static status_t
acpi_call_read(void *cookie, off_t position, void *buffer, size_t *numBytes)
{
	TRACE("read(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


static status_t
acpi_call_write(void *cookie, off_t position, const void *buffer,
	size_t *numBytes)
{
	TRACE("write(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


void
acpi_call_fixup_pointers(acpi_object_type *p, void *target)
{
	CALLED();
	switch (p->object_type)
	{
	case ACPI_TYPE_STRING:
		p->string.string = (char*)((uint8*)(p->string.string) - (uint8*)p + (uint8*)target);
		break;
	case ACPI_TYPE_BUFFER:
		p->buffer.buffer = (void*)((uint8*)(p->buffer.buffer) - (uint8*)p + (uint8*)target);
		break;
	}
}


/*!	Frees what acpi_call_copy_arguments() allocated for the first \a count
	arguments.
*/
static void
acpi_call_free_arguments(acpi_object_type* args, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		if (args[i].object_type == ACPI_TYPE_STRING)
			free(args[i].string.string);
		else if (args[i].object_type == ACPI_TYPE_BUFFER)
			free(args[i].buffer.buffer);
	}
}


/*!	Copies the caller's argument objects, and the strings and buffers they
	point to, into kernel memory: ACPICA reads all of them directly, which
	it must not do with memory of the calling team (a wrong pointer would be
	an unhandled fault, and with SMAP even a right one is).
	On success the caller owns the copies (acpi_call_free_arguments()).
*/
static status_t
acpi_call_copy_arguments(const acpi_objects& userArgs, acpi_object_type* args)
{
	if (userArgs.count == 0)
		return B_OK;
	if (userArgs.count > kMaxArguments)
		return B_BAD_VALUE;
	if (!IS_USER_ADDRESS(userArgs.pointer)
		|| user_memcpy(args, userArgs.pointer,
			userArgs.count * sizeof(acpi_object_type)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	status_t status = B_OK;
	uint32 i = 0;
	for (; i < userArgs.count; i++) {
		void* userData;
		size_t size;
		if (args[i].object_type == ACPI_TYPE_INTEGER) {
			continue;
		} else if (args[i].object_type == ACPI_TYPE_STRING) {
			userData = args[i].string.string;
			size = args[i].string.len;
		} else if (args[i].object_type == ACPI_TYPE_BUFFER) {
			userData = args[i].buffer.buffer;
			size = args[i].buffer.length;
		} else {
			// Packages would have to be copied object by object, and
			// references carry a kernel pointer: neither can be passed.
			status = B_BAD_VALUE;
			break;
		}

		if (size > kMaxArgumentSize) {
			status = B_BAD_VALUE;
			break;
		}
		if (!IS_USER_ADDRESS(userData)) {
			status = B_BAD_ADDRESS;
			break;
		}

		// one more byte, so that a string is terminated in any case
		char* data = (char*)malloc(size + 1);
		if (data == NULL) {
			status = B_NO_MEMORY;
			break;
		}
		if (user_memcpy(data, userData, size) != B_OK) {
			free(data);
			status = B_BAD_ADDRESS;
			break;
		}
		data[size] = '\0';

		if (args[i].object_type == ACPI_TYPE_STRING)
			args[i].string.string = data;
		else
			args[i].buffer.buffer = data;
	}

	if (status != B_OK)
		acpi_call_free_arguments(args, i);
	return status;
}


static status_t
acpi_call_control(void *_device, uint32 op, void *buffer, size_t length)
{
	TRACE("control(%p, %" B_PRIu32 ", %p, %lu)\n", _device, op, buffer, length);
	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	if (op == 'ACCA') {
		struct acpi_call_desc params;
		char path[1024];
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&params, buffer, sizeof(params)) != B_OK
			|| !IS_USER_ADDRESS(params.path)) {
			return B_BAD_ADDRESS;
		}

		// The path is a string: copying a fixed number of bytes fails for
		// one that ends near the end of a mapping, and does not terminate.
		ssize_t pathLength = user_strlcpy(path, params.path, sizeof(path));
		if (pathLength < 0)
			return B_BAD_ADDRESS;
		if ((size_t)pathLength >= sizeof(path))
			return B_NAME_TOO_LONG;

		// Check where the result goes before the method runs.
		if (params.result.pointer != NULL
			&& !IS_USER_ADDRESS(params.result.pointer)) {
			return B_BAD_ADDRESS;
		}

		acpi_object_type argObjects[kMaxArguments];
		status_t status = acpi_call_copy_arguments(params.args, argObjects);
		if (status != B_OK)
			return status;

		acpi_objects args;
		args.count = params.args.count;
		args.pointer = argObjects;

		acpi_data result;
		result.length = ACPI_ALLOCATE_BUFFER;
		result.pointer = NULL;

		acpi_status retval = device->acpi->evaluate_method(NULL, path, &args,
			&result);
		acpi_call_free_arguments(argObjects, args.count);

		if (retval == 0 && result.pointer != NULL
			&& params.result.pointer != NULL) {
			params.result.length = min_c(params.result.length, result.length);
			if (result.length >= sizeof(acpi_object_type)) {
				acpi_call_fixup_pointers((acpi_object_type*)(result.pointer),
					params.result.pointer);
			}

			if (user_memcpy(params.result.pointer, result.pointer,
					params.result.length) != B_OK
				|| user_memcpy(buffer, &params, sizeof(params)) != B_OK) {
				status = B_BAD_ADDRESS;
			}
		}
		free(result.pointer);
		return status;
	}

	return B_ERROR;
}


static status_t
acpi_call_close(void *cookie)
{
	TRACE("close(%p)\n", cookie);
	return B_OK;
}


static status_t
acpi_call_free(void *cookie)
{
	TRACE("free(%p)\n", cookie);
	return B_OK;
}


//	#pragma mark -


struct device_module_info gAcpiCallDeviceModule = {
	{
		ACPI_CALL_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	acpi_call_init_device,
	acpi_call_uninit_device,
	NULL, // remove,

	acpi_call_open,
	acpi_call_close,
	acpi_call_free,
	acpi_call_read,
	acpi_call_write,
	NULL,	// io
	acpi_call_control,

	NULL,	// select
	NULL,	// deselect
};

