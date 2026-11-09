/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RK3588_NPU_H
#define _RK3588_NPU_H


/*!	The interface of the rk3588_npu driver: the neural processor of the
	Rockchip RK3588 (three cores of an NVDLA descendant, "RKNN").

	The device takes "register command" lists: tables of values for the
	registers of its convolution, post-processing and pooling units, one
	list per layer, that the user side makes for a network (see the
	rk3588_npu Neural Kit back end). The driver knows nothing of networks:
	it gives a client buffers the processor can reach, powers the core,
	runs the lists a client submits one after the other and says when they
	are done.

	Each open of the device has an address space of its own in the
	processor's IOMMU: a client's command lists can only read and write
	that client's buffers.
*/


#include <OS.h>
#include <Drivers.h>


#define RK3588_NPU_DEVICE_PATH		"/dev/npu/rk3588/0"

#define RK3588_NPU_INTERFACE_VERSION	1


enum {
	RK3588_NPU_GET_INFO = B_DEVICE_OP_CODES_END + 0x4e50,
	RK3588_NPU_CREATE_BUFFER,
	RK3588_NPU_DELETE_BUFFER,
	RK3588_NPU_SYNC_BUFFER,
	RK3588_NPU_SUBMIT
};


struct rk3588_npu_info {
	uint32		interface_version;
	//! of the hardware, as its version registers give it
	uint32		hardware_version;
	//! cores the driver uses
	uint32		core_count;
	//! frequency of the cores' clock in Hz
	uint32		clock_rate;
	uint32		_reserved[8];
};


/*!	Makes a buffer of memory that both the caller and the processor can
	use. \c area is an area in the caller's team that shows it (deleting
	the area does not free the buffer); \c device_address is where the
	processor sees it, valid only in command lists of this client. */
struct rk3588_npu_create_buffer {
	//! in: bytes, rounded up to whole pages
	uint64		size;
	//! out
	uint32		handle;
	uint32		_reserved;
	uint64		device_address;
	area_id		area;
	void*		address;
};


struct rk3588_npu_delete_buffer {
	uint32		handle;
};


//! rk3588_npu_sync_buffer::direction
enum {
	//! the caller wrote the buffer and the processor is to read it
	RK3588_NPU_SYNC_FOR_DEVICE = 1,
	//! the processor wrote the buffer and the caller is to read it
	RK3588_NPU_SYNC_FOR_CPU = 2
};

/*!	The processor does not see the CPU's caches and the CPU does not see
	what the processor wrote behind them: a buffer has to be synchronized
	after the CPU wrote to it before a job reads it, and after a job
	wrote to it before the CPU reads it. A size of 0 means to the end. */
struct rk3588_npu_sync_buffer {
	uint32		handle;
	uint32		direction;
	uint64		offset;
	uint64		size;
};


//! One command list: a layer, or a slice of one.
struct rk3588_npu_task {
	//! device address of the list (pairs of 64 bit commands)
	uint32		regcmd;
	//! number of commands in it
	uint32		regcmd_count;
};


/*!	Runs tasks one after the other on one core and returns when the last
	one is done, when one failed, or after the timeout. */
struct rk3588_npu_submit {
	const struct rk3588_npu_task* tasks;
	uint32		task_count;
	uint32		_reserved;
	//! microseconds for all tasks together; 0: the driver's default
	bigtime_t	timeout;

	// out
	uint32		tasks_done;
	//! raw interrupt status of the task that failed, 0 otherwise
	uint32		interrupt_status;
	//! time the processor took, in microseconds
	bigtime_t	duration;
};


#endif	// _RK3588_NPU_H
