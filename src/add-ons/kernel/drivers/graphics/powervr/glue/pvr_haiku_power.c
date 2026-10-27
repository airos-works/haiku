/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	drm/imagination's pvr_power.c, as far as a GPU that stays powered needs
	it: the power-off handshake before the firmware stops, the soft and hard
	resets (pvr_power_reset(), written after Linux's), the device-lost
	state, and the reset the scheduler asks for when no job finishes for its
	timeout (pvr_queue.c's own timeout handler leaves recovery to the
	firmware, which does not end a job that runs forever).

	A hard reset stops the firmware (or, if a hung GPU never goes idle,
	puts it into soft reset anyway), reinitialises the firmware's memory and
	starts it again. Jobs that had not finished end with an error, and so
	does every context that existed before: their client CCBs and firmware
	contexts start over, so what they would submit next is not trusted.
	New contexts work. If the firmware does not come back, the device is
	lost: every job fails, the opens' ioctls answer EIO, new opens get no
	device (Mesa then falls back to software), and the GPU's memory is kept
	until the next boot. */


#include "pvr_haiku_glue.h"

#include "pvr_ccb.h"
#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_startstop.h"
#include "pvr_haiku_device.h"
#include "pvr_power.h"
#include "pvr_queue.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_fwif.h"


#define TRACE(x...)	dprintf("powervr: " x)

#define POWER_SYNC_TIMEOUT_US	1000000


/* #pragma mark - device lost */


/*!	Fails every job of every queue, on the scheduler work queue (where
	pvr_queue.c's own work runs) since pvr_device_lost() can be called with
	any lock held.
*/
static void
lost_work(struct work_struct* work)
{
	struct pvr_haiku_device* device = container_of(work,
		struct pvr_haiku_device, lost_work);
	struct pvr_device* pvr_dev = &device->pvr;
	struct pvr_queue* queue;

	mutex_lock(&pvr_dev->queues.lock);
	list_for_each_entry(queue, &pvr_dev->queues.active, node)
		lx_sched_kill_all(&queue->scheduler, -ENODEV);
	list_for_each_entry(queue, &pvr_dev->queues.idle, node)
		lx_sched_kill_all(&queue->scheduler, -ENODEV);
	mutex_unlock(&pvr_dev->queues.lock);
}


void
pvr_device_lost(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	if (pvr_dev->lost)
		return;

	pvr_dev->lost = true;
	drm_dev_unplug(from_pvr_device(pvr_dev));
	// the GPU may still read or write what it was given
	device->keep_memory = true;
	drm_err(from_pvr_device(pvr_dev), "GPU device lost: jobs fail, new opens"
		" get no device, its memory is kept until the next boot\n");
	if (pvr_dev->sched_wq != NULL)
		queue_work(pvr_dev->sched_wq, &device->lost_work);
}


/* #pragma mark - resets */


/*!	Every context there is ends: what it submits from now on fails. */
static void
fail_contexts(struct pvr_device* pvr_dev)
{
	unsigned long index;
	struct pvr_context* ctx;
	u32 count = 0;

	xa_lock(&pvr_dev->ctx_ids);
	xa_for_each(&pvr_dev->ctx_ids, index, ctx) {
		atomic_set(&ctx->faulty, 1);
		count++;
	}
	xa_unlock(&pvr_dev->ctx_ids);
	if (count > 0)
		TRACE("reset: %u context(s) marked faulty\n", count);
}


static int
fw_disable(struct pvr_device* pvr_dev, bool hard_reset)
{
	if (!hard_reset) {
		int error = pvr_haiku_power_off(pvr_dev);
		if (error != 0)
			return error;
	}
	return pvr_fw_stop(pvr_dev);
}


static int
fw_enable(struct pvr_device* pvr_dev)
{
	int error = pvr_fw_start(pvr_dev);
	if (error != 0)
		return error;

	error = pvr_wait_for_fw_boot(pvr_dev);
	if (error != 0) {
		drm_err(from_pvr_device(pvr_dev), "Firmware failed to boot\n");
		pvr_fw_stop(pvr_dev);
	}
	return error;
}


/*!	The soft reset bits, without waiting for the GPU to go idle first, as
	the power-off of a runtime suspend would cut it (Linux's hard reset
	powers the GPU off at this point).
*/
static void
force_soft_reset(struct pvr_device* pvr_dev)
{
	u64 mask = PVR_HAS_FEATURE(pvr_dev, pbe2_in_xe)
		? ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL : ROGUE_CR_SOFT_RESET_MASKFULL;
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, mask);
	if (PVR_HAS_FEATURE(pvr_dev, xe_tpu2)) {
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET2,
			ROGUE_CR_SOFT_RESET2_MASKFULL);
	}
	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
}


int
pvr_power_reset(struct pvr_device* pvr_dev, bool hard_reset)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	bool queues_disabled = false;
	int error;

	down_write(&pvr_dev->reset_sem);
	if (pvr_dev->lost) {
		up_write(&pvr_dev->reset_sem);
		return -EIO;
	}

	bigtime_t start = system_time();
	atomic_set(&device->resetting, 1);
	TRACE("reset: %s, #%u\n", hard_reset ? "hard" : "soft",
		device->resets + 1);
	if (device->resets == 0)
		pvr_haiku_dump(pvr_dev, "before the first reset", 32);

	do {
		if (hard_reset && !queues_disabled) {
			pvr_queue_device_pre_reset(pvr_dev);
			queues_disabled = true;
		}

		error = fw_disable(pvr_dev, hard_reset);
		if (error != 0 && hard_reset) {
			// a hung GPU need not go idle: reset it all the same
			TRACE("reset: the firmware did not stop (%d), forcing the soft"
				" reset\n", error);
			force_soft_reset(pvr_dev);
			error = 0;
		}

		if (error == 0) {
			if (hard_reset) {
				WRITE_ONCE(pvr_dev->fw_dev.initialised, false);
				error = pvr_fw_hard_reset(pvr_dev);
				WRITE_ONCE(pvr_dev->fw_dev.initialised, true);
				if (error != 0)
					goto device_lost;
			} else {
				// clear the firmware's fault flags
				pvr_dev->fw_dev.fwif_sysdata->hwr_state_flags
					&= ~(ROGUE_FWIF_HWR_FW_FAULT
						| ROGUE_FWIF_HWR_RESTART_REQUESTED);
			}

			pvr_fw_irq_clear(pvr_dev);
			error = fw_enable(pvr_dev);
		}

		if (error != 0 && hard_reset)
			goto device_lost;
		if (error != 0) {
			drm_err(from_pvr_device(pvr_dev), "FW stalled, trying hard"
				" reset\n");
			hard_reset = true;
		}
	} while (error != 0);

	if (queues_disabled) {
		fail_contexts(pvr_dev);
		pvr_queue_device_post_reset(pvr_dev);
	}

	device->resets++;
	atomic_set(&device->resetting, 0);
	up_write(&pvr_dev->reset_sem);
	TRACE("reset: done in %" B_PRIdBIGTIME " us, the firmware runs again\n",
		system_time() - start);
	return 0;

device_lost:
	pvr_device_lost(pvr_dev);
	pvr_haiku_dump(pvr_dev, "the reset failed", 32);
	if (queues_disabled)
		pvr_queue_device_post_reset(pvr_dev);
	// interrupts stay ignored ("resetting") for good
	up_write(&pvr_dev->reset_sem);
	return error;
}


/*!	The scheduler's timeout: no job of \a sched finished for a minute
	(pvr_queue.c's SCHED_TIMEOUT_PERIOD) while \a job was on the GPU.
*/
static void
scheduler_timeout(struct drm_gpu_scheduler* sched, struct drm_sched_job* job)
{
	struct pvr_queue* queue = container_of(sched, struct pvr_queue,
		scheduler);
	struct pvr_device* pvr_dev = queue->ctx->pvr_dev;
	(void)job;

	drm_err(from_pvr_device(pvr_dev), "job timeout on a %s queue (context"
		" %u): resetting the GPU\n", queue->type == DRM_PVR_JOB_TYPE_COMPUTE
			? "compute" : queue->type == DRM_PVR_JOB_TYPE_FRAGMENT
			? "fragment" : queue->type == DRM_PVR_JOB_TYPE_GEOMETRY
			? "geometry" : "transfer", queue->ctx->ctx_id);
	pvr_power_reset(pvr_dev, true);
}


status_t
pvr_haiku_reset(struct pvr_device* pvr_dev)
{
	return lx_status(pvr_power_reset(pvr_dev, true));
}


void
pvr_haiku_set_job_timeout(uint32 milliseconds)
{
	lx_sched_timeout_override_ms = milliseconds;
	TRACE("job timeout for new queues: %s%" B_PRIu32 " ms\n",
		milliseconds == 0 ? "the default, not " : "", milliseconds);
}


uint32
pvr_haiku_job_timeout(void)
{
	return lx_sched_timeout_override_ms;
}


/*	Per job type: jobs done and the time from run_job() (into the firmware's
	queue) to done, which is the upper bound of what the GPU spent on them.
	For pvrinfo --dump. */
static struct {
	int64	count;
	int64	time;
	int64	max_time;
} sJobTimes[4];


static void
count_job_time(struct drm_gpu_scheduler* sched, bigtime_t elapsed)
{
	struct pvr_queue* queue = container_of(sched, struct pvr_queue,
		scheduler);
	u32 type = queue->type;
	if (type >= ARRAY_SIZE(sJobTimes))
		return;
	atomic_add64(&sJobTimes[type].count, 1);
	atomic_add64(&sJobTimes[type].time, elapsed);
	if (elapsed > atomic_get64(&sJobTimes[type].max_time))
		atomic_set64(&sJobTimes[type].max_time, elapsed);
}


void
pvr_haiku_job_times_dump(void)
{
	static const char* const kNames[] = {
		"geometry", "fragment", "compute", "transfer"
	};
	for (u32 i = 0; i < ARRAY_SIZE(sJobTimes); i++) {
		int64 count = atomic_get64(&sJobTimes[i].count);
		if (count == 0)
			continue;
		int64 time = atomic_get64(&sJobTimes[i].time);
		dprintf("powervr: dump: jobs %s: %" B_PRId64 " done, %" B_PRId64
			" us, %" B_PRId64 " us each, longest %" B_PRId64 " us\n",
			kNames[i], count, time, time / count,
			atomic_get64(&sJobTimes[i].max_time));
	}
}


void
pvr_haiku_power_init(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	atomic_set(&device->resetting, 0);
	INIT_WORK(&device->lost_work, lost_work);
	lx_sched_timeout_hook = scheduler_timeout;
	lx_sched_job_time_hook = count_job_time;
}


static int
pvr_haiku_power_send_command(struct pvr_device* pvr_dev,
	struct rogue_fwif_kccb_cmd* command)
{
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	u32 slot;
	u32 value;

	WRITE_ONCE(*fw_dev->power_sync, 0);

	int error = pvr_kccb_send_cmd_powered(pvr_dev, command, &slot);
	if (error != 0)
		return error;

	// the firmware acknowledges through the power sync object
	return readl_poll_timeout(fw_dev->power_sync, value, value != 0, 100,
		POWER_SYNC_TIMEOUT_US);
}


/*!	Forced idle, then power off, as before a runtime suspend: afterwards
	pvr_fw_stop() finds the LAYOUT_MARS core's CPU idle.
*/
int
pvr_haiku_power_off(struct pvr_device* pvr_dev)
{
	struct rogue_fwif_kccb_cmd command = {};

	command.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	command.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_FORCED_IDLE_REQ;
	command.cmd_data.pow_data.power_req_data.pow_request_type
		= ROGUE_FWIF_POWER_FORCE_IDLE;
	int error = pvr_haiku_power_send_command(pvr_dev, &command);
	if (error != 0) {
		drm_err(from_pvr_device(pvr_dev), "forced idle request: %d\n",
			error);
		return error;
	}

	memset(&command, 0, sizeof(command));
	command.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	command.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_OFF_REQ;
	command.cmd_data.pow_data.power_req_data.forced = true;
	error = pvr_haiku_power_send_command(pvr_dev, &command);
	if (error != 0)
		drm_err(from_pvr_device(pvr_dev), "power off request: %d\n", error);
	return error;
}
