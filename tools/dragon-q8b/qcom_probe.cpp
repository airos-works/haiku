/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab probe for the Radxa Dragon Q8B (SC8280XP): logs what the firmware
	leaves running of the GPU, display, video and SD blocks. A block's own
	registers are only read once the clocks and power domain in front of it
	read as on: touching an unclocked block hangs the SoC. Linked into
	drivers/dev/misc by hand; publishes nothing. */


#include <Drivers.h>
#include <KernelExport.h>
#include <arch/atomic.h>


int32 api_version = B_CUR_DRIVER_API_VERSION;


struct region {
	const char*	name;
	phys_addr_t	base;
	size_t		size;
};


static volatile uint8*
map(const region& r, area_id& area)
{
	volatile uint8* address = NULL;
	area = map_physical_memory(r.name, r.base, r.size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA, (void**)&address);
	return area < 0 ? NULL : address;
}


static inline uint32
rd(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


// CBCR: bit 31 = clock off; GDSC: bit 31 = power on
static bool
clock_on(volatile uint8* cc, uint32 offset)
{
	return (rd(cc, offset) & (1u << 31)) == 0;
}


static volatile uint8*
map_rw(const region& r, area_id& area)
{
	volatile uint8* address = NULL;
	area = map_physical_memory(r.name, r.base, r.size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&address);
	return area < 0 ? NULL : address;
}


static inline void
wr(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


static bool
wait_bits(volatile uint8* base, uint32 offset, uint32 mask, uint32 value)
{
	for (int i = 0; i < 10000; i++) {
		if ((rd(base, offset) & mask) == value)
			return true;
		spin(10);
	}
	return false;
}


static void
enable_branch(volatile uint8* cc, uint32 offset, bool check, const char* name)
{
	wr(cc, offset, rd(cc, offset) | 1);
	if (check && !wait_bits(cc, offset, 1u << 31, 0)) {
		dprintf("qcom_probe: %s does not start (%#" B_PRIx32 ")\n", name,
			rd(cc, offset));
	}
}


/*!	What Linux' GMU resume does before the GMU runs: GCC's GPU clocks, the
	GPU clock controller's GMU and hub clocks at 200 MHz, the CX power
	domain. Then the CX side (GMU, GPU SMMU) can be read.
*/
static void
gpu_power_up()
{
	area_id gccArea, gpuccArea;
	volatile uint8* gcc = map_rw({"probe gcc", 0x100000, 0x1f4200}, gccArea);
	volatile uint8* gpucc = map_rw({"probe gpucc", 0x3d90000, 0x9000},
		gpuccArea);
	if (gcc == NULL || gpucc == NULL)
		return;

	// GPLL0 and GPLL0/2 for the GPU clock controller
	wr(gcc, 0x52000, rd(gcc, 0x52000) | 1u << 15 | 1u << 16);
	enable_branch(gcc, 0x71010, true, "gcc_gpu_memnoc_gfx");
	enable_branch(gcc, 0x71020, true, "gcc_gpu_snoc_dvm_gfx");
	enable_branch(gcc, 0x7115c, false, "gcc_ddrss_gpu_axi");

	// CX GDSC: on, flip-flops retained (status in its hardware control
	// register, as it is votable)
	wr(gpucc, 0x106c, (rd(gpucc, 0x106c) & ~1u) | 1u << 11);
	if (!wait_bits(gpucc, 0x1540, 1u << 31, 1u << 31)) {
		dprintf("qcom_probe: CX GDSC does not power on (%#" B_PRIx32 " / %#"
			B_PRIx32 ")\n", rd(gpucc, 0x106c), rd(gpucc, 0x1540));
	}

	// RCGs: GMU = GPLL0/2 / 1.5, hub = GPLL0 / 3, both 200 MHz
	wr(gpucc, 0x1124, 6u << 8 | 2u);
	wr(gpucc, 0x1120, rd(gpucc, 0x1120) | 1);
	if (!wait_bits(gpucc, 0x1120, 1, 0))
		dprintf("qcom_probe: GMU clock source does not update\n");
	wr(gpucc, 0x1180, 5u << 8 | 5u);
	wr(gpucc, 0x117c, rd(gpucc, 0x117c) | 1);
	if (!wait_bits(gpucc, 0x117c, 1, 0))
		dprintf("qcom_probe: hub clock source does not update\n");

	enable_branch(gpucc, 0x109c, true, "gpu_cc_cxo");
	enable_branch(gpucc, 0x1170, false, "gpu_cc_cb");
	enable_branch(gpucc, 0x1078, false, "gpu_cc_ahb");
	enable_branch(gpucc, 0x1098, true, "gpu_cc_cx_gmu");
	enable_branch(gpucc, 0x1204, true, "gpu_cc_hub_cx_int");
	enable_branch(gpucc, 0x1178, true, "gpu_cc_hub_aon");
	enable_branch(gpucc, 0x108c, false, "gpu_cc_cx_snoc_dvm");
	enable_branch(gpucc, 0x5000, false, "gpu_cc_hlos1_vote_gpu_smmu");
	spin(100);

	dprintf("qcom_probe: after power up: cx gdsc %#" B_PRIx32 "/%#" B_PRIx32
		" gmu rcg %#" B_PRIx32 "/%#" B_PRIx32 " cx_gmu %#" B_PRIx32 " ahb %#"
		B_PRIx32 " hub %#" B_PRIx32 " smmu vote %#" B_PRIx32 "\n",
		rd(gpucc, 0x106c), rd(gpucc, 0x1540), rd(gpucc, 0x1120),
		rd(gpucc, 0x1124), rd(gpucc, 0x1098), rd(gpucc, 0x1078),
		rd(gpucc, 0x1204), rd(gpucc, 0x5000));
	bool cxOn = (rd(gpucc, 0x1540) & (1u << 31)) != 0
		&& clock_on(gpucc, 0x1098);
	delete_area(gpuccArea);
	delete_area(gccArea);
	if (!cxOn)
		return;

	area_id area;
	volatile uint8* smmu = map({"probe gpu smmu", 0x3da0000, 0x20000}, area);
	if (smmu != NULL) {
		dprintf("qcom_probe: gpu smmu sCR0 %#" B_PRIx32 " IDR0 %#" B_PRIx32
			" IDR1 %#" B_PRIx32 " IDR2 %#" B_PRIx32 " IDR7 %#" B_PRIx32 "\n",
			rd(smmu, 0), rd(smmu, 0x20), rd(smmu, 0x24), rd(smmu, 0x28),
			rd(smmu, 0x3c));
		uint32 groups = rd(smmu, 0x20) & 0xff;
		for (uint32 i = 0; i < groups; i++) {
			dprintf("qcom_probe: gpu smmu SMR%" B_PRIu32 " %#" B_PRIx32 " S2CR %#"
				B_PRIx32 "\n", i, rd(smmu, 0x800 + 4 * i),
				rd(smmu, 0xc00 + 4 * i));
		}
		uint32 banks = rd(smmu, 0x24) & 0xff;
		for (uint32 i = 0; i < banks; i++) {
			volatile uint8* cb = smmu + 0x10000 + 0x1000 * i;
			dprintf("qcom_probe: gpu smmu CB%" B_PRIu32 " CBAR %#" B_PRIx32
				" CBA2R %#" B_PRIx32 " SCTLR %#" B_PRIx32 " TCR %#" B_PRIx32
				" TTBR0 %#" B_PRIx32 "%08" B_PRIx32 " FSR %#" B_PRIx32 "\n", i,
				rd(smmu, 0x1000 + 4 * i), rd(smmu, 0x1800 + 4 * i), rd(cb, 0),
				rd(cb, 0x30), rd(cb, 0x24), rd(cb, 0x20), rd(cb, 0x58));
		}
		delete_area(area);
	}
	volatile uint8* gmu = map({"probe gmu", 0x3d6a000, 0x34000}, area);
	if (gmu != NULL) {
		// register offsets are in 32 bit words from the GPU's base
		auto reg = [](uint32 dword) { return dword * 4 - 0x6a000; };
		dprintf("qcom_probe: gmu sysreset %#" B_PRIx32 " fw init %#" B_PRIx32
			" hfi status %#" B_PRIx32 " general0 %#" B_PRIx32 " cx busy %#"
			B_PRIx32 "\n", rd(gmu, reg(0x1f800)), rd(gmu, reg(0x1f81c)),
			rd(gmu, reg(0x1f980)), rd(gmu, reg(0x1f9c5)),
			rd(gmu, reg(0x23b0c)));
		delete_area(area);
	}
}


static void
probe()
{
	area_id gccArea;
	volatile uint8* gcc = map({"probe gcc", 0x100000, 0x1f4200}, gccArea);
	if (gcc == NULL)
		return;

	static const struct { const char* name; uint32 offset; } kBranches[] = {
		{"gpu_cfg_ahb", 0x71004}, {"gpu_memnoc_gfx", 0x71010},
		{"gpu_snoc_dvm_gfx", 0x71020}, {"gpu_tcu_throttle_ahb", 0x71008},
		{"disp_ahb", 0x27004}, {"disp_hf_axi", 0x27010},
		{"disp_sf_axi", 0x27018}, {"disp1_ahb", 0xbb004},
		{"disp1_hf_axi", 0xbb010}, {"video_ahb", 0x28004},
		{"video_axi0", 0x28010}, {"video_axi1", 0x28018},
		{"sdcc2_ahb", 0x14008}, {"sdcc2_apps", 0x14004},
		{"mmnoc_tbu_hf0 gdsc", 0x7d050}, {"mmnoc_tbu_hf1 gdsc", 0x7d058},
		{"mmnoc_tbu_sf0 gdsc", 0x7d054},
	};
	for (size_t i = 0; i < B_COUNT_OF(kBranches); i++) {
		dprintf("qcom_probe: gcc %-20s %#010" B_PRIx32 "\n", kBranches[i].name,
			rd(gcc, kBranches[i].offset));
	}
	bool gpuAhb = clock_on(gcc, 0x71004);
	bool dispAhb = clock_on(gcc, 0x27004);
	bool sdAhb = clock_on(gcc, 0x14008);
	delete_area(gccArea);

	if (gpuAhb) {
		area_id area;
		volatile uint8* gpucc = map({"probe gpucc", 0x3d90000, 0x9000}, area);
		if (gpucc != NULL) {
			uint32 cx = rd(gpucc, 0x106c), gx = rd(gpucc, 0x100c);
			dprintf("qcom_probe: gpucc cx gdsc %#" B_PRIx32 " gx gdsc %#" B_PRIx32
				" ahb %#" B_PRIx32 " cx_gmu %#" B_PRIx32 " hub_aon %#" B_PRIx32
				"\n", cx, gx, rd(gpucc, 0x1078), rd(gpucc, 0x1098),
				rd(gpucc, 0x1178));
			bool cxOn = (cx & (1u << 31)) != 0 && clock_on(gpucc, 0x1078);
			delete_area(area);
			if (cxOn) {
				volatile uint8* smmu = map({"probe gpu smmu", 0x3da0000,
					0x1000}, area);
				if (smmu != NULL) {
					dprintf("qcom_probe: gpu smmu sCR0 %#" B_PRIx32 " IDR0 %#"
						B_PRIx32 " IDR1 %#" B_PRIx32 "\n", rd(smmu, 0),
						rd(smmu, 0x20), rd(smmu, 0x24));
					delete_area(area);
				}
			} else
				dprintf("qcom_probe: GPU CX off, its registers left alone\n");
		}
		gpu_power_up();
	} else
		dprintf("qcom_probe: gpu_cfg_ahb off, GPU clock controller left alone\n");

	if (dispAhb) {
		area_id area;
		volatile uint8* dispcc = map({"probe dispcc0", 0xaf00000, 0x20000},
			area);
		if (dispcc != NULL) {
			uint32 gdsc = rd(dispcc, 0x3000);
			dprintf("qcom_probe: dispcc0 mdss gdsc %#" B_PRIx32 " ahb %#" B_PRIx32
				" mdp %#" B_PRIx32 " dptx0 link %#" B_PRIx32 " pixel %#" B_PRIx32
				" dptx1 link %#" B_PRIx32 " pixel %#" B_PRIx32 " dptx2 link %#"
				B_PRIx32 "\n", gdsc, rd(dispcc, 0x20bc), rd(dispcc, 0x200c),
				rd(dispcc, 0x205c), rd(dispcc, 0x2070), rd(dispcc, 0x2084),
				rd(dispcc, 0x2078), rd(dispcc, 0x2090));
			bool mdssOn = (gdsc & (1u << 31)) != 0 && clock_on(dispcc, 0x20bc)
				&& clock_on(dispcc, 0x200c);
			delete_area(area);
			if (mdssOn) {
				volatile uint8* mdss = map({"probe mdss", 0xae00000, 0x100000},
					area);
				if (mdss != NULL) {
					dprintf("qcom_probe: mdss hw version %#" B_PRIx32
						", dpu hw version %#" B_PRIx32 "\n", rd(mdss, 0x0),
						rd(mdss, 0x1000));
					// DPU INTF blocks at 0x34000 + 0x1000 * n (DPU 8.x): timing
					// engine enable at +0x0
					for (int i = 0; i < 9; i++) {
						dprintf("qcom_probe: intf %d enable %#" B_PRIx32
							" hsync %#" B_PRIx32 " active h %#" B_PRIx32
							"\n", i, rd(mdss, 0x35000 + 0x1000 * i),
							rd(mdss, 0x35000 + 0x1000 * i + 0x8),
							rd(mdss, 0x35000 + 0x1000 * i + 0xa8));
					}
					// what feeds the timing engines (DPU 8.0 offsets from the
					// MDP block at +0x1000, Linux's dpu_8_0_sc8280xp.h):
					// control paths, layer mixers and source pipes in use
					volatile uint8* mdp = mdss + 0x1000;
					for (int i = 0; i < 6; i++) {
						volatile uint8* ctl = mdp + 0x15000 + 0x1000 * i;
						dprintf("qcom_probe: ctl %d layer %#" B_PRIx32 " %#"
							B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32
							" top %#" B_PRIx32 " intf %#" B_PRIx32 " fetch %#"
							B_PRIx32 " pipes %#" B_PRIx32 " layers %#" B_PRIx32
							"\n", i, rd(ctl, 0x0), rd(ctl, 0x4), rd(ctl, 0x8),
							rd(ctl, 0xc), rd(ctl, 0x10), rd(ctl, 0x14),
							rd(ctl, 0xf4), rd(ctl, 0xfc), rd(ctl, 0x12c),
							rd(ctl, 0x130));
					}
					for (int i = 0; i < 6; i++) {
						volatile uint8* lm = mdp + 0x44000 + 0x1000 * i;
						dprintf("qcom_probe: lm %d op %#" B_PRIx32 " out %#"
							B_PRIx32 " border %#" B_PRIx32 "\n", i, rd(lm, 0x0),
							rd(lm, 0x4), rd(lm, 0x8));
					}
					static const uint32 kPipes[] = {0x4000, 0x6000, 0x8000,
						0xa000, 0x24000, 0x26000, 0x28000, 0x2a000};
					for (int i = 0; i < 8; i++) {
						volatile uint8* pipe = mdp + kPipes[i];
						dprintf("qcom_probe: sspp %#" B_PRIx32 " src %#" B_PRIx32
							" xy %#" B_PRIx32 " out %#" B_PRIx32 " oxy %#" B_PRIx32
							" addr %#" B_PRIx32 " stride %#" B_PRIx32 " format %#"
							B_PRIx32 " unpack %#" B_PRIx32 " op %#" B_PRIx32 "\n",
							kPipes[i], rd(pipe, 0x0), rd(pipe, 0x8),
							rd(pipe, 0xc), rd(pipe, 0x10), rd(pipe, 0x14),
							rd(pipe, 0x24), rd(pipe, 0x30), rd(pipe, 0x34),
							rd(pipe, 0x38));
					}
					// the DisplayPort INTFs (0, 4, 5, 6): their timing
					static const uint32 kInterfaces[] = {0x34000, 0x38000,
						0x39000, 0x3a000};
					for (int i = 0; i < 4; i++) {
						volatile uint8* intf = mdp + kInterfaces[i];
						if (rd(intf, 0x0) == 0)
							continue;
						dprintf("qcom_probe: intf %#" B_PRIx32 " config %#"
							B_PRIx32 " hsync %#" B_PRIx32 " vperiod %#" B_PRIx32
							" vpulse %#" B_PRIx32 " vstart %#" B_PRIx32 " vend %#"
							B_PRIx32 " hctl %#" B_PRIx32 " polarity %#" B_PRIx32
							" panel %#" B_PRIx32 " config2 %#" B_PRIx32
							" line %#" B_PRIx32 "\n", kInterfaces[i],
							rd(intf, 0x4), rd(intf, 0x8), rd(intf, 0xc),
							rd(intf, 0x14), rd(intf, 0x1c), rd(intf, 0x24),
							rd(intf, 0x3c), rd(intf, 0x50), rd(intf, 0x90),
							rd(intf, 0x60), rd(intf, 0xb0));
					}
					dprintf("qcom_probe: mdp top split %#" B_PRIx32 " clk %#"
						B_PRIx32 " intr en %#" B_PRIx32 " %#" B_PRIx32 "\n",
						rd(mdp, 0x2f4), rd(mdp, 0x2ac), rd(mdp, 0x10),
						rd(mdp, 0x20));
					delete_area(area);
				}
			}
		}
	}

	if (sdAhb) {
		area_id area;
		volatile uint8* sd = map({"probe sdhc2", 0x8804000, 0x1000}, area);
		if (sd != NULL) {
			dprintf("qcom_probe: sdhc2 caps %#" B_PRIx32 " %#" B_PRIx32
				" present %#" B_PRIx32 " host control %#" B_PRIx32 " version %#"
				B_PRIx32 "\n", rd(sd, 0x40), rd(sd, 0x44), rd(sd, 0x24),
				rd(sd, 0x28), rd(sd, 0xfc));
			delete_area(area);
		}
	}
}


// devfs calls init_hardware() when it loads the driver: the probe runs then,
// and the driver reports no device.
status_t
init_hardware()
{
	probe();
	return B_ERROR;
}


status_t
init_driver()
{
	return B_ERROR;
}


void
uninit_driver()
{
}


const char**
publish_devices()
{
	return NULL;
}


device_hooks*
find_device(const char* name)
{
	return NULL;
}
