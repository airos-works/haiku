/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _PVR_HAIKU_H
#define _PVR_HAIKU_H


/*	The PowerVR (Imagination Rogue) driver's interface to userland: Linux's
	pvr_drm.h and drm.h structures, unchanged, behind Haiku ioctl op codes
	PVR_HAIKU_OP(nr), nr being the Linux DRM ioctl number: the driver's own
	ioctls are 0x40-0x4d (DRM_COMMAND_BASE + DRM_PVR_*), the generic DRM
	ioctls keep theirs. The length argument of ioctl() is the size of the
	structure the request names (IOCPARM_LEN() of the Linux request code).
	As with drm_ioctl(), that many bytes are copied in, zero-extended to the
	driver's structure, and copied back out. Haiku-only ops use nr
	0xe0-0xef.

	A libdrm stand-in therefore needs no table:
		ioctl(fd, PVR_HAIKU_OP(request & 0xff), arg, IOCPARM_LEN(request)) */


#include <SupportDefs.h>


#define PVR_HAIKU_DEVICE_PATH		"/dev/graphics/powervr/0"
#define PVR_HAIKU_DEVICE_ENV		"HAIKU_PVR_DEVICE"

// 'P' 'V' 'R' nn
#define PVR_HAIKU_OP_BASE			0x50565200u
#define PVR_HAIKU_OP(nr)			(PVR_HAIKU_OP_BASE | ((nr) & 0xffu))
#define PVR_HAIKU_OP_NR(op)			((op) & 0xffu)
#define PVR_HAIKU_IS_OP(op)			(((op) & 0xffffff00u) == PVR_HAIKU_OP_BASE)

// generic DRM ioctls the driver answers (drm.h numbers and structures)
#define PVR_HAIKU_NR_VERSION				0x00	// struct drm_version
#define PVR_HAIKU_NR_GEM_CLOSE				0x09	// struct drm_gem_close
#define PVR_HAIKU_NR_GET_CAP				0x0c	// struct drm_get_cap
#define PVR_HAIKU_NR_SYNCOBJ_CREATE			0xbf
#define PVR_HAIKU_NR_SYNCOBJ_DESTROY		0xc0
#define PVR_HAIKU_NR_SYNCOBJ_WAIT			0xc3
#define PVR_HAIKU_NR_SYNCOBJ_RESET			0xc4
#define PVR_HAIKU_NR_SYNCOBJ_SIGNAL			0xc5
#define PVR_HAIKU_NR_SYNCOBJ_TIMELINE_WAIT	0xca
#define PVR_HAIKU_NR_SYNCOBJ_QUERY			0xcb
#define PVR_HAIKU_NR_SYNCOBJ_TRANSFER		0xcc
#define PVR_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL 0xcd

// the driver's own (pvr_drm.h)
#define PVR_HAIKU_NR_PVR_FIRST				0x40	// DRM_IOCTL_PVR_DEV_QUERY
#define PVR_HAIKU_NR_PVR_LAST				0x4d	// DRM_IOCTL_PVR_SUBMIT_JOBS

// Haiku-only
#define PVR_HAIKU_NR_MAP_BO			0xe0	// struct pvr_haiku_map_bo
#define PVR_HAIKU_NR_IMPORT_HOST	0xe1	// struct pvr_haiku_import_host
#define PVR_HAIKU_NR_STAGE			0xef	// struct pvr_haiku_stage

#define PVR_HAIKU_ABI_VERSION		3


/*	PVR_HAIKU_OP(PVR_HAIKU_NR_MAP_BO) replaces GET_BO_MMAP_OFFSET + mmap():
	the buffer's kernel area is cloned into the calling team, write-combined
	(the GPU is not cache coherent). With address 0 the clone goes anywhere,
	otherwise exactly there (B_EXACT_ADDRESS, for placed maps). Only buffers
	created with DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS can be mapped. Unmap
	with delete_area(area).

	With PVR_HAIKU_MAP_BO_CACHED the clone is cached (write-back) instead,
	for fast CPU reads of what the GPU wrote; the caller keeps it coherent
	itself: "dc cvac" over what the CPU wrote before the GPU reads it, "dc
	civac" over what it will read after the GPU wrote it (both allowed in
	userland). A caller that passes the old, shorter structure gets a
	write-combined clone. */
#define PVR_HAIKU_MAP_BO_CACHED		0x1u

struct pvr_haiku_map_bo {
	uint32	handle;					// in: buffer handle
	int32	area;					// out: the clone's area
	uint64	address;				// in: placement or 0; out: address
	uint64	size;					// out
	uint32	flags;					// in: PVR_HAIKU_MAP_BO_*
	uint32	reserved;
};


/*	PVR_HAIKU_OP(PVR_HAIKU_NR_IMPORT_HOST) makes a buffer object of the
	caller's own memory (VK_EXT_external_memory_host), such as another
	team's bitmap area cloned into it: [address, address + size) must be
	page aligned and lie within one area the caller can read and write.
	The pages stay locked, and the memory alive, until the handle is
	closed (GEM_CLOSE) and the GPU is done with it. The object cannot be
	mapped with MAP_BO; the caller already has it, cached as it is, and
	keeps it coherent as described for PVR_HAIKU_MAP_BO_CACHED. Only
	DRM_PVR_BO_BYPASS_DEVICE_CACHE is allowed in flags. Drivers without it
	answer B_DEV_INVALID_IOCTL. */
struct pvr_haiku_import_host {
	uint64	address;				// in
	uint64	size;					// in
	uint64	flags;					// in: DRM_PVR_BO_*
	uint32	handle;					// out: buffer handle
	uint32	reserved;
};


// stage
enum {
	PVR_HAIKU_STAGE_OFF = 0,		// disabled by the driver settings
	PVR_HAIKU_STAGE_POWERED,		// clocks, resets, power domains on
	PVR_HAIKU_STAGE_IDENTIFIED,		// the core's BVNC read
	PVR_HAIKU_STAGE_FIRMWARE		// the firmware runs and answers
};

// command
enum {
	PVR_HAIKU_STAGE_QUERY = 0,		// report only
	PVR_HAIKU_STAGE_HEALTH_CHECK,	// send a HEALTH_CHECK first (root)
	PVR_HAIKU_STAGE_DUMP,			// log registers, firmware state and
									// trace to the syslog first (root)
	PVR_HAIKU_STAGE_RESET,			// hard reset the GPU first (root)
	PVR_HAIKU_STAGE_JOB_TIMEOUT		// set job_timeout_ms first, for the
									// queues created from then on (root)
};

/*	PVR_HAIKU_OP(PVR_HAIKU_NR_STAGE): in: version and command; out: the
	rest. The firmware fields are zero until the firmware stage ran; it is
	off unless the driver settings file ("powervr") says "firmware true". */
struct pvr_haiku_stage {
	uint32	version;				// in: PVR_HAIKU_ABI_VERSION
	uint32	command;				// in: PVR_HAIKU_STAGE_QUERY, ...
	uint32	stage;					// PVR_HAIKU_STAGE_*
	uint32	core_id;
	uint64	bvnc;					// packed 16:16:16:16
	uint32	core_clock;				// Hz

	int32	firmware_status;		// B_OK, B_NO_INIT if the stage did not
									// run, or why it failed
	uint32	firmware_running;		// SYSINIT firmware_started seen
	uint32	fw_version_major;
	uint32	fw_version_minor;
	uint32	fw_version_build;
	uint64	fw_boot_time;			// microseconds, MIPS out of reset to
									// firmware_started
	uint32	health_checks;			// HEALTH_CHECKs the firmware executed
	uint32	health_check_failures;
	uint32	last_kccb_return;		// return slot of the last KCCB command
	uint32	kccb_cmds_executed;		// as the firmware counts them (OSDATA)
	uint32	irq_count;				// MIPS wrapper interrupts taken
	uint32	irq_spurious;			// GPU interrupts without one
	uint32	mips_exception_status;	// ROGUE_CR_MIPS_EXCEPTION_STATUS
	uint32	fw_faults;				// SYSDATA fw_faults
	uint32	resets;					// GPU resets that brought it back
	uint32	device_lost;			// 1 once the GPU is given up
	uint32	job_timeout_ms;			// in (JOB_TIMEOUT), out: a job queue
									// with no job finishing that long has
									// the GPU reset; 0 in: the default
	uint32	reserved32;
	uint64	reserved[2];
};


#endif	/* _PVR_HAIKU_H */
