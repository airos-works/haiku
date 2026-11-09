/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef XDNA_INTERFACE_H
#define XDNA_INTERFACE_H


/*!	The interface of the amdxdna driver, as far as this back end uses it.

	The driver is Linux's (repository linux-drm, haiku/amdxdna): its ioctls
	are Linux's DRM ioctls with Linux's numbers and structures
	(include/uapi/drm/amdxdna_accel.h there is the reference; the names
	here are this file's own). What Linux does with mmap() is the driver's
	"map" ioctl on Haiku.
*/


#include <SupportDefs.h>


#define XDNA_DEVICE_PATH	"/dev/npu/amdxdna/0"

// Linux's ioctl numbers: direction (2 bits), size (14), type (8), number (8)
#define XDNA_IOC(direction, number, type) \
	(((uint32)(direction) << 30) | ((uint32)sizeof(type) << 16) \
		| ((uint32)'d' << 8) | (uint32)(number))
#define XDNA_IOC_WRITE		1
#define XDNA_IOC_READ		2
#define XDNA_IOC_BOTH		3

// what the driver means with "invalid address"
#define XDNA_INVALID_ADDRESS	(~(uint64)0)


// #pragma mark - DRM


struct xdna_gem_close {
	uint32	handle;
	uint32	pad;
};

struct xdna_drm_version {
	int32	version_major;
	int32	version_minor;
	int32	version_patchlevel;
	size_t	name_len;
	char*	name;
	size_t	date_len;
	char*	date;
	size_t	desc_len;
	char*	desc;
};

/*!	A sync object: what the NPUs of today's processors signal the end of
	a command on. Each hardware context has one; a command's sequence number
	is its point on it. The timeout is a time of the monotonic clock
	(system_time()) in nanoseconds. */
struct xdna_syncobj_destroy {
	uint32	handle;
	uint32	pad;
};

struct xdna_syncobj_timeline_wait {
	uint64	handles;			// uint32[count_handles]
	uint64	points;				// uint64[count_handles]
	int64	timeout_nsec;
	uint32	count_handles;
	uint32	flags;
	uint32	first_signaled;
	uint32	pad;
	uint64	deadline_nsec;
};

#define XDNA_SYNCOBJ_WAIT_FOR_SUBMIT	(1 << 1)

#define XDNA_IOCTL_VERSION \
	XDNA_IOC(XDNA_IOC_BOTH, 0x00, xdna_drm_version)
#define XDNA_IOCTL_GEM_CLOSE \
	XDNA_IOC(XDNA_IOC_WRITE, 0x09, xdna_gem_close)
#define XDNA_IOCTL_SYNCOBJ_DESTROY \
	XDNA_IOC(XDNA_IOC_BOTH, 0xc0, xdna_syncobj_destroy)
#define XDNA_IOCTL_SYNCOBJ_TIMELINE_WAIT \
	XDNA_IOC(XDNA_IOC_BOTH, 0xca, xdna_syncobj_timeline_wait)


// #pragma mark - Haiku additions (drm_haiku.h)


struct xdna_map {
	uint64	offset;		// in: the buffer's map_offset
	uint64	size;		// in: bytes, page aligned
	uint32	flags;		// in: XDNA_MAP_WRITE
	int32	area;		// out
	uint64	address;	// in: wanted address or 0; out: the address
};

struct xdna_unmap {
	uint64	address;
};

#define XDNA_MAP_WRITE		0x1

#define XDNA_IOCTL_MAP		XDNA_IOC(XDNA_IOC_BOTH, 0xf0, xdna_map)
#define XDNA_IOCTL_UNMAP	XDNA_IOC(XDNA_IOC_WRITE, 0xf1, xdna_unmap)


// #pragma mark - amdxdna


struct xdna_qos_info {
	uint32	gops;
	uint32	fps;
	uint32	dma_bandwidth;
	uint32	latency;
	uint32	frame_exec_time;
	uint32	priority;
};

#define XDNA_QOS_NORMAL_PRIORITY	0x200

struct xdna_create_hwctx {
	uint64	ext;
	uint64	ext_flags;
	uint64	qos_p;
	uint32	umq_bo;
	uint32	log_buf_bo;
	uint32	max_opc;
	uint32	num_tiles;
	uint32	mem_size;
	uint32	umq_doorbell;
	uint32	handle;				// out
	uint32	syncobj_handle;		// out
};

struct xdna_destroy_hwctx {
	uint32	handle;
	uint32	pad;
};

struct xdna_cu_config {
	uint32	cu_bo;
	uint8	cu_func;
	uint8	pad[3];
};

//! One compute unit: all this back end configures.
struct xdna_config_cu {
	uint16	num_cus;
	uint16	pad[3];
	xdna_cu_config cu;
};

#define XDNA_HWCTX_CONFIG_CU	0

struct xdna_config_hwctx {
	uint32	handle;
	uint32	param_type;
	uint64	param_val;
	uint32	param_val_size;
	uint32	pad;
};

enum {
	XDNA_BO_SHARE = 1,		// memory of its own: data
	XDNA_BO_DEV_HEAP = 2,	// a client's device heap
	XDNA_BO_DEV = 3,		// a range of the heap: configuration, instructions
	XDNA_BO_CMD = 4			// a command
};

struct xdna_create_bo {
	uint64	flags;
	uint64	vaddr;
	uint64	size;
	uint32	type;
	uint32	handle;				// out
};

struct xdna_get_bo_info {
	uint64	ext;
	uint64	ext_flags;
	uint32	handle;
	uint32	pad;
	uint64	map_offset;			// out
	uint64	vaddr;				// out
	uint64	xdna_addr;			// out: the address the NPU uses
};

struct xdna_sync_bo {
	uint32	handle;
	uint32	direction;			// 0 to the device, 1 from it
	uint64	offset;
	uint64	size;
};

struct xdna_exec_cmd {
	uint64	ext;
	uint64	ext_flags;
	uint32	hwctx;
	uint32	type;				// 0: one command buffer
	uint64	cmd_handles;
	uint64	args;
	uint32	cmd_count;
	uint32	arg_count;
	uint64	seq;				// out
};

//! (EOPNOTSUPP on the NPUs seen so far; see xdna_syncobj_timeline_wait)
struct xdna_wait_cmd {
	uint32	hwctx;
	uint32	timeout;			// ms, 0: for ever
	uint64	seq;
};

struct xdna_get_info {
	uint32	param;
	uint32	buffer_size;
	uint64	buffer;
};

enum {
	XDNA_QUERY_AIE_METADATA = 1,
	XDNA_QUERY_AIE_VERSION = 2,
	XDNA_QUERY_FIRMWARE_VERSION = 8
};

struct xdna_aie_version {
	uint32	major;
	uint32	minor;
};

struct xdna_aie_tile_metadata {
	uint16	row_count;
	uint16	row_start;
	uint16	dma_channel_count;
	uint16	lock_count;
	uint16	event_reg_count;
	uint16	pad[3];
};

struct xdna_aie_metadata {
	uint32	col_size;
	uint16	cols;
	uint16	rows;
	xdna_aie_version version;
	xdna_aie_tile_metadata core;
	xdna_aie_tile_metadata mem;
	xdna_aie_tile_metadata shim;
};

struct xdna_firmware_version {
	uint32	major;
	uint32	minor;
	uint32	patch;
	uint32	build;
};

#define XDNA_IOCTL_CREATE_HWCTX \
	XDNA_IOC(XDNA_IOC_BOTH, 0x40, xdna_create_hwctx)
#define XDNA_IOCTL_DESTROY_HWCTX \
	XDNA_IOC(XDNA_IOC_BOTH, 0x41, xdna_destroy_hwctx)
#define XDNA_IOCTL_CONFIG_HWCTX \
	XDNA_IOC(XDNA_IOC_BOTH, 0x42, xdna_config_hwctx)
#define XDNA_IOCTL_CREATE_BO \
	XDNA_IOC(XDNA_IOC_BOTH, 0x43, xdna_create_bo)
#define XDNA_IOCTL_GET_BO_INFO \
	XDNA_IOC(XDNA_IOC_BOTH, 0x44, xdna_get_bo_info)
#define XDNA_IOCTL_SYNC_BO \
	XDNA_IOC(XDNA_IOC_BOTH, 0x45, xdna_sync_bo)
#define XDNA_IOCTL_EXEC_CMD \
	XDNA_IOC(XDNA_IOC_BOTH, 0x46, xdna_exec_cmd)
#define XDNA_IOCTL_GET_INFO \
	XDNA_IOC(XDNA_IOC_BOTH, 0x47, xdna_get_info)
#define XDNA_IOCTL_WAIT_CMD \
	XDNA_IOC(XDNA_IOC_WRITE, 0x49, xdna_wait_cmd)


// #pragma mark - commands


/*!	A command buffer: a header and words. For the "start compute unit"
	command the first word is the mask of compute units, the rest the
	kernel's arguments as the firmware reads them (see Program.cpp). */
#define XDNA_CMD_HEADER(state, count, opcode) \
	((uint32)(state) | ((uint32)(count) << 12) | ((uint32)(opcode) << 23))
#define XDNA_CMD_STATE(header)		((header) & 0xf)

#define XDNA_CMD_START_CU			0
#define XDNA_CMD_STATE_NEW			1
#define XDNA_CMD_STATE_COMPLETED	4

//! the device heap comes in pieces of this size
#define XDNA_HEAP_SIZE				((size_t)64 << 20)


#endif	// XDNA_INTERFACE_H
