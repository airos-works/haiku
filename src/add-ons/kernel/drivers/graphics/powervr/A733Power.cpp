/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "A733Power.h"

#include <KernelExport.h>
#include <vm/vm.h>

// the R_TWI controller and pin helpers of the display driver
#include "registers.h"
#include "twi.h"

#include <sunxi_pmic.h>


#define TRACE(x...)	dprintf("powervr: " x)


namespace powervr {


static const phys_addr_t kCcu			= 0x02002000;
static const phys_addr_t kRCcu			= 0x07010000;
static const phys_addr_t kPck600		= 0x07060000;

enum {
	CCU_PLL_GPU			= 0x00e0,	// b31 enable, b30 LDO, b29 lock enable,
									// b28 locked, b27 output gate
	CCU_GPU_CLOCK		= 0x0b20,	// b31 gate, b27 update, [26:24] mux, [3:0] M-1
	CCU_GPU_BUS			= 0x0b24,	// b0 bus gate, b16 reset deassert
	CCU_AHB_GATES		= 0x05c0,	// b7 GPU (critical, already on)
	CCU_MBUS_GATES		= 0x05e0,	// b16 GPU (critical, already on)
	R_CCU_PPU			= 0x01ac,	// b0 PCK-600 bus clock

	PPU_GPU_TOP			= 5,
	PPU_GPU_CORE		= 6,
	PPU_POLICY			= 0x000,
	PPU_STATUS			= 0x008,
	PPU_DEVICE_REQUESTS	= 0x020,	// PWCR
	PPU_ON				= 0x8,

	// gate, update, pll-gpu / pll-peri0-800m / pll-peri0-600m, / 1
	// (the mux: pll-gpu, peri0-800m, -600m, -400m, -300m, -200m)
	GPU_CLOCK_PLL_GPU	= 0x88000000,
	GPU_CLOCK_800MHZ	= 0x89000000,
	GPU_CLOCK_600MHZ	= 0x8a000000
};


static inline uint32
read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


static status_t
wait_power_domain(volatile uint8* pck, uint32 domain)
{
	volatile uint8* ppu = pck + domain * 0x1000;
	bigtime_t deadline = system_time() + 10000;
	while ((read32(ppu, PPU_STATUS) & 0xf) != PPU_ON) {
		if (system_time() > deadline) {
			TRACE("power domain %" B_PRIu32 " does not come up, status %#"
				B_PRIx32 "\n", domain, read32(ppu, PPU_STATUS));
			return B_TIMED_OUT;
		}
		spin(10);
	}
	return B_OK;
}


/*!	Allwinner's PPU set-up (pck600_domains.c, sunxi_pd_init): the device
	control and logic power switch delays, then the policy. A policy that
	already says ON while the domain is off is taken back first, so that
	the PPU sees a request.
*/
static void
request_power_domain(volatile uint8* pck, uint32 domain)
{
	volatile uint8* ppu = pck + domain * 0x1000;
	if ((read32(ppu, PPU_STATUS) & 0xf) == PPU_ON)
		return;

	write32(ppu, 0x170, 0x001f1f1f);
	write32(ppu, 0x174, 0x00001f1f);
	write32(ppu, 0xc00, 0x08080808);
	write32(ppu, 0xc04, 0x00000808);
	write32(ppu, 0xc10, 0x00000008);
	if ((read32(ppu, PPU_POLICY) & 0xf) == PPU_ON) {
		write32(ppu, PPU_POLICY, read32(ppu, PPU_POLICY) & ~0xfu);
		spin(10);
	}
	write32(ppu, PPU_POLICY, (read32(ppu, PPU_POLICY) & ~0xfu) | PPU_ON);
}


static void
log_state(volatile uint8* ccu, volatile uint8* rccu, volatile uint8* pck,
	const char* when)
{
	for (uint32 domain = PPU_GPU_TOP; domain <= PPU_GPU_CORE; domain++) {
		volatile uint8* ppu = pck + domain * 0x1000;
		TRACE("%s: domain %" B_PRIu32 ": PWPR %#" B_PRIx32 " PWSR %#" B_PRIx32
			" DISR %#" B_PRIx32 " MISR %#" B_PRIx32 " PWCR %#" B_PRIx32
			" DCDR %#" B_PRIx32 " LPS %#" B_PRIx32 "\n", when, domain,
			read32(ppu, 0x000), read32(ppu, 0x008), read32(ppu, 0x010),
			read32(ppu, 0x014), read32(ppu, 0x020), read32(ppu, 0x170),
			read32(ppu, 0xc00));
	}
	TRACE("%s: GPU_TOP %#" B_PRIx32 "/%#" B_PRIx32 ", GPU_CORE %#" B_PRIx32
		"/%#" B_PRIx32 ", clock %#" B_PRIx32 ", bus %#" B_PRIx32 ", AHB %#"
		B_PRIx32 ", MBUS %#" B_PRIx32 ", PPU clock %#" B_PRIx32 "\n", when,
		read32(pck + PPU_GPU_TOP * 0x1000, PPU_POLICY),
		read32(pck + PPU_GPU_TOP * 0x1000, PPU_STATUS),
		read32(pck + PPU_GPU_CORE * 0x1000, PPU_POLICY),
		read32(pck + PPU_GPU_CORE * 0x1000, PPU_STATUS),
		read32(ccu, CCU_GPU_CLOCK), read32(ccu, CCU_GPU_BUS),
		read32(ccu, CCU_AHB_GATES), read32(ccu, CCU_MBUS_GATES),
		read32(rccu, R_CCU_PPU));
}


/*!	PLL_GPU at whatever the boot firmware programmed (24 MHz x 70 / 2, 840
	MHz): enable, lock, output on, as the CCU driver does for any PLL.
*/
static status_t
enable_pll_gpu(volatile uint8* ccu)
{
	uint32 value = read32(ccu, CCU_PLL_GPU);
	if ((value & (1u << 31)) != 0 && (value & (1u << 27)) != 0)
		return B_OK;

	write32(ccu, CCU_PLL_GPU, value | (1u << 30));
	write32(ccu, CCU_PLL_GPU, read32(ccu, CCU_PLL_GPU) | (1u << 31));
	write32(ccu, CCU_PLL_GPU, read32(ccu, CCU_PLL_GPU) & ~(1u << 29));
	write32(ccu, CCU_PLL_GPU, read32(ccu, CCU_PLL_GPU) | (1u << 29));
	bigtime_t deadline = system_time() + 2000;
	while ((read32(ccu, CCU_PLL_GPU) & (1u << 28)) == 0) {
		if (system_time() > deadline) {
			TRACE("PLL_GPU does not lock: %#" B_PRIx32 "\n",
				read32(ccu, CCU_PLL_GPU));
			return B_TIMED_OUT;
		}
		spin(5);
	}
	spin(20);
	write32(ccu, CCU_PLL_GPU, read32(ccu, CCU_PLL_GPU) | (1u << 27));
	return B_OK;
}


/*!	The GPU's supply, DCDC4 of the AXP8191 PMIC (I2C address 0x36 on
	R_TWI0): Allwinner's driver enables it at 0.8 V before anything else,
	and Linux's regulator core turns it on at boot; the boot firmware does
	not. Register 0x10 bit 3 enables it, 0x15[6:0] sets the voltage
	(0x1e = 0.8 V; axp2101-regulator.c of the BSP).

	Through the sunxi_regulator driver's PMIC module where there is one: it
	serializes the bus with the CPU voltage scaling (sunxi_thermal). Without
	it, directly on the controller.
*/
static status_t
enable_gpu_supply_direct()
{
	sunxi::r_pin_set_function(0, 0, 2);	// PL0, PL1: R_TWI0
	sunxi::r_pin_set_function(0, 1, 2);
	sunxi::Twi twi;
	status_t status = twi.Init(0x07083000, 0x19c, 0);
	if (status != B_OK)
		return status;

	uint8 enables, voltage;
	status = twi.Read8(0x36, 0x10, enables);
	if (status == B_OK)
		status = twi.Read8(0x36, 0x15, voltage);
	if (status != B_OK) {
		TRACE("PMIC unreadable: %s\n", strerror(status));
		return status;
	}
	TRACE("DCDC4: %s, voltage code %#x\n",
		(enables & (1u << 3)) != 0 ? "on" : "off", voltage & 0x7f);
	if ((enables & (1u << 3)) != 0 && (voltage & 0x7f) == 0x1e)
		return B_OK;

	status = twi.Write8(0x36, 0x15, (voltage & ~0x7f) | 0x1e);
	if (status == B_OK)
		status = twi.Write8(0x36, 0x10, enables | (1u << 3));
	snooze(1000);
	if (status == B_OK)
		status = twi.Read8(0x36, 0x10, enables);
	TRACE("DCDC4 to 0.8 V: %s, enables now %#x\n", strerror(status), enables);
	return status;
}


static status_t
enable_gpu_supply()
{
	sunxi_pmic_module_info* pmic;
	if (get_module(SUNXI_PMIC_MODULE_NAME, (module_info**)&pmic) != B_OK)
		return enable_gpu_supply_direct();

	uint8 enables, voltage;
	status_t status = pmic->read(0x10, &enables);
	if (status == B_OK)
		status = pmic->read(0x15, &voltage);
	if (status == B_OK) {
		TRACE("DCDC4: %s, voltage code %#x\n",
			(enables & (1u << 3)) != 0 ? "on" : "off", voltage & 0x7f);
		if ((enables & (1u << 3)) == 0 || (voltage & 0x7f) != 0x1e) {
			status = pmic->update(0x15, 0x7f, 0x1e);
			if (status == B_OK)
				status = pmic->update(0x10, 1u << 3, 1u << 3);
			snooze(1000);
			TRACE("DCDC4 to 0.8 V: %s\n", strerror(status));
		}
	} else
		TRACE("PMIC unreadable: %s\n", strerror(status));
	put_module(SUNXI_PMIC_MODULE_NAME);
	return status;
}


/*!	\a megahertz is 800 or 600: both run at 0.8 V on this chip's speed
	grade (the BSP's GPU operating points for vf0500: 400, 600 and 800 MHz
	at 0.8 V, 1008 MHz at 0.88 V).
*/
status_t
a733_gpu_power_on(uint32 megahertz, uint32* _coreClock)
{
	volatile uint8* ccu;
	volatile uint8* rccu;
	volatile uint8* pck;
	area_id ccuArea = map_physical_memory("a733 ccu", kCcu, 0x2000,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&ccu);
	area_id rccuArea = map_physical_memory("a733 r_ccu", kRCcu, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&rccu);
	area_id pckArea = map_physical_memory("a733 pck-600", kPck600, 0xb000,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&pck);
	status_t status = B_OK;
	if (ccuArea < 0 || rccuArea < 0 || pckArea < 0)
		status = B_NO_MEMORY;

	if (status == B_OK) {
		log_state(ccu, rccu, pck, "before");
		status = enable_gpu_supply();
	}
	if (status == B_OK) {

		if ((read32(rccu, R_CCU_PPU) & 1) == 0)
			write32(rccu, R_CCU_PPU, read32(rccu, R_CCU_PPU) | 1);

		// GPU_TOP is on from the boot firmware. GPU_CORE only reaches ON
		// once the GPU is clocked: Allwinner's driver clears its device
		// request enables, sets the policy and turns the clocks on, without
		// waiting in between.
		request_power_domain(pck, PPU_GPU_TOP);
		status = wait_power_domain(pck, PPU_GPU_TOP);
	}
	if (status == B_OK)
		status = enable_pll_gpu(ccu);
	if (status == B_OK) {
		write32(pck + PPU_GPU_CORE * 0x1000, PPU_DEVICE_REQUESTS, 0);
		request_power_domain(pck, PPU_GPU_CORE);

		if ((read32(ccu, CCU_AHB_GATES) & (1u << 7)) == 0
			|| (read32(ccu, CCU_MBUS_GATES) & (1u << 16)) == 0) {
			TRACE("warning: the GPU's AHB or MBUS master gate is off\n");
		}

		// GPU_CORE completes its power-up only while the GPU runs from
		// PLL_GPU (as Allwinner's driver brings it up; from pll-peri0-600m
		// the domain stays off). Clocks first, then out of reset after
		// >= 32 GPU cycles (Linux's order, pvr_power.c).
		write32(ccu, CCU_GPU_CLOCK, GPU_CLOCK_PLL_GPU);
		write32(ccu, CCU_GPU_BUS, read32(ccu, CCU_GPU_BUS) | 1u);
		spin(1);
		write32(ccu, CCU_GPU_BUS, read32(ccu, CCU_GPU_BUS) | (1u << 16));
		spin(1);

		status = wait_power_domain(pck, PPU_GPU_CORE);
		// then the clock the firmware is told about (sunxi_thermal lowers
		// it to the 600-200 MHz taps when it throttles)
		const bool fast = megahertz != 600;
		write32(ccu, CCU_GPU_CLOCK,
			fast ? GPU_CLOCK_800MHZ : GPU_CLOCK_600MHZ);
		log_state(ccu, rccu, pck, "after");
		*_coreClock = fast ? 800000000 : 600000000;
	}

	if (ccuArea >= 0)
		delete_area(ccuArea);
	if (rccuArea >= 0)
		delete_area(rccuArea);
	if (pckArea >= 0)
		delete_area(pckArea);
	return status;
}


}	// namespace powervr
