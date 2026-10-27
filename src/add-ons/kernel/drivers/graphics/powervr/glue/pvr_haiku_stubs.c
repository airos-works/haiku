/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	Functions the reused files refer to for cases this GPU never reaches:
	the META and RISC-V firmware processors (the BXM-4-64 runs its firmware
	on MIPS; pvr_fw_meta.c and pvr_fw_riscv.c map their code through a
	kernel GPU VM context, which a MIPS core does not have), and the DRM
	core's debugfs. Each says so if it runs. */


#include "pvr_device.h"
#include "pvr_debugfs.h"
#include "pvr_fw.h"
#include "pvr_fw_meta.h"


static int
pvr_haiku_unsupported_init(struct pvr_device* pvr_dev)
{
	drm_err(from_pvr_device(pvr_dev), "only MIPS firmware processors are"
		" supported\n");
	return -ENODEV;
}


const struct pvr_fw_defs pvr_fw_defs_meta = {
	.init = pvr_haiku_unsupported_init,
};

const struct pvr_fw_defs pvr_fw_defs_riscv = {
	.init = pvr_haiku_unsupported_init,
};


int
pvr_meta_cr_read32(struct pvr_device* pvr_dev, u32 reg_addr,
	u32* reg_value_out)
{
	(void)reg_addr;
	*reg_value_out = 0;
	drm_err(from_pvr_device(pvr_dev), "no META firmware processor\n");
	return -ENODEV;
}


/*	The DRM core calls this for the device's debugfs directory, which Haiku
	does not have; the driver reads what it needs (the firmware trace)
	through lx_debugfs_dump() instead. */
void
pvr_debugfs_init(struct drm_minor* minor)
{
	(void)minor;
}
