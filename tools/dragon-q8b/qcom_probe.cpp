/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab probe for the Radxa Dragon Q8B (SC8280XP): logs what the firmware
	leaves running of the GPU, display, video and SD blocks. A block's own
	registers are only read once the clocks and power domain in front of it
	read as on: touching an unclocked block hangs the SoC. Linked into
	drivers/dev/misc by hand; publishes nothing. */


#include <stdio.h>
#include <string.h>
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


static void sd_identify();


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
	bool videoAhb = clock_on(gcc, 0x28004);
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
			// DisplayPort controller 2 (the HDMI bridge): its clocks and the
			// pixel and link RCGs (CMD, CFG, M, N, D)
			bool dp2On = mdssOn && clock_on(dispcc, 0x20a4)
				&& clock_on(dispcc, 0x2090);
			dprintf("qcom_probe: dptx2 aux %#" B_PRIx32 " link %#" B_PRIx32
				" link intf %#" B_PRIx32 " pixel0 %#" B_PRIx32 " pixel rcg %#"
				B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#"
				B_PRIx32 " link rcg %#" B_PRIx32 " %#" B_PRIx32 "\n",
				rd(dispcc, 0x20a4), rd(dispcc, 0x2090), rd(dispcc, 0x2094),
				rd(dispcc, 0x207c), rd(dispcc, 0x2208), rd(dispcc, 0x220c),
				rd(dispcc, 0x2210), rd(dispcc, 0x2214), rd(dispcc, 0x2218),
				rd(dispcc, 0x2284), rd(dispcc, 0x2288));
			delete_area(area);
			if (dp2On) {
				volatile uint8* dp = map({"probe dp2", 0xae9a000, 0x2000}, area);
				if (dp != NULL) {
					volatile uint8* aux = dp + 0x200;
					volatile uint8* link = dp + 0x400;
					dprintf("qcom_probe: dp2 version %#" B_PRIx32 " intr %#"
						B_PRIx32 " %#" B_PRIx32 " hpd ctrl %#" B_PRIx32
						" hpd status %#" B_PRIx32 " aux ctrl %#" B_PRIx32
						" aux status %#" B_PRIx32 " limits %#" B_PRIx32 "\n",
						rd(dp, 0x0), rd(dp, 0x20), rd(dp, 0x24), rd(aux, 0x0),
						rd(aux, 0x4), rd(aux, 0x30), rd(aux, 0x44),
						rd(aux, 0x40));
					dprintf("qcom_probe: dp2 mainlink %#" B_PRIx32 " state %#"
						B_PRIx32 " config %#" B_PRIx32 " mvid %#" B_PRIx32
						" nvid %#" B_PRIx32 " total %#" B_PRIx32 " start %#"
						B_PRIx32 " sync %#" B_PRIx32 " active %#" B_PRIx32
						" misc %#" B_PRIx32 " ready %#" B_PRIx32 " tu %#" B_PRIx32
						" lanes %#" B_PRIx32 "\n", rd(link, 0x0), rd(link, 0x4),
						rd(link, 0x8), rd(link, 0x10), rd(link, 0x18),
						rd(link, 0x1c), rd(link, 0x20), rd(link, 0x24),
						rd(link, 0x28), rd(link, 0x2c), rd(link, 0x40),
						rd(link, 0x4c), rd(link, 0x38));
					delete_area(area);
				}
			}
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

	// the video codec's clock controller (videocc-sm8350.c, the SC8280XP
	// values): GDSCs mvs0c/mvs0 (bit 31 on), branches mvs0c/mvs0, PLL modes
	if (videoAhb) {
		area_id area;
		volatile uint8* videocc = map({"probe videocc", 0xabf0000, 0x10000},
			area);
		if (videocc != NULL) {
			dprintf("qcom_probe: videocc mvs0c gdsc %#" B_PRIx32 " mvs0 gdsc %#"
				B_PRIx32 " mvs0c clk %#" B_PRIx32 " mvs0 clk %#" B_PRIx32
				" pll0 %#" B_PRIx32 " pll1 %#" B_PRIx32 " ahb rcg %#" B_PRIx32
				" mvs0 rcg %#" B_PRIx32 " %#" B_PRIx32 "\n", rd(videocc, 0xbf8),
				rd(videocc, 0xd18), rd(videocc, 0xc34), rd(videocc, 0xd34),
				rd(videocc, 0x0), rd(videocc, 0x400), rd(videocc, 0xbd4),
				rd(videocc, 0xb94), rd(videocc, 0xb98));
			delete_area(area);
		}
	} else
		dprintf("qcom_probe: video_ahb off, video clock controller left alone\n");

	// the apps SMMU: who has stream mappings (SMR valid), and how
	{
		area_id area;
		volatile uint8* smmu = map({"probe apps smmu", 0x15000000, 0x1000},
			area);
		if (smmu != NULL) {
			uint32 idr0 = rd(smmu, 0x20);
			uint32 groups = idr0 & 0xff;
			dprintf("qcom_probe: apps smmu sCR0 %#" B_PRIx32 " IDR0 %#" B_PRIx32
				", %" B_PRIu32 " stream groups\n", rd(smmu, 0), idr0, groups);
			for (uint32 i = 0; i < groups && i < 128; i++) {
				uint32 smr = rd(smmu, 0x800 + 4 * i);
				if ((smr & (1u << 31)) == 0)
					continue;
				dprintf("qcom_probe: apps smmu SMR %" B_PRIu32 " id %#" B_PRIx32
					" mask %#" B_PRIx32 " S2CR %#" B_PRIx32 "\n", i,
					smr & 0xffff, (smr >> 16) & 0x7fff, rd(smmu, 0xc00 + 4 * i));
			}
			delete_area(area);
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

	// the CPUs' clocks: EPSS frequency domains (qcom-cpufreq-hw's epss
	// layout: LUT at 0x100, 4 bytes a row, source in bits 31:30, L in 7:0;
	// the current L in domain state 0x20; the requested index at 0x320)
	for (uint32 domain = 0; domain < 2; domain++) {
		area_id area;
		volatile uint8* epss = map({"probe epss",
			0x18591000 + domain * 0x1000, 0x1000}, area);
		if (epss == NULL)
			continue;
		char table[256] = "";
		size_t used = 0;
		uint32 entries = 0, previous = 0;
		for (uint32 i = 0; i < 40; i++) {
			uint32 row = rd(epss, 0x100 + 4 * i);
			uint32 mhz = ((row >> 30) != 0 ? 19200 * (row & 0xff) : 300000)
				/ 1000;
			if (mhz == previous)
				break;
			previous = mhz;
			entries++;
			if (used < sizeof(table) - 8)
				used += snprintf(table + used, sizeof(table) - used, " %u", mhz);
		}
		uint32 state = rd(epss, 0x20) & 0xff;
		dprintf("qcom_probe: cpu domain %" B_PRIu32 ": enable %#" B_PRIx32
			", running %" B_PRIu32 " MHz, requested index %" B_PRIu32 " of %"
			B_PRIu32 "; MHz:%s\n", domain, rd(epss, 0), 19200 * state / 1000,
			rd(epss, 0x320), entries, table);
		delete_area(area);
	}

	// the apps SMMU: how the SD controller's stream (0x4e0) is routed
	{
		area_id area;
		volatile uint8* smmu = map({"probe apps smmu", 0x15000000, 0x1000},
			area);
		if (smmu != NULL) {
			uint32 groups = rd(smmu, 0x20) & 0xff;
			dprintf("qcom_probe: apps smmu sCR0 %#" B_PRIx32 ", %" B_PRIu32
				" stream groups\n", rd(smmu, 0), groups);
			for (uint32 i = 0; i < groups; i++) {
				uint32 smr = rd(smmu, 0x800 + 4 * i);
				if ((smr & (1u << 31)) == 0)
					continue;
				uint32 id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;
				if (((id ^ 0x4e0) & ~mask & 0x7fff) != 0)
					continue;
				dprintf("qcom_probe: apps smmu SMR%" B_PRIu32 " id %#" B_PRIx32
					" mask %#" B_PRIx32 " S2CR %#" B_PRIx32 "\n", i, id, mask,
					rd(smmu, 0xc00 + 4 * i));
			}
			delete_area(area);
		}
	}

	// the SD slot's card detect (TLMM GPIO 131: the schematic's switch is
	// closed to ground without a card and open with one, whatever the
	// device tree's GPIO_ACTIVE_LOW says) and its
	// pin configuration, and the SD pins' (sdc2 clk/cmd/data in their own
	// register)
	{
		area_id area;
		volatile uint8* tlmm = map_rw({"probe tlmm", 0xf100000 + 131 * 0x1000,
			0x1000}, area);
		if (tlmm != NULL) {
			uint32 cfg = rd(tlmm, 0), io = rd(tlmm, 4);
			dprintf("qcom_probe: sd card detect gpio131 cfg %#" B_PRIx32
				" in/out %#" B_PRIx32 "\n", cfg, io);
			// as sdc2-default-state wants it: gpio, 2 mA, no bias (the
			// board pulls it up); the firmware leaves a pull-down
			*(volatile uint32*)tlmm = 0;
			spin(1000);
			io = rd(tlmm, 4);
			dprintf("qcom_probe: sd card detect, no bias: in/out %#" B_PRIx32
				": %s\n", io, (io & 1) == 0 ? "no card" : "card in");
			// and with the pin's own pull-up, in case the board's is to a
			// supply that is off
			*(volatile uint32*)tlmm = 3;
			spin(1000);
			io = rd(tlmm, 4);
			dprintf("qcom_probe: sd card detect, pull-up: in/out %#" B_PRIx32
				": %s\n", io, (io & 1) == 0 ? "no card" : "card in");
			*(volatile uint32*)tlmm = 0;
			sd_identify();
			delete_area(area);
		}
	}
}


// #pragma mark - SD card identification (no data transfers, so no DMA)


static inline void
wr8(volatile uint8* base, uint32 offset, uint8 value)
{
	base[offset] = value;
}


static inline void
wr16(volatile uint8* base, uint32 offset, uint16 value)
{
	*(volatile uint16*)(base + offset) = value;
}


static inline void
wr32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


static inline uint16
rd16(volatile uint8* base, uint32 offset)
{
	return *(volatile uint16*)(base + offset);
}


// #pragma mark - RPMh (the SD card's supplies)


struct probe_cmd_db_rsc {
	uint16	slave_id;
	uint16	header_offset;
	uint16	data_offset;
	uint16	count;
	uint16	version;
	uint16	reserved[3];
};

struct probe_cmd_db_entry {
	char	id[8];
	uint32	priority[2];
	uint32	address;
	uint16	length;
	uint16	offset;
};

struct probe_cmd_db_header {
	uint32	version;
	uint8	magic[4];
	probe_cmd_db_rsc resources[8];
	uint32	checksum;
	uint32	reserved;
	uint8	data[];
};


//!	An RPMh resource's address from the command DB, 0 when it has none.
static uint32
cmd_db_address(const char* id)
{
	area_id area;
	volatile uint8* base = map({"probe cmd-db", 0x80860000, 0x20000}, area);
	if (base == NULL)
		return 0;
	const probe_cmd_db_header* header = (const probe_cmd_db_header*)base;
	uint32 address = 0;
	static const uint8 kMagic[] = {0xdb, 0x30, 0x03, 0x0c};
	char query[8] = {};
	memcpy(query, id, strnlen(id, 8));
	if (memcmp((const void*)header->magic, kMagic, 4) == 0) {
		for (int i = 0; i < 8 && header->resources[i].slave_id != 0
				&& address == 0; i++) {
			const probe_cmd_db_rsc& rsc = header->resources[i];
			const probe_cmd_db_entry* entry = (const probe_cmd_db_entry*)
				(header->data + rsc.header_offset);
			for (int j = 0; j < rsc.count; j++, entry++) {
				if (memcmp(entry->id, query, 8) == 0) {
					address = entry->address;
					break;
				}
			}
		}
	}
	delete_area(area);
	return address;
}


/*!	One active-mode request on the apps RSC (DRV 2, its first active TCS, as
	rpmh-rsc's tcs_write()): \a count writes of \a data to \a addresses,
	each waited for. */
static bool
rpmh_write(const uint32* addresses, const uint32* data, int count)
{
	area_id area;
	volatile uint8* drv = map_rw({"probe apps rsc", 0x18220000, 0x10000},
		area);
	if (drv == NULL)
		return false;
	uint32 version = rd(drv, 0);
	uint32 major = (version >> 16) & 0xff;
	// rpmh_rsc_reg_offset_ver_3_0 / _2_7
	bool v3 = major >= 3;
	const uint32 tcsSize = 672, cmdSize = v3 ? 24 : 20;
	const uint32 irqStatus = 0x04, irqClear = 0x08;
	const uint32 waitForCompletion = v3 ? 0x20 : 0x10;
	const uint32 control = v3 ? 0x24 : 0x14, enableRegister = v3 ? 0x2c : 0x1c;
	const uint32 msgId = v3 ? 0x34 : 0x30, addr = v3 ? 0x38 : 0x34;
	const uint32 dataRegister = v3 ? 0x3c : 0x38, cmdStatus = v3 ? 0x40 : 0x3c;
	volatile uint8* tcs = drv + 0xd00;
	const int tcsId = 0;
	volatile uint8* t = tcs + tcsSize * tcsId;

	uint32 enable = 0;
	for (int i = 0; i < count; i++) {
		volatile uint8* c = t + cmdSize * i;
		wr32(c, msgId, 8 | 1u << 16 | 1u << 8);	// write, response wanted
		wr32(c, addr, addresses[i]);
		wr32(c, dataRegister, data[i]);
		enable |= 1u << i;
	}
	wr32(t, waitForCompletion, enable);
	wr32(t, enableRegister, enable);
	// __tcs_set_trigger(): AMC mode on, then the trigger
	uint32 value = rd(t, control);
	value &= ~(1u << 24);
	wr32(t, control, value);
	value &= ~(1u << 16);
	wr32(t, control, value);
	value = 1u << 16;
	wr32(t, control, value);
	value |= 1u << 24;
	wr32(t, control, value);

	bigtime_t deadline = system_time() + 100000;
	bool done = false;
	while (system_time() < deadline) {
		if ((rd(tcs, irqStatus) & (1u << tcsId)) != 0) {
			done = true;
			break;
		}
		spin(10);
	}
	uint32 status[4] = {};
	for (int i = 0; i < count && i < 4; i++)
		status[i] = rd(t + cmdSize * i, cmdStatus);
	wr32(tcs, irqClear, 1u << tcsId);
	wr32(t, control, 0);
	wr32(t, enableRegister, 0);
	dprintf("qcom_probe: rpmh v%" B_PRIu32 ".%" B_PRIu32 " write of %d: %s, "
		"status %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n", major,
		(version >> 8) & 0xff, count, done ? "done" : "timed out", status[0],
		status[1], status[2]);
	delete_area(area);
	return done;
}


//!	An LDO of the VRM on, at \a millivolts (rpmh-regulator: voltage at +0,
//	enable at +4, mode at +8; PMIC5 LDO HPM is mode 7).
static void
ldo_on(const char* name, uint32 millivolts)
{
	uint32 base = cmd_db_address(name);
	dprintf("qcom_probe: %s at %#" B_PRIx32 "\n", name, base);
	if (base == 0)
		return;
	uint32 addresses[3] = {base + 0, base + 8, base + 4};
	uint32 data[3] = {millivolts, 7, 1};
	rpmh_write(addresses, data, 3);
}


/*!	sdhci-msm v5's power handshake: the controller waits for the host to
	acknowledge bus and I/O voltage changes (CORE_PWRCTL_* at 0x240..0x24c). */
static void
sd_power_ack(volatile uint8* sd, const char* what)
{
	bigtime_t deadline = system_time() + 50000;
	while (system_time() < deadline) {
		uint32 status = rd(sd, 0x240);
		if (status == 0) {
			spin(100);
			continue;
		}
		wr32(sd, 0x248, status);
		// as sdhci_msm_handle_pwr_irq(): a bus on or off is an I/O level
		// change as well, both acknowledged
		uint32 ack = 0;
		if ((status & 0x3) != 0)
			ack |= 1u << 0 | 1u << 2;	// bus, I/O done
		if ((status & 0xc) != 0)
			ack |= 1u << 2;				// I/O high/low done
		wr32(sd, 0x24c, ack);
		dprintf("qcom_probe: sd %s: power event %#" B_PRIx32 ", ack %#" B_PRIx32
			"\n", what, status, ack);
		return;
	}
}


/*!	One command; \a flags as in the command register's low byte (response
	type, CRC and index checks). Returns false on an error or timeout. */
static bool
sd_command(volatile uint8* sd, uint32 index, uint32 argument, uint16 flags,
	uint32* response)
{
	wr16(sd, 0x30, 0xffff);
	wr16(sd, 0x32, 0xffff);
	bigtime_t deadline = system_time() + 100000;
	while ((rd(sd, 0x24) & 1) != 0) {
		if (system_time() > deadline)
			return false;
		spin(10);
	}
	wr32(sd, 0x08, argument);
	wr16(sd, 0x0e, (uint16)(index << 8) | flags);
	deadline = system_time() + 250000;
	while (true) {
		uint16 status = rd16(sd, 0x30);
		if ((status & 0x8000) != 0) {
			dprintf("qcom_probe: sd CMD%" B_PRIu32 ": error %#x\n", index,
				rd16(sd, 0x32));
			wr8(sd, 0x2f, 0x06);	// reset the command and data lines
			spin(1000);
			return false;
		}
		if ((status & 1) != 0)
			break;
		if (system_time() > deadline) {
			dprintf("qcom_probe: sd CMD%" B_PRIu32 ": no response\n", index);
			return false;
		}
		spin(10);
	}
	for (int i = 0; i < 4; i++)
		response[i] = rd(sd, 0x10 + 4 * i);
	return true;
}


static void
sd_identify()
{
	area_id area;
	volatile uint8* sd = map_rw({"probe sdhc2 rw", 0x8804000, 0x1000}, area);
	if (sd == NULL)
		return;

	// the controller's core clock (gcc_sdcc2_apps_clk, its RCG at 0x1400c)
	{
		area_id gccArea;
		volatile uint8* gcc = map_rw({"probe gcc rw", 0x100000, 0x1f4200},
			gccArea);
		if (gcc != NULL) {
			dprintf("qcom_probe: sd apps clock cbcr %#" B_PRIx32 ", rcg cmd %#"
				B_PRIx32 " cfg %#" B_PRIx32 " m %#" B_PRIx32 " n %#" B_PRIx32
				" d %#" B_PRIx32 "\n", rd(gcc, 0x14004), rd(gcc, 0x1400c),
				rd(gcc, 0x14010), rd(gcc, 0x14014), rd(gcc, 0x14018),
				rd(gcc, 0x1401c));
			if ((rd(gcc, 0x14004) & (1u << 31)) != 0) {
				wr32(gcc, 0x14004, rd(gcc, 0x14004) | 1);
				spin(100);
				dprintf("qcom_probe: sd apps clock on: cbcr %#" B_PRIx32 "\n",
					rd(gcc, 0x14004));
			}
			delete_area(gccArea);
		}
	}

	// the SD pins as sdc2-default-state has them (pinctrl-sc8280xp.c: one
	// register at TLMM + 0xe8000; clk pull 15:14 drive 8:6, cmd pull 12:11
	// drive 5:3, data pull 10:9 drive 2:0; pull 3 = up, drive 7 = 16 mA)
	{
		area_id tlmmArea;
		volatile uint8* tlmm = map_rw({"probe tlmm sdc2", 0xf100000 + 0xe8000,
			0x1000}, tlmmArea);
		if (tlmm != NULL) {
			uint32 before = rd(tlmm, 0);
			uint32 value = before & ~(3u << 14 | 7u << 6 | 3u << 11 | 7u << 3
				| 3u << 9 | 7u << 0);
			value |= 0u << 14 | 7u << 6 | 3u << 11 | 7u << 3 | 3u << 9
				| 7u << 0;
			wr32(tlmm, 0, value);
			dprintf("qcom_probe: sdc2 pins %#" B_PRIx32 " -> %#" B_PRIx32 "\n",
				before, rd(tlmm, 0));
			delete_area(tlmmArea);
		}
	}

	// sdhci_msm_probe(): the block reset through GCC (GCC_SDCC2_BCR), the
	// vendor register's power on value with the pad voltage switch on and
	// at 3 V, 3.0 V and 8 bits in the capabilities, the power interrupts
	// unmasked
	{
		area_id gccArea;
		volatile uint8* gcc = map_rw({"probe gcc bcr", 0x100000, 0x1f4200},
			gccArea);
		if (gcc != NULL) {
			wr32(gcc, 0x14000, rd(gcc, 0x14000) | 1);
			spin(200);
			wr32(gcc, 0x14000, rd(gcc, 0x14000) & ~1u);
			spin(200);
			delete_area(gccArea);
		}
	}
	dprintf("qcom_probe: sd mci version %#" B_PRIx32 ", vendor spec %#" B_PRIx32
		"\n", rd(sd, 0x50), rd(sd, 0x20c));
	wr32(sd, 0x20c, 0xa9c);
	wr32(sd, 0x21c, rd(sd, 0x40) | 1u << 25 | 1u << 18);
	wr32(sd, 0x20c, (rd(sd, 0x20c) | 1u << 15) & ~(1u << 16));
	wr32(sd, 0x244, 0xf);

	// the card's supply (vmmc, L9C) and its pins' (vqmmc, L6C), 2.96 V
	ldo_on("ldoc9", 2960);
	ldo_on("ldoc6", 2960);
	snooze(20000);

	wr8(sd, 0x2f, 0x01);	// reset all
	bigtime_t deadline = system_time() + 100000;
	while ((sd[0x2f] & 1) != 0 && system_time() < deadline)
		spin(10);
	sd_power_ack(sd, "reset");
	wr32(sd, 0x34, 0xffffffff);	// status bits latch
	wr32(sd, 0x38, 0);			// no interrupt signals

	// 3.0 V: the capabilities (sdhci-msm's CORE_VENDOR_SPEC_CAPABILITIES0)
	// offer 3.0 and 1.8 V, not 3.3; the pads at 3 V (CORE_VENDOR_SPEC at
	// 0x20c: CORE_IO_PAD_PWR_SWITCH, bit 16, clear)
	wr32(sd, 0x20c, rd(sd, 0x20c) & ~(1u << 16));
	wr8(sd, 0x29, 0x0c);
	wr8(sd, 0x29, 0x0d);		// bus power on
	sd_power_ack(sd, "bus power");
	spin(10000);

	// <= 400 kHz from the RCG's rate (gcc_parent_map_15: XO, GPLL0, GPLL9,
	// -, -, GPLL4, GPLL0 even; half divider; M/N in dual edge mode), the
	// 10-bit divider being N for base / 2N
	uint32 divider = 250;
	{
		area_id gccArea;
		volatile uint8* gcc = map({"probe gcc rd", 0x100000, 0x1f4200},
			gccArea);
		if (gcc != NULL) {
			uint32 cfg = rd(gcc, 0x14010);
			static const uint64 kSources[8] = {19200000, 600000000,
				808000000, 0, 0, 806400000, 300000000, 0};
			uint64 base = kSources[(cfg >> 8) & 7];
			uint32 halfDiv = cfg & 0x1f;
			if (halfDiv != 0)
				base = base * 2 / (halfDiv + 1);
			if (((cfg >> 12) & 3) == 2) {
				uint32 m = rd(gcc, 0x14014) & 0xff;
				uint32 n = ((~rd(gcc, 0x14018)) & 0xff) + m;
				if (n != 0)
					base = base * m / n;
			}
			if (base != 0)
				divider = (uint32)((base + 2 * 400000 - 1) / (2 * 400000));
			if (divider == 0)
				divider = 1;
			dprintf("qcom_probe: sd base clock %" B_PRIu64 " Hz (cfg %#" B_PRIx32
				"), divider %" B_PRIu32 " (%" B_PRIu64 " Hz)\n", base, cfg,
				divider, base / (2 * divider));
			delete_area(gccArea);
		}
	}
	wr16(sd, 0x2c, (uint16)((divider & 0xff) << 8 | ((divider >> 8) & 3) << 6
		| 1));
	deadline = system_time() + 100000;
	while ((rd16(sd, 0x2c) & 2) == 0 && system_time() < deadline)
		spin(10);
	wr16(sd, 0x2c, rd16(sd, 0x2c) | 4);
	wr8(sd, 0x2e, 0x0e);
	spin(5000);
	dprintf("qcom_probe: sd powered: power %#x clock %#x present %#" B_PRIx32
		" caps %#" B_PRIx32 "\n", sd[0x29], rd16(sd, 0x2c), rd(sd, 0x24),
		rd(sd, 0x40));

	uint32 response[4];
	sd_command(sd, 0, 0, 0x00, response);
	spin(2000);
	if (!sd_command(sd, 8, 0x1aa, 0x1a, response)) {
		dprintf("qcom_probe: sd: no answer to CMD8 (no card, no power, or an "
			"old card)\n");
		delete_area(area);
		return;
	}
	dprintf("qcom_probe: sd CMD8: %#" B_PRIx32 "\n", response[0]);

	uint32 ocr = 0;
	deadline = system_time() + 1500000;
	while (system_time() < deadline) {
		if (!sd_command(sd, 55, 0, 0x1a, response)
			|| !sd_command(sd, 41, 0x40ff8000, 0x02, response))
			break;
		ocr = response[0];
		if ((ocr & (1u << 31)) != 0)
			break;
		snooze(10000);
	}
	dprintf("qcom_probe: sd ACMD41: OCR %#" B_PRIx32 "%s\n", ocr,
		(ocr & (1u << 31)) != 0 ? ((ocr & (1u << 30)) != 0
			? " (ready, SDHC/SDXC)" : " (ready, SDSC)") : " (not ready)");
	if ((ocr & (1u << 31)) == 0) {
		delete_area(area);
		return;
	}

	if (sd_command(sd, 2, 0, 0x09, response)) {
		// R2 without its CRC byte: CID bits 127..8 in response[3..0]
		char name[6];
		uint64 high = (uint64)response[3] << 32 | response[2];
		for (int i = 0; i < 5; i++)
			name[i] = (char)(high >> (8 * (4 - i)) & 0xff);
		name[5] = '\0';
		dprintf("qcom_probe: sd CID %08" B_PRIx32 " %08" B_PRIx32 " %08"
			B_PRIx32 " %08" B_PRIx32 ": manufacturer %#" B_PRIx32 ", \"%s\"\n",
			response[3], response[2], response[1], response[0],
			response[3] >> 16, name);
	}
	if (sd_command(sd, 3, 0, 0x1a, response)) {
		dprintf("qcom_probe: sd CMD3: RCA %#" B_PRIx32 "\n",
			response[0] >> 16);
	}
	delete_area(area);
}


// #pragma mark - ADSP (what the firmware leaves of it)


/*!	An SMEM item's data in the partition of \a host0 and \a host1 (0xfffe
	both: the global one), from Linux' smem.c layout; NULL when it is not
	there. Only the uncached items: SMP2P and GLINK's are. */
static volatile uint8*
smem_item(volatile uint8* smem, size_t smemSize, uint16 host0, uint16 host1,
	uint16 item, uint32* _size)
{
	volatile uint8* table = smem + smemSize - 4096;
	if (rd(table, 0) != 0x434f5424)	// "$TOC"
		return NULL;
	uint32 count = rd(table, 8);
	for (uint32 i = 0; i < count && i < 64; i++) {
		volatile uint8* entry = table + 32 + 48 * i;
		uint32 offset = rd(entry, 0), size = rd(entry, 4);
		uint16 a = rd16(entry, 12), b = rd16(entry, 14);
		if (!((a == host0 && b == host1) || (a == host1 && b == host0))
			|| offset == 0 || offset + size > smemSize)
			continue;
		volatile uint8* partition = smem + offset;
		if (rd(partition, 0) != 0x54525024)	// "$PRT"
			return NULL;
		uint32 freeUncached = rd(partition, 12);
		uint32 position = 32;
		while (position + 16 <= freeUncached && freeUncached <= size) {
			volatile uint8* header = partition + position;
			if (rd16(header, 0) != 0xa5a5)
				break;
			uint32 itemSize = rd(header, 4);
			uint16 paddingData = rd16(header, 8);
			uint16 paddingHeader = rd16(header, 10);
			if (rd16(header, 2) == item) {
				if (_size != NULL)
					*_size = itemSize - paddingData;
				return header + 16 + paddingHeader;
			}
			position += 16 + paddingHeader + itemSize;
		}
		return NULL;
	}
	return NULL;
}


static void
smem_partitions(volatile uint8* smem, size_t smemSize)
{
	dprintf("qcom_probe: smem version %#" B_PRIx32 "\n", rd(smem, 64 + 7 * 4));
	volatile uint8* table = smem + smemSize - 4096;
	if (rd(table, 0) != 0x434f5424) {
		dprintf("qcom_probe: smem: no partition table\n");
		return;
	}
	uint32 count = rd(table, 8);
	for (uint32 i = 0; i < count && i < 64; i++) {
		volatile uint8* entry = table + 32 + 48 * i;
		uint32 offset = rd(entry, 0), size = rd(entry, 4);
		uint16 a = rd16(entry, 12), b = rd16(entry, 14);
		if (offset == 0 || offset + size > smemSize)
			continue;
		volatile uint8* partition = smem + offset;
		uint32 items = 0;
		char list[160] = {};
		size_t length = 0;
		if (rd(partition, 0) == 0x54525024) {
			uint32 freeUncached = rd(partition, 12);
			uint32 position = 32;
			while (position + 16 <= freeUncached && freeUncached <= size) {
				volatile uint8* header = partition + position;
				if (rd16(header, 0) != 0xa5a5)
					break;
				if (length + 8 < sizeof(list)) {
					length += snprintf(list + length, sizeof(list) - length,
						" %u", rd16(header, 2));
				}
				items++;
				position += 16 + rd16(header, 10) + rd(header, 4);
			}
		}
		dprintf("qcom_probe: smem partition %#x-%#x at %#" B_PRIx32 " (%#"
			B_PRIx32 "): %" B_PRIu32 " uncached items:%s\n", a, b, offset,
			size, items, a == 0xfffe ? " (global)" : list);
	}
}


static void
smp2p_log(volatile uint8* smem, size_t smemSize, uint16 remote, uint16 item,
	const char* what)
{
	uint32 size = 0;
	volatile uint8* smp2p = smem_item(smem, smemSize, 0, remote, item, &size);
	if (smp2p == NULL) {
		dprintf("qcom_probe: smp2p %s (item %u): none\n", what, item);
		return;
	}
	uint32 magic = rd(smp2p, 0), features = rd(smp2p, 4);
	uint32 pids = rd(smp2p, 8), entries = rd(smp2p, 12);
	dprintf("qcom_probe: smp2p %s (item %u, %" B_PRIu32 " bytes): magic %#"
		B_PRIx32 " version %" B_PRIu32 " features %#" B_PRIx32 ", pid %"
		B_PRIu32 " -> %" B_PRIu32 ", %" B_PRIu32 " of %" B_PRIu32
		" entries, flags %#" B_PRIx32 "\n", what, item, size, magic,
		features & 0xff, features >> 8, pids & 0xffff, pids >> 16,
		entries >> 16, entries & 0xffff, rd(smp2p, 16));
	for (uint32 i = 0; i < (entries >> 16) && i < 16; i++) {
		volatile uint8* entry = smp2p + 20 + 20 * i;
		char name[17] = {};
		for (int c = 0; c < 16; c++)
			name[c] = entry[c];
		dprintf("qcom_probe: smp2p %s entry \"%s\" = %#" B_PRIx32 "\n", what,
			name, rd(entry, 16));
	}
}


static void
adsp_state()
{
	area_id area;
	const size_t smemSize = 0x200000;
	volatile uint8* smem = map({"probe smem", 0x80900000, smemSize}, area);
	if (smem == NULL)
		return;
	smem_partitions(smem, smemSize);
	smp2p_log(smem, smemSize, 2, 443, "apps->adsp");
	smp2p_log(smem, smemSize, 2, 429, "adsp->apps");
	static const uint16 kGlinkItems[] = {478, 479, 480};
	for (uint16 item : kGlinkItems) {
		uint32 size = 0;
		volatile uint8* data = smem_item(smem, smemSize, 0, 2, item, &size);
		dprintf("qcom_probe: glink adsp item %u: %s %#" B_PRIx32 " bytes\n",
			item, data != NULL ? "there," : "none", size);
		if (data != NULL && item == 478) {
			dprintf("qcom_probe: glink adsp descriptor: tx tail %#" B_PRIx32
				" head %#" B_PRIx32 ", rx tail %#" B_PRIx32 " head %#" B_PRIx32
				"\n", rd(data, 0), rd(data, 4), rd(data, 8), rd(data, 12));
		}
	}
	delete_area(area);

	// the AOSS's QMP message RAM (qcom_aoss.c's layout)
	volatile uint8* qmp = map({"probe aoss qmp", 0xc300000, 0x400}, area);
	if (qmp == NULL)
		return;
	dprintf("qcom_probe: aoss qmp magic %#" B_PRIx32 " version %#" B_PRIx32
		" features %#" B_PRIx32 "; ucore link %#" B_PRIx32 "/%#" B_PRIx32
		" channel %#" B_PRIx32 "/%#" B_PRIx32 " mailbox %#" B_PRIx32 " at %#"
		B_PRIx32 "; mcore link %#" B_PRIx32 "/%#" B_PRIx32 " channel %#"
		B_PRIx32 "/%#" B_PRIx32 " mailbox %#" B_PRIx32 " at %#" B_PRIx32 "\n",
		rd(qmp, 0), rd(qmp, 4), rd(qmp, 8), rd(qmp, 0xc), rd(qmp, 0x10),
		rd(qmp, 0x14), rd(qmp, 0x18), rd(qmp, 0x1c), rd(qmp, 0x20),
		rd(qmp, 0x24), rd(qmp, 0x28), rd(qmp, 0x2c), rd(qmp, 0x30),
		rd(qmp, 0x34), rd(qmp, 0x38));
	delete_area(area);
}


// #pragma mark - USB-C DisplayPort PHY (a PLL lock test)


/*	The QMP USB43DP combo PHY of USB-C port 0 (0x88eb000) in DisplayPort
	mode, as Linux' phy-qcom-qmp-combo.c does it for the SC8280XP
	(sc8280xp_usb43dpphy_cfg, qmp_combo_offsets_v5): its GDSC, clocks,
	supplies and resets, the common block in USB3+DP mode, the AUX block,
	the DP PLL at RBR (1.62 Gbit/s) for four lanes in the normal
	orientation. Without a sink this can only show the PLL locking and the
	PHY saying it is ready. */

struct qmp_setting {
	uint16	offset;
	uint8	value;
};

static const qmp_setting kDpSerdes[] = {
	{0x184, 0x05}, {0x094, 0x3b}, {0x04c, 0x02}, {0x048, 0x0c},
	{0x050, 0x06}, {0x154, 0x30}, {0x058, 0x0f}, {0x084, 0x36},
	{0x088, 0x36}, {0x07c, 0x16}, {0x080, 0x16}, {0x074, 0x06},
	{0x078, 0x06}, {0x17c, 0x02}, {0x0ec, 0x3f}, {0x0f0, 0x00},
	{0x10c, 0x02}, {0x0cc, 0x00}, {0x00c, 0x0a}, {0x168, 0x0a},
	{0x108, 0x00}, {0x044, 0x17}, {0x174, 0x1f},
};

static const qmp_setting kDpSerdesRbr[] = {
	{0x158, 0x05}, {0x0bc, 0x69}, {0x0d0, 0x80}, {0x0d4, 0x07},
	{0x0ac, 0x6f}, {0x0b0, 0x08}, {0x0a4, 0x04},
};

static const qmp_setting kDpTx[] = {
	{0x80, 0x51}, {0x48, 0x1a}, {0xb0, 0x40}, {0x20, 0x00},
	{0xe4, 0xff}, {0x08, 0x0f}, {0x1c, 0x03}, {0xac, 0x0f},
	{0x54, 0x00}, {0x30, 0x11}, {0x34, 0x11}, {0xd8, 0x01},
};

enum {
	kComPhyModeCtrl = 0x00, kComSwReset = 0x04, kComPowerDownCtrl = 0x08,
	kComSwiCtrl = 0x0c, kComTypecCtrl = 0x10, kComResetOverride = 0x1c,

	kSerdesResetsmCntrl = 0x09c, kSerdesCReadyStatus = 0x178,
	kSerdesCmnStatus = 0x140, kSerdesBiasEn = 0x044,

	kDpPhyCfg = 0x010, kDpPhyCfg1 = 0x014, kDpPhyPdCtl = 0x018,
	kDpPhyMode = 0x01c, kDpPhyAuxCfg0 = 0x020, kDpPhyAuxInterruptMask = 0x054,
	kDpPhyVcoDiv = 0x070, kDpPhyTx01LaneCtl = 0x078,
	kDpPhyTx23LaneCtl = 0x09c, kDpPhyStatus = 0x0dc,

	kTxEmpPost1Level = 0x0c, kTxDrvLevel = 0x14,
	kTxTransceiverBias = 0x48, kTxHighzDriver = 0x4c, kTxPolInv = 0x50,
};

static const uint32 kPhyCom = 0x0000, kPhyTxA = 0x0400, kPhyTxB = 0x0a00,
	kPhyDpSerdes = 0x2000, kPhyDpPhy = 0x2200;


static bool
poll_bits(volatile uint8* base, uint32 offset, uint32 bits, const char* what)
{
	for (int i = 0; i < 100; i++) {
		if ((rd(base, offset) & bits) == bits)
			return true;
		spin(100);
	}
	dprintf("qcom_probe: dp phy: no %s (%#" B_PRIx32 " = %#" B_PRIx32 ")\n",
		what, offset, rd(base, offset));
	return false;
}


/*!	DP0's AUX channel over USB-C port 0, with nothing plugged in: the SBU
	mux (PI3USB102, "gpio-sbu-mux": enable GPIO 101 active low, select
	GPIO 164 for the orientation) on, the AUX clock from XO, the
	controller's AUX reset and enabled (msm_dp_aux), then one native read of
	DPCD 0. Without a sink it must end in a timeout or NACK, not hang. */
static void
dp0_aux_test()
{
	static const uint32 kPins[][2] = {{101, 0}, {164, 0}};	// pin, level
	for (const uint32* pin : kPins) {
		area_id area;
		volatile uint8* tlmm = map_rw({"probe tlmm sbu", 0xf100000
			+ pin[0] * 0x1000, 0x1000}, area);
		if (tlmm == NULL)
			return;
		wr32(tlmm, 4, pin[1] << 1);		// the output value
		wr32(tlmm, 0, 1u << 9);			// GPIO, output enabled, 2 mA
		dprintf("qcom_probe: dp0 aux: gpio %" B_PRIu32 " cfg %#" B_PRIx32
			" io %#" B_PRIx32 "\n", pin[0], rd(tlmm, 0), rd(tlmm, 4));
		delete_area(area);
	}

	area_id dispccArea;
	volatile uint8* dispcc = map_rw({"probe dispcc aux", 0xaf00000, 0x20000},
		dispccArea);
	if (dispcc == NULL)
		return;
	// the AUX RCG (0x2238) from XO, then its branch
	wr32(dispcc, 0x223c, 0);
	wr32(dispcc, 0x2238, rd(dispcc, 0x2238) | 1);
	wait_bits(dispcc, 0x2238, 1, 0);
	wr32(dispcc, 0x206c, rd(dispcc, 0x206c) | 1);
	bool auxOn = wait_bits(dispcc, 0x206c, 1u << 31, 0);
	dprintf("qcom_probe: dp0 aux: clock %#" B_PRIx32 " (%s)\n",
		rd(dispcc, 0x206c), auxOn ? "on" : "off");
	delete_area(dispccArea);
	if (!auxOn)
		return;

	area_id area;
	volatile uint8* dp = map_rw({"probe dp0", 0xae90000, 0x1000}, area);
	if (dp == NULL)
		return;
	const uint32 aux = 0x200, intrStatus = 0x020;
	wr32(dp, aux + 0x30, rd(dp, aux + 0x30) | 2);		// AUX reset
	snooze(1000);
	wr32(dp, aux + 0x30, rd(dp, aux + 0x30) & ~2u);
	wr32(dp, aux + 0x3c, 0xffff);						// timeout count
	wr32(dp, aux + 0x40, 0xffff);						// limits
	wr32(dp, aux + 0x30, rd(dp, aux + 0x30) | 1);		// enable

	// a native read of one byte at DPCD 0: the request's four bytes
	static const uint8 kHeader[4] = {0x10, 0, 0, 0};
	for (int i = 0; i < 4; i++)
		wr32(dp, aux + 0x34, (uint32)kHeader[i] << 8 | (i == 0 ? 1u << 31 : 0));
	wr32(dp, aux + 0x38, 0);
	wr32(dp, aux + 0x4c, 0x1f);
	wr32(dp, aux + 0x4c, 0x9f);
	wr32(dp, aux + 0x4c, 0);
	wr32(dp, intrStatus, rd(dp, intrStatus) << 1);
	wr32(dp, aux + 0x38, 1u << 9);						// go
	uint32 status = 0;
	bigtime_t start = system_time();
	while (system_time() - start < 500000) {
		status = rd(dp, intrStatus) & 0x0a249248;
		if (status != 0)
			break;
		spin(20);
	}
	wr32(dp, intrStatus, status << 1);
	wr32(dp, aux + 0x38, 0);
	dprintf("qcom_probe: dp0 aux: DPCD read after %" B_PRId64 " us: status %#"
		B_PRIx32 "%s%s%s, aux ctrl %#" B_PRIx32 ", phy aux status %#" B_PRIx32
		"\n", system_time() - start, status,
		(status & (1u << 3)) != 0 ? " done" : "",
		(status & (1u << 9)) != 0 ? " timeout" : "",
		status == 0 ? " nothing" : "", rd(dp, aux + 0x30),
		rd(dp, aux + 0xbc));
	delete_area(area);
}


static void
dp_phy_test()
{
	area_id gccArea;
	volatile uint8* gcc = map_rw({"probe gcc dp", 0x100000, 0x1f4200},
		gccArea);
	if (gcc == NULL)
		return;

	// USB30_PRIM_GDSC: on (bit 0 SW_COLLAPSE clear), status in bit 31
	uint32 gdsc = rd(gcc, 0xf004);
	if ((gdsc & 1) != 0)
		wr32(gcc, 0xf004, gdsc & ~1u);
	wait_bits(gcc, 0xf004, 1u << 31, 1u << 31);
	dprintf("qcom_probe: dp phy: usb30 prim gdsc %#" B_PRIx32 " -> %#" B_PRIx32
		"\n", gdsc, rd(gcc, 0xf004));
	// aux, ref (usb4 eud clkref), com aux
	static const uint32 kBranches[] = {0xf054, 0x8c02c, 0xf058};
	for (uint32 branch : kBranches) {
		uint32 before = rd(gcc, branch);
		wr32(gcc, branch, before | 1);
		spin(100);
		dprintf("qcom_probe: dp phy: gcc %#" B_PRIx32 " %#" B_PRIx32 " -> %#"
			B_PRIx32 "\n", branch, before, rd(gcc, branch));
	}

	// vdda-phy L9D 0.912 V, vdda-pll L4D 1.2 V
	ldo_on("ldod9", 912);
	ldo_on("ldod4", 1200);

	// phy and common resets (GCC_USB3_PHY_PRIM_BCR, GCC_USB4_DP_PHY_PRIM_BCR)
	wr32(gcc, 0x50000, rd(gcc, 0x50000) | 1);
	wr32(gcc, 0x4a008, rd(gcc, 0x4a008) | 1);
	spin(200);
	wr32(gcc, 0x50000, rd(gcc, 0x50000) & ~1u);
	wr32(gcc, 0x4a008, rd(gcc, 0x4a008) & ~1u);
	spin(200);
	delete_area(gccArea);

	area_id phyArea;
	volatile uint8* phy = map_rw({"probe usb43dp phy", 0x88eb000, 0x4000},
		phyArea);
	if (phy == NULL)
		return;
	volatile uint8* com = phy + kPhyCom;
	volatile uint8* serdes = phy + kPhyDpSerdes;
	volatile uint8* dp = phy + kPhyDpPhy;
	volatile uint8* tx[2] = {phy + kPhyTxA, phy + kPhyTxB};
	dprintf("qcom_probe: dp phy: dp_com mode %#" B_PRIx32 " typec %#" B_PRIx32
		", dp phy revision %#" B_PRIx32 "\n", rd(com, kComPhyModeCtrl),
		rd(com, kComTypecCtrl), rd(dp, 0));

	// qmp_combo_com_init(), USB3 + DP, normal orientation
	wr32(com, kComPowerDownCtrl, rd(com, kComPowerDownCtrl) | 1);
	wr32(com, kComResetOverride, rd(com, kComResetOverride) | 0xf);
	wr32(com, kComTypecCtrl, 1u << 1);
	wr32(com, kComPhyModeCtrl, 3);
	wr32(com, kComResetOverride, rd(com, kComResetOverride) & ~0xfu);
	wr32(com, kComSwiCtrl, rd(com, kComSwiCtrl) & ~3u);
	wr32(com, kComSwReset, rd(com, kComSwReset) & ~1u);

	// qmp_v4_dp_aux_init()
	wr32(dp, kDpPhyPdCtl, 0x01 | 0x02 | 0x04 | 0x20 | 0x40);
	wr32(serdes, kSerdesBiasEn, 0x17);
	static const uint8 kAux[] = {0x00, 0x13, 0xa4, 0x00, 0x0a, 0x26, 0x0a,
		0x03, 0xb7, 0x03};
	for (uint32 i = 0; i < sizeof(kAux); i++)
		wr32(dp, kDpPhyAuxCfg0 + 4 * i, kAux[i]);
	wr32(dp, kDpPhyAuxInterruptMask, 0x1f);

	// qmp_combo_dp_power_on(): serdes at RBR, both lanes' TX
	for (const qmp_setting& setting : kDpSerdes)
		wr32(serdes, setting.offset, setting.value);
	for (const qmp_setting& setting : kDpSerdesRbr)
		wr32(serdes, setting.offset, setting.value);
	for (int lane = 0; lane < 2; lane++) {
		for (const qmp_setting& setting : kDpTx)
			wr32(tx[lane], setting.offset, setting.value);
		// qmp_v4_configure_dp_tx(): swing and pre-emphasis level 0
		wr32(tx[lane], kTxDrvLevel, 0x27);
		wr32(tx[lane], kTxEmpPost1Level, 0x20);
		wr32(tx[lane], kTxDrvLevel, 0x28 | 0x20);
		wr32(tx[lane], kTxEmpPost1Level, 0x20 | 0x20);
	}

	// qmp_v456_configure_dp_phy(): four lanes, normal orientation
	wr32(dp, kDpPhyCfg1, 0x0f);
	wr32(dp, kDpPhyPdCtl, 0x01 | 0x04 | 0x20 | 0x40 | 0x08 | 0x10);
	wr32(dp, kDpPhyMode, 0x5c);
	wr32(dp, kDpPhyAuxCfg0 + 4, 0x13);
	wr32(dp, kDpPhyAuxCfg0 + 8, 0xa4);
	wr32(dp, kDpPhyTx01LaneCtl, 0x05);
	wr32(dp, kDpPhyTx23LaneCtl, 0x05);
	wr32(dp, kDpPhyVcoDiv, 0x1);	// RBR
	wr32(dp, kDpPhyCfg, 0x01);
	wr32(dp, kDpPhyCfg, 0x05);
	wr32(dp, kDpPhyCfg, 0x01);
	wr32(dp, kDpPhyCfg, 0x09);
	wr32(serdes, kSerdesResetsmCntrl, 0x20);
	bool locked = poll_bits(serdes, kSerdesCReadyStatus, 1, "C_READY")
		&& poll_bits(serdes, kSerdesCmnStatus, 1, "PLL lock")
		&& poll_bits(serdes, kSerdesCmnStatus, 2, "PLL ready");
	bool ready = false;
	if (locked) {
		wr32(dp, kDpPhyCfg, 0x19);
		ready = poll_bits(dp, kDpPhyStatus, 1, "PHY status 0")
			&& poll_bits(dp, kDpPhyStatus, 2, "PHY status 1");
	}
	if (ready) {
		// qmp_v4_configure_dp_phy(): drivers and bias for four lanes
		for (int lane = 0; lane < 2; lane++) {
			wr32(tx[lane], kTxHighzDriver, 0x10);
			wr32(tx[lane], kTxTransceiverBias, 0x3f);
		}
		wr32(dp, kDpPhyCfg, 0x18);
		snooze(2000);
		wr32(dp, kDpPhyCfg, 0x19);
		ready = poll_bits(dp, kDpPhyStatus, 2, "PHY ready");
		for (int lane = 0; lane < 2; lane++) {
			wr32(tx[lane], kTxPolInv, 0x0a);
			wr32(tx[lane], kTxDrvLevel, 0x27);
			wr32(tx[lane], kTxEmpPost1Level, 0x20);
		}
	}
	if (ready) {
		// dispcc 0: DP0's link RCG (0x21a4) from the PHY's link clock
		// (disp_cc_parent_map_0: source 1), undivided, then its link
		// branch: running only if the PHY's clock comes
		area_id dispccArea;
		volatile uint8* dispcc = map_rw({"probe dispcc dp0", 0xaf00000,
			0x20000}, dispccArea);
		if (dispcc != NULL) {
			uint32 cfg = rd(dispcc, 0x21a8);
			wr32(dispcc, 0x21a8, (cfg & ~(7u << 8 | 0x1fu)) | 1u << 8);
			wr32(dispcc, 0x21a4, rd(dispcc, 0x21a4) | 1);
			wait_bits(dispcc, 0x21a4, 1, 0);
			uint32 before = rd(dispcc, 0x205c);
			wr32(dispcc, 0x205c, before | 1);
			bool running = wait_bits(dispcc, 0x205c, 1u << 31, 0);
			dprintf("qcom_probe: dp phy: dp0 link rcg cfg %#" B_PRIx32 " -> %#"
				B_PRIx32 " (cmd %#" B_PRIx32 "), link branch %#" B_PRIx32
				" -> %#" B_PRIx32 ": %s\n", cfg, rd(dispcc, 0x21a8),
				rd(dispcc, 0x21a4), before, rd(dispcc, 0x205c),
				running ? "running" : "off");
			delete_area(dispccArea);
		}
	}
	if (ready)
		dp0_aux_test();
	dprintf("qcom_probe: dp phy: PLL %s, PHY %s (c_ready %#" B_PRIx32
		" cmn %#" B_PRIx32 " dp phy status %#" B_PRIx32 ")\n",
		locked ? "locked" : "not locked", ready ? "ready" : "not ready",
		rd(serdes, kSerdesCReadyStatus), rd(serdes, kSerdesCmnStatus),
		rd(dp, kDpPhyStatus));
	delete_area(phyArea);
}


// #pragma mark - the SD stream's SMMU context bank


/*!	What context bank 2 of the apps SMMU (the SD controller's stream 0x4e0)
	holds: its registers, and its stage 1 walk for a few addresses. */
static void
sd_smmu_bank()
{
	area_id area;
	volatile uint8* smmu = map({"probe apps smmu all", 0x15000000, 0x100000},
		area);
	if (smmu == NULL)
		return;
	uint32 idr1 = rd(smmu, 0x24);
	uint32 page = (idr1 & (1u << 31)) != 0 ? 0x10000 : 0x1000;
	uint32 pages = 1u << (((idr1 >> 28) & 7) + 1);
	uint32 bank = 2;
	volatile uint8* cb = smmu + pages * page + bank * page;
	uint32 sctlr = rd(cb, 0), tcr = rd(cb, 0x30), tcr2 = rd(cb, 0x10);
	uint64 ttbr0 = rd(cb, 0x20) | (uint64)rd(cb, 0x24) << 32;
	dprintf("qcom_probe: smmu cb2: cbar %#" B_PRIx32 " cba2r %#" B_PRIx32
		" sctlr %#" B_PRIx32 " tcr %#" B_PRIx32 " tcr2 %#" B_PRIx32 " ttbr0 %#"
		B_PRIx64 " mair %#" B_PRIx32 " fsr %#" B_PRIx32 "\n",
		rd(smmu, page + 4 * bank), rd(smmu, page + 0x800 + 4 * bank), sctlr,
		tcr, tcr2, ttbr0, rd(cb, 0x38), rd(cb, 0x58));
	delete_area(area);
	if ((sctlr & 1) == 0)
		return;

	// a 4 KiB granule walk (TG0 in TCR 15:14 = 0), from the level T0SZ gives
	uint32 t0sz = tcr & 0x3f;
	int startLevel = t0sz >= 25 ? 1 : 0;
	static const uint64 kAddresses[] = {0x80000000, 0x90000000, 0xc0000000,
		0x100000000ull, 0x200000000ull};
	for (uint64 va : kAddresses) {
		uint64 table = ttbr0 & 0xfffffffff000ull;
		char trail[160] = "";
		size_t length = 0;
		for (int level = startLevel; level <= 3; level++) {
			uint32 index = (va >> (39 - 9 * level)) & 0x1ff;
			area_id tableArea;
			volatile uint8* t = map({"probe smmu table", table, 0x1000},
				tableArea);
			if (t == NULL)
				break;
			uint64 entry = rd(t, index * 8) | (uint64)rd(t, index * 8 + 4) << 32;
			delete_area(tableArea);
			length += snprintf(trail + length, sizeof(trail) - length,
				" L%d[%u]=%#" B_PRIx64, level, index, entry);
			if ((entry & 1) == 0 || (entry & 2) == 0 || level == 3)
				break;
			table = entry & 0xfffffffff000ull;
		}
		dprintf("qcom_probe: smmu cb2 va %#" B_PRIx64 ":%s\n", va, trail);
	}
}


// devfs calls init_hardware() when it loads the driver: the probe runs then,
// and the driver reports no device.
status_t
init_hardware()
{
	probe();
	adsp_state();
	dp_phy_test();
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
