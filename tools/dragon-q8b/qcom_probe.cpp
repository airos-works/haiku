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

	// the SD slot's card detect (TLMM GPIO 131, low with a card in) and its
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
				": %s\n", io, (io & 1) == 0 ? "card in" : "no card");
			// and with the pin's own pull-up, in case the board's is to a
			// supply that is off
			*(volatile uint32*)tlmm = 3;
			spin(1000);
			io = rd(tlmm, 4);
			dprintf("qcom_probe: sd card detect, pull-up: in/out %#" B_PRIx32
				": %s\n", io, (io & 1) == 0 ? "card in" : "no card");
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
		uint32 ack = 0;
		if ((status & 0x3) != 0)
			ack |= 1u << 0;		// bus on/off done
		if ((status & 0xc) != 0)
			ack |= 1u << 2;		// I/O high/low done
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

	wr8(sd, 0x29, 0x0e);		// 3.3 V
	wr8(sd, 0x29, 0x0f);		// bus power on
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
