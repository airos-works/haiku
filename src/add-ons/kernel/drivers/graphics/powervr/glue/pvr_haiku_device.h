/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_HAIKU_DEVICE_H
#define POWERVR_HAIKU_DEVICE_H


/*	The Haiku side of a PowerVR device, around drm/imagination's struct
	pvr_device; shared by the native C files in glue/. */


#include "pvr_device.h"
#include "pvr_haiku_glue.h"


struct rogue_fwif_kccb_cmd;


struct pvr_haiku_device {
	struct pvr_device		pvr;

	struct device			device;
	struct resource			registers;
	struct clk				core_clock;
	size_t					register_size;

	/*	Set once the GPU may have been left running on firmware memory: the
		memory is then kept, never freed, until the next boot. */
	bool					keep_memory;
	bool					firmware_loaded;
	bool					reset_released;		/* the MIPS left reset */
	bigtime_t				reset_release_time;
	bool					failed_start;	/* in pvr_haiku_fw_boot_failed() */

	atomic_t				irq_count;
	atomic_t				irq_spurious;

	/*	A reset runs (pvr_power_reset()): the interrupt thread leaves the
		firmware alone meanwhile. */
	atomic_t				resetting;
	u32						resets;
	struct work_struct		lost_work;		/* fails every job once lost */

	u32						health_checks;
	u32						health_check_failures;
	u32						last_kccb_return;
	status_t				firmware_status;
	bigtime_t				boot_time;
	u32						trace_lines;
	bool					verbose;	/* the settings asked for firmware
										   tracing: log more than errors */
};


static inline struct pvr_haiku_device*
to_haiku_device(struct pvr_device* pvr_dev)
{
	return container_of(pvr_dev, struct pvr_haiku_device, pvr);
}


/*	One open of the device: the DRM file (its driver_priv is upstream's
	struct pvr_file), the buffer handles and the sync objects. */
struct pvr_haiku_file {
	struct drm_file			drm_file;
	struct pvr_device*		pvr_dev;
	team_id					team;
	struct xarray			bo_handles;
	struct xarray			syncobjs;
};

static inline struct pvr_haiku_file*
to_haiku_file(struct pvr_file* pvr_file)
{
	return container_of(from_pvr_file(pvr_file), struct pvr_haiku_file,
		drm_file);
}


/* pvr_haiku_device.c */
int		pvr_haiku_kccb_execute(struct pvr_device* pvr_dev,
			struct rogue_fwif_kccb_cmd* command, const char* what,
			bool answers, u32* _return);
void	pvr_haiku_fw_boot_failed(struct pvr_device* pvr_dev);
void	pvr_haiku_force_reset(struct pvr_device* pvr_dev, const char* why);

/* pvr_haiku_drm.c: per ioctl calls and time, into the syslog */
void	pvr_haiku_ioctl_stats_dump(void);

/* pvr_haiku_gem.c */
struct pvr_gem_object*	pvr_haiku_gem_object_import(
							struct pvr_device* pvr_dev, const void* address,
							size_t size, u64 flags);

/* pvr_haiku_drv.c: pvr_drv.c's driver description (ioctls, open) */
const struct drm_driver*	pvr_haiku_drm_driver(void);

/* pvr_haiku_power.c */
int		pvr_haiku_power_off(struct pvr_device* pvr_dev);
void	pvr_haiku_power_init(struct pvr_device* pvr_dev);
void	pvr_haiku_job_times_dump(void);


#endif	/* POWERVR_HAIKU_DEVICE_H */
