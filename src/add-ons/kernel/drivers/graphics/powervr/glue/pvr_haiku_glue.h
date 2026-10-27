/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_HAIKU_GLUE_H
#define POWERVR_HAIKU_GLUE_H


/*	The interface between the driver's C++ code (PvrDevice) and the C side,
	where the reused drm/imagination code and its native glue live. Only
	plain types cross; struct pvr_device stays opaque to C++. */


#include <SupportDefs.h>


#ifdef __cplusplus
extern "C" {
#endif


struct pvr_device;
struct pvr_haiku_file;


typedef struct pvr_haiku_platform {
	volatile uint8*	registers;			// mapped control registers
	uint64			register_base;		// their physical address
	uint64			register_size;
	uint32			core_clock;			// Hz, as the GPU runs
} pvr_haiku_platform;

typedef struct pvr_haiku_firmware_options {
	uint32			trace_mask;			// firmware log groups, 0 for none
	bool			trace_registers;	// log the start's register accesses
	bool			vendor_secure_config;	// Allwinner's writes first
	uint32			trace_lines;		// firmware trace lines to log at most
} pvr_haiku_firmware_options;

typedef struct pvr_haiku_firmware_state {
	status_t		status;				// of the firmware stage
	bool			loaded;				// image read and accepted
	bool			running;			// firmware_started seen
	uint32			version_major;
	uint32			version_minor;
	uint32			version_build;
	bigtime_t		boot_time;			// MIPS reset to firmware_started
	uint32			health_checks;		// HEALTH_CHECKs executed
	uint32			health_check_failures;
	uint32			last_kccb_return;	// return slot of the last command
	uint32			kccb_cmds_executed;	// as the firmware counts them
	uint32			irq_count;			// MIPS wrapper interrupts taken
	uint32			irq_spurious;		// GPU interrupts without one
	uint32			mips_exception_status;
	uint32			fw_faults;
	uint32			resets;				// GPU resets that brought it back
	bool			lost;				// the GPU is given up
} pvr_haiku_firmware_state;


struct pvr_device*	pvr_haiku_device_create(
						const pvr_haiku_platform* platform);
void				pvr_haiku_device_delete(struct pvr_device* device);

uint64				pvr_haiku_identify(struct pvr_device* device);
bool				pvr_haiku_device_lost(struct pvr_device* device);

status_t			pvr_haiku_firmware_load(struct pvr_device* device);
void				pvr_haiku_clear_stale_interrupts(
						struct pvr_device* device);
status_t			pvr_haiku_firmware_boot(struct pvr_device* device,
						const pvr_haiku_firmware_options* options);
status_t			pvr_haiku_firmware_verify(struct pvr_device* device,
						const pvr_haiku_firmware_options* options);
status_t			pvr_haiku_health_check(struct pvr_device* device);
status_t			pvr_haiku_reset(struct pvr_device* device);
// how long a job queue may go without a job finishing before the GPU is
// reset; 0 for the default (pvr_queue.c's), for queues created later
void				pvr_haiku_set_job_timeout(uint32 milliseconds);
uint32				pvr_haiku_job_timeout(void);
void				pvr_haiku_firmware_shutdown(struct pvr_device* device);

void				pvr_haiku_dump(struct pvr_device* device, const char* why,
						uint32 traceLines);
void				pvr_haiku_firmware_state_get(struct pvr_device* device,
						pvr_haiku_firmware_state* state);

// interrupts: the hard handler acknowledges, the thread does the work
bool				pvr_haiku_interrupt(struct pvr_device* device);
void				pvr_haiku_interrupt_work(struct pvr_device* device);

// the opens of the device and their DRM ioctls (pvr_haiku_drm.c)
status_t			pvr_haiku_file_open(struct pvr_device* device,
						struct pvr_haiku_file** _file);
void				pvr_haiku_file_close(struct pvr_haiku_file* file);
status_t			pvr_haiku_file_ioctl(struct pvr_haiku_file* file,
						uint32 nr, void* buffer, size_t length);


#ifdef __cplusplus
}
#endif


#endif	/* POWERVR_HAIKU_GLUE_H */
