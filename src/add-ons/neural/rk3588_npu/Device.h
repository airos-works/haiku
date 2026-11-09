/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_NPU_DEVICE_H
#define RK3588_NPU_DEVICE_H


#include <SupportDefs.h>

#include "rk3588_npu.h"


/*!	Memory the processor and this team both see. */
struct DeviceBuffer {
	uint32		handle;
	uint32		device_address;
	uint8*		address;
	size_t		size;

	DeviceBuffer()
		:
		handle(0),
		device_address(0),
		address(NULL),
		size(0)
	{
	}
};


/*!	The team's open device. All programs of the team share one, so that
	the driver need not switch address spaces between their jobs; it is
	closed when the last program is gone, which is what lets the driver
	switch the processor off. */
class Device {
public:
	//! Opens the device for a moment to see that it works.
	static	status_t			Probe(rk3588_npu_info* info);

	static	status_t			Acquire(Device** _device);
			void				Release();

			status_t			CreateBuffer(size_t size,
									DeviceBuffer& buffer);
			void				DeleteBuffer(DeviceBuffer& buffer);
			status_t			Sync(const DeviceBuffer& buffer,
									uint32 direction, size_t offset,
									size_t size);
			status_t			Submit(const rk3588_npu_task* tasks,
									uint32 count, bigtime_t* _duration);

	//! Whether an error of Submit() means that the device is gone.
	static	bool				IsGone(status_t error);

private:
								Device(int fd);
								~Device();

			int					fFD;
			int32				fReferences;
};


#endif	// RK3588_NPU_DEVICE_H
