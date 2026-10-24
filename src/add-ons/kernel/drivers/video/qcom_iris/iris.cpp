/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Qualcomm SC8280XP's video codec (Iris, VPU 2.0), for the Radxa Dragon
	Q8B, after Linux 6.18's drivers/media/platform/qcom/iris (its SM8250
	platform, HFI generation 1, which the SC8280XP's device tree falls back to).

	The firmware leaves the whole block off. Bring-up, at the first open:
	GCC's video branches and resets, video PLL0 and the mvs0 clocks, the
	mvs0c and mvs0 power domains (videocc-sm8350.c), the apps SMMU passing
	the codec's streams through untranslated (it faults unmatched ones), the
	HFI queues, the firmware through TrustZone's PAS, its boot handshake and
	HFI's system init. */


#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <fcntl.h>
#include <libfdt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <qcom_iris.h>
#include <team.h>

#include "qcom_scm.h"


#define INFO(x...)	dprintf("qcom_iris: " x)
#define ERROR(x...)	dprintf("qcom_iris: " x)


int32 api_version = B_CUR_DRIVER_API_VERSION;

extern void* gFDT;

static const char* sDeviceNames[] = { "video/qcom_iris/0", NULL };


//	#pragma mark - registers


// GCC: the video branches (gcc-sc8280xp.c) and the AXI reset
static const phys_addr_t kGccBase = 0x100000;
static const uint32 kGccVideoAhb = 0x28004;
static const uint32 kGccVideoAxi0 = 0x28010;
static const uint32 kGccVideoXo = 0x28028;

// videocc (videocc-sm8350.c, the SC8280XP values)
static const phys_addr_t kVideoccBase = 0xabf0000;
static const uint32 kVideoPll0 = 0x42c;
static const uint32 kMvs0Rcg = 0xb94;
static const uint32 kMvs0cGdsc = 0xbf8;
static const uint32 kMvs0cClock = 0xc34;
static const uint32 kMvs0Gdsc = 0xd18;
static const uint32 kMvs0Clock = 0xd34;
static const uint32 kVideoccXo = 0xf34;

// Lucid 5LPE PLL registers (clk-alpha-pll.c, Trion layout)
enum {
	PLL_MODE			= 0x00,
	PLL_L_VAL			= 0x04,
	PLL_CAL_L_VAL		= 0x08,
	PLL_USER_CTL		= 0x0c,
	PLL_USER_CTL_U		= 0x10,
	PLL_USER_CTL_U1		= 0x14,
	PLL_CONFIG_CTL		= 0x18,
	PLL_CONFIG_CTL_U	= 0x1c,
	PLL_CONFIG_CTL_U1	= 0x20,
	PLL_TEST_CTL		= 0x24,
	PLL_TEST_CTL_U		= 0x28,
	PLL_TEST_CTL_U1		= 0x2c,
	PLL_OPMODE			= 0x38,
	PLL_ALPHA_VAL		= 0x40,
};
static const uint32 kPllOutControl = 1u << 0;
static const uint32 kPllResetN = 1u << 2;
static const uint32 kPllUpdateBypass = 1u << 23;
static const uint32 kPllLockDetect = 1u << 31;

// clock branches, RCGs and GDSCs
static const uint32 kBranchEnable = 1u << 0;
static const uint32 kBranchReset = 1u << 2;
static const uint32 kBranchOff = 1u << 31;
static const uint32 kGdscSwCollapse = 1u << 0;
static const uint32 kGdscHwControl = 1u << 1;
static const uint32 kGdscRetainFf = 1u << 11;
static const uint32 kGdscPowerOn = 1u << 31;

// the apps SMMU (MMU-500): stream match registers and their S2CRs
static const phys_addr_t kAppsSmmuBase = 0x15000000;
static const uint32 kSmmuIdr0 = 0x20;
static const uint32 kSmmuSmr = 0x800;
static const uint32 kSmmuS2cr = 0xc00;
static const uint32 kSmrValid = 1u << 31;
static const uint32 kS2crBypass = 1u << 16;
// the codec's streams (the device tree's "iommus"): ID and mask
static const uint32 kStreams[][2] = { {0x2a00, 0x400}, {0x2a07, 0x400} };

// Iris (iris_vpu_common.c, iris_vpu_register_defines.h)
static const phys_addr_t kIrisBase = 0xaa00000;
static const size_t kIrisSize = 0x100000;
enum {
	CPU_CS_A2HSOFTINTCLR	= 0xa001c,
	CTRL_INIT				= 0xa0048,
	CTRL_STATUS				= 0xa004c,
	QTBL_INFO				= 0xa0050,
	QTBL_ADDR				= 0xa0054,
	CPU_CS_SCIACMDARG3		= 0xa0058,
	SFR_ADDR				= 0xa005c,
	UC_REGION_ADDR			= 0xa0064,
	UC_REGION_SIZE			= 0xa0068,
	CPU_CS_H2XSOFTINTEN		= 0xa0148,
	CPU_IC_SOFTINT			= 0xa0150,
	CPU_CS_X2RPMH			= 0xa0168,
	WRAPPER_INTR_STATUS		= 0xb000c,
	WRAPPER_INTR_MASK		= 0xb0010,
	WRAPPER_PRESET			= 0xb0088,	// iris_set_sm8250_preset_registers()
	WRAPPER_CORE_POWER_STATUS = 0xb0080,
};
static const uint32 kCtrlErrorMask = 0xfe;
static const uint32 kIntrA2hWatchdog = 1u << 3;
static const uint32 kIntrA2h = 1u << 2;

// the firmware
#define IRIS_PAS_ID				9
#define IRIS_FIRMWARE			"qcom/vpu/vpu20_p4.mbn"
// tz_cp_config_sm8250: content protected address ranges
static const uint32 kCpStart = 0;
static const uint32 kCpSize = 0x25800000;
static const uint32 kCpNonpixelStart = 0x01000000;
static const uint32 kCpNonpixelSize = 0x24800000;
#define QCOM_SCM_MP_VIDEO_VAR	0x08

// what the firmware can address (sm8250 dma_mask)
static const phys_addr_t kDmaLimit = 0xe0000000;

// the codec's interrupt (SPI 174, the device tree's)
static const uint32 kIrisInterrupt = 174 + 32;


//	#pragma mark - HFI (iris_hfi_queue.h, iris_hfi_gen1_defines.h)


#define IFACEQ_MAX_PKT_SIZE		1024
#define IFACEQ_CORE_PKT_SIZE	(1024 * 4)	// what one message may take
#define IFACEQ_MAX_BUF_COUNT	64
#define IFACE_MAX_PARALLEL_SESSIONS 16
#define IFACEQ_QUEUE_SIZE		(IFACEQ_MAX_PKT_SIZE * IFACEQ_MAX_BUF_COUNT \
									* IFACE_MAX_PARALLEL_SESSIONS)
#define IFACEQ_NUMQ				3
#define IFACEQ_DFLT_QHDR		0x0101
#define SFR_SIZE				4096

enum {
	IFACEQ_CMDQ_ID = 0,
	IFACEQ_MSGQ_ID,
	IFACEQ_DBGQ_ID,
};

struct hfi_queue_header {
	uint32	status;
	uint32	start_addr;
	uint16	queue_type;
	uint16	header_type;
	uint32	q_size;
	uint32	pkt_size;
	uint32	pkt_drop_cnt;
	uint32	rx_wm;
	uint32	tx_wm;
	uint32	rx_req;
	uint32	tx_req;
	uint32	rx_irq_status;
	uint32	tx_irq_status;
	uint32	read_idx;
	uint32	write_idx;
};

// as Linux lays it out on arm64 (the firmware reads it): a 64-bit pointer
struct hfi_queue_table_header {
	uint32	version;
	uint32	size;
	uint32	qhdr0_offset;
	uint32	qhdr_size;
	uint32	num_q;
	uint32	num_active_q;
	uint64	device_addr;
	char	name[256];
	hfi_queue_header q_hdr[IFACEQ_NUMQ];
};
static_assert(sizeof(hfi_queue_table_header) == 456, "queue table layout");

#define HFI_VIDEO_ARCH_OX				0x1
#define HFI_CMD_SYS_INIT				0x10001
#define HFI_CMD_SYS_SET_PROPERTY		0x10005
#define HFI_CMD_SYS_GET_PROPERTY		0x10006
#define HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL 0x5
#define HFI_PROPERTY_SYS_IMAGE_VERSION	0x6
#define HFI_MSG_SYS_INIT				0x20001
#define HFI_MSG_SYS_PROPERTY_INFO		0x2000a
#define HFI_MSG_EVENT_NOTIFY			0x21001


//	#pragma mark - state


struct register_window {
	area_id			area = -1;
	volatile uint8*	base = NULL;

	status_t Map(const char* name, phys_addr_t physical, size_t size)
	{
		area = map_physical_memory(name, physical, size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
		return area < 0 ? area : B_OK;
	}

	void Unmap()
	{
		if (area >= 0)
			delete_area(area);
		area = -1;
	}

	uint32 Read(uint32 offset) const
		{ return *(volatile uint32*)(base + offset); }
	void Write(uint32 offset, uint32 value)
		{ *(volatile uint32*)(base + offset) = value; }
	void Set(uint32 offset, uint32 bits)
		{ Write(offset, Read(offset) | bits); }
	void Clear(uint32 offset, uint32 bits)
		{ Write(offset, Read(offset) & ~bits); }

	bool Wait(uint32 offset, uint32 mask, uint32 value, bigtime_t timeout)
	{
		bigtime_t end = system_time() + timeout;
		while ((Read(offset) & mask) != value) {
			if (system_time() > end)
				return false;
			spin(10);
		}
		return true;
	}
};


static register_window sGcc, sVideocc, sIris;
static mutex sLock = MUTEX_INITIALIZER("qcom iris");
static status_t sStartStatus = B_NO_INIT;

// the queue table, the three queues and the SFR in one contiguous block
static area_id sQueueArea = -1;
static uint8* sQueues;
static phys_addr_t sQueuesPhysical;
static size_t sQueuesSize;
static hfi_queue_table_header* sQueueTable;

static char sFirmwareVersion[128];
static uint32 sSysInitProperties;

static ConditionVariable sMessageCondition;
static int32 sInterrupts;
static bool sInterruptInstalled;


//	#pragma mark - firmware files


static void*
load_firmware(const char* name, size_t* _size)
{
	static const char* kDirectories[] = {
		"/boot/system/non-packaged/data/firmware",
		"/boot/system/data/firmware",
	};
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", kDirectories[i], name);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		off_t size = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		void* data = size > 0 ? malloc(size) : NULL;
		if (data != NULL && read(fd, data, size) != size) {
			free(data);
			data = NULL;
		}
		close(fd);
		if (data != NULL) {
			*_size = size;
			INFO("firmware %s, %" B_PRIdOFF " bytes\n", path, size);
			return data;
		}
	}
	ERROR("firmware %s not found\n", name);
	return NULL;
}


//	#pragma mark - power


static status_t
branch_enable(register_window& block, uint32 offset, const char* name)
{
	block.Set(offset, kBranchEnable);
	if (!block.Wait(offset, kBranchOff, 0, 10000)) {
		ERROR("clock %s does not come on (%#" B_PRIx32 ")\n", name,
			block.Read(offset));
		return B_TIMED_OUT;
	}
	return B_OK;
}


static void
branch_reset(register_window& block, uint32 offset)
{
	block.Set(offset, kBranchReset);
	spin(400);
	block.Clear(offset, kBranchReset);
	spin(400);
}


static status_t
gdsc_enable(uint32 offset, const char* name)
{
	sVideocc.Set(offset, kGdscRetainFf);
	sVideocc.Clear(offset, kGdscSwCollapse);
	if (!sVideocc.Wait(offset, kGdscPowerOn, kGdscPowerOn, 10000)) {
		ERROR("power domain %s does not come on (%#" B_PRIx32 ")\n", name,
			sVideocc.Read(offset));
		return B_TIMED_OUT;
	}
	return B_OK;
}


/*!	Video PLL0 at 720 MHz (L 0x25, alpha 0x8000: the lowest mvs0 rate of
	the SC8280XP's table), as clk_lucid_pll_configure() and
	alpha_pll_lucid_5lpe_enable() set it up.
*/
static status_t
pll0_start()
{
	uint32 pll = kVideoPll0;
	if ((sVideocc.Read(pll + PLL_MODE) & kPllLockDetect) != 0
		&& (sVideocc.Read(pll + PLL_MODE) & kPllOutControl) != 0) {
		return B_OK;
	}
	sVideocc.Write(pll + PLL_L_VAL, 0x25);
	sVideocc.Write(pll + PLL_CAL_L_VAL, 0x44);
	sVideocc.Write(pll + PLL_ALPHA_VAL, 0x8000);
	sVideocc.Write(pll + PLL_CONFIG_CTL, 0x20485699);
	sVideocc.Write(pll + PLL_CONFIG_CTL_U, 0x00002261);
	sVideocc.Write(pll + PLL_CONFIG_CTL_U1, 0x2a9a699c);
	sVideocc.Write(pll + PLL_USER_CTL, 0);
	sVideocc.Write(pll + PLL_USER_CTL_U, 0x00000805);
	sVideocc.Write(pll + PLL_USER_CTL_U1, 0);
	sVideocc.Write(pll + PLL_TEST_CTL, 0);
	sVideocc.Write(pll + PLL_TEST_CTL_U, 0);
	sVideocc.Write(pll + PLL_TEST_CTL_U1, 0x01800000);
	sVideocc.Set(pll + PLL_MODE, kPllUpdateBypass);
	sVideocc.Clear(pll + PLL_MODE, kPllOutControl);
	sVideocc.Write(pll + PLL_OPMODE, 0);	// standby
	sVideocc.Set(pll + PLL_MODE, kPllResetN);

	// run
	sVideocc.Write(pll + PLL_OPMODE, 1);
	if (!sVideocc.Wait(pll + PLL_MODE, kPllLockDetect, kPllLockDetect,
			200000)) {
		ERROR("video PLL0 does not lock (mode %#" B_PRIx32 ")\n",
			sVideocc.Read(pll + PLL_MODE));
		return B_TIMED_OUT;
	}
	sVideocc.Set(pll + PLL_USER_CTL, 0x7);	// outputs
	sVideocc.Set(pll + PLL_MODE, kPllOutControl);
	return B_OK;
}


/*!	The mvs0 RCG on PLL0, divided by 1. */
static status_t
mvs0_clock_source()
{
	sVideocc.Write(kMvs0Rcg + 4, 1u << 8 | 1);	// CFG: PLL0, divider 1
	sVideocc.Set(kMvs0Rcg, 1u << 0);			// CMD: update
	if (!sVideocc.Wait(kMvs0Rcg, 1u << 0, 0, 10000)) {
		ERROR("mvs0 clock source does not switch (CMD %#" B_PRIx32 ")\n",
			sVideocc.Read(kMvs0Rcg));
		return B_TIMED_OUT;
	}
	return B_OK;
}


/*!	iris_vpu_power_on(): the controller (mvs0c domain, resets, AXI and core
	clocks), then the hardware (mvs0 domain and clock, then under hardware
	control).
*/
static status_t
power_up()
{
	status_t status = branch_enable(sGcc, kGccVideoAhb, "video ahb");
	if (status == B_OK)
		status = branch_enable(sGcc, kGccVideoXo, "video xo");
	if (status != B_OK)
		return status;
	if (sVideocc.Map("qcom videocc", kVideoccBase, 0x10000) != B_OK)
		return B_NO_MEMORY;
	status = branch_enable(sVideocc, kVideoccXo, "videocc xo");
	if (status == B_OK)
		status = pll0_start();
	if (status == B_OK)
		status = mvs0_clock_source();
	if (status != B_OK)
		return status;

	status = gdsc_enable(kMvs0cGdsc, "mvs0c");
	if (status != B_OK)
		return status;
	// "bus" and "core" resets
	branch_reset(sGcc, kGccVideoAxi0);
	branch_reset(sVideocc, kMvs0cClock);
	status = branch_enable(sGcc, kGccVideoAxi0, "video axi0");
	if (status == B_OK)
		status = branch_enable(sVideocc, kMvs0cClock, "mvs0c");
	if (status != B_OK)
		return status;

	status = gdsc_enable(kMvs0Gdsc, "mvs0");
	if (status == B_OK)
		status = branch_enable(sVideocc, kMvs0Clock, "mvs0");
	if (status != B_OK)
		return status;
	sVideocc.Set(kMvs0Gdsc, kGdscHwControl);

	INFO("powered: PLL0 %#" B_PRIx32 ", mvs0c %#" B_PRIx32 "/%#" B_PRIx32
		", mvs0 %#" B_PRIx32 "/%#" B_PRIx32 "\n",
		sVideocc.Read(kVideoPll0 + PLL_MODE), sVideocc.Read(kMvs0cGdsc),
		sVideocc.Read(kMvs0cClock), sVideocc.Read(kMvs0Gdsc),
		sVideocc.Read(kMvs0Clock));
	return B_OK;
}


//	#pragma mark - SMMU


/*!	The codec's address space on the apps SMMU: a stage 1 table of 4 GiB
	(T0SZ 32, from level 1). The driver's queues are identity mapped, the
	buffers it allocates for userland are in kBufferWindow. Anything else
	faults in the context bank, which the SoC survives; an unmatched or
	bypassed stream that strays does not.
*/
class IrisSpace {
public:
	status_t Init()
	{
		fLevel1 = AllocateTable(fLevel1Physical);
		return fLevel1 != NULL ? B_OK : B_NO_MEMORY;
	}

	phys_addr_t Root() const { return fLevel1Physical; }

	/*!	\a cached: write-back (the codec is coherent), else non-cacheable
		(MAIR below: index 0 and 1). */
	status_t Map(uint64 address, phys_addr_t physical, size_t size,
		bool cached)
	{
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
			uint64* level3 = Level3(address + offset, true);
			if (level3 == NULL)
				return B_NO_MEMORY;
			level3[((address + offset) >> 12) & 511] = ((physical + offset)
					& 0x0000fffffffff000ull)
				| 1ull << 0 | 1ull << 1 | (cached ? 0 : 1ull << 2) | 1ull << 6
				| 3ull << 8 | 1ull << 10;
		}
		memory_full_barrier();
		return B_OK;
	}

	void Unmap(uint64 address, size_t size)
	{
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
			uint64* level3 = Level3(address + offset, false);
			if (level3 != NULL)
				level3[((address + offset) >> 12) & 511] = 0;
		}
		memory_full_barrier();
	}

private:
	uint64* AllocateTable(phys_addr_t& physical)
	{
		void* table;
		virtual_address_restrictions virtualRestrictions = {};
		physical_address_restrictions physicalRestrictions = {};
		area_id area = create_area_etc(B_SYSTEM_TEAM, "qcom iris page table",
			B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			0, 0, &virtualRestrictions, &physicalRestrictions, &table);
		if (area < 0)
			return NULL;
		memset(table, 0, B_PAGE_SIZE);
		physical_entry entry;
		get_memory_map(table, B_PAGE_SIZE, &entry, 1);
		physical = entry.address;
		return (uint64*)table;
	}

	//!	The level 3 table of \a address, made when \a create.
	uint64* Level3(uint64 address, bool create)
	{
		if (address >= (1ull << 32))
			return NULL;
		uint32 gigabyte = address >> 30;
		uint32 block = address >> 21;
		if (fLevel2[gigabyte] == NULL) {
			if (!create)
				return NULL;
			phys_addr_t physical;
			fLevel2[gigabyte] = AllocateTable(physical);
			if (fLevel2[gigabyte] == NULL)
				return NULL;
			memory_full_barrier();
			fLevel1[gigabyte] = physical | 3;
		}
		if (fLevel3[block] == NULL) {
			if (!create)
				return NULL;
			phys_addr_t physical;
			fLevel3[block] = AllocateTable(physical);
			if (fLevel3[block] == NULL)
				return NULL;
			memory_full_barrier();
			fLevel2[gigabyte][block & 511] = physical | 3;
		}
		return fLevel3[block];
	}

	uint64*		fLevel1 = NULL;
	phys_addr_t	fLevel1Physical = 0;
	uint64*		fLevel2[4] = {};
	uint64*		fLevel3[2048] = {};	// one for each 2 MiB
};


static IrisSpace sSpace;
static register_window sSmmu;
static int32 sContextBank = -1;
static uint32 sSmmuPage;
static uint32 sSmmuContexts;		// offset of context bank 0


static uint32
smmu_context(uint32 bank)
{
	return sSmmuContexts + bank * sSmmuPage;
}


/*!	A context bank of the apps SMMU no stream uses yet, translating through
	sSpace, and the codec's streams routed to it (as arm-smmu attaches a
	device; the firmware left them unmatched).
*/
static status_t
smmu_attach()
{
	if (sSmmu.Map("qcom iris smmu", kAppsSmmuBase, 0x100000) != B_OK)
		return B_NO_MEMORY;
	uint32 idr1 = sSmmu.Read(0x24);
	sSmmuPage = (idr1 & (1u << 31)) != 0 ? 0x10000 : 0x1000;
	uint32 pages = 1u << (((idr1 >> 28) & 7) + 1);
	sSmmuContexts = pages * sSmmuPage;
	uint32 contexts = idr1 & 0xff;
	uint32 groups = sSmmu.Read(kSmmuIdr0) & 0xff;

	// the banks in use: those a valid stream match entry translates to,
	// and those switched on
	uint64 used[4] = {};
	for (uint32 i = 0; i < groups; i++) {
		if ((sSmmu.Read(kSmmuSmr + 4 * i) & kSmrValid) == 0)
			continue;
		uint32 s2cr = sSmmu.Read(kSmmuS2cr + 4 * i);
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
	}
	for (int32 bank = contexts - 1; bank >= 0; bank--) {
		if ((used[bank / 64] & (1ull << (bank % 64))) != 0
			|| (sSmmu.Read(smmu_context(bank)) & 1) != 0) {
			continue;
		}
		sContextBank = bank;
		break;
	}
	if (sContextBank < 0) {
		ERROR("no free context bank on the apps SMMU\n");
		return B_BUSY;
	}

	status_t status = sSpace.Init();
	if (status != B_OK)
		return status;

	uint32 bank = sContextBank;
	uint32 cb = smmu_context(bank);
	sSmmu.Write(cb + 0x0, 0);								// SCTLR
	sSmmu.Write(sSmmuPage + 0x800 + 4 * bank, 1u << 0);		// CBA2R: VA64
	// CBAR: stage 1 with stage 2 bypass, write-back, VMID bank + 1
	sSmmu.Write(sSmmuPage + 4 * bank, 1u << 16 | 0xfu << 12 | 3u << 8
		| (bank + 1));
	sSmmu.Write(cb + 0x10, 7u << 15 | 2);	// TCR2: 40-bit PA
	// TCR: T0SZ 32, walks write-back inner shareable, no TTBR1
	sSmmu.Write(cb + 0x30, 32 | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23);
	uint64 root = sSpace.Root();
	sSmmu.Write(cb + 0x20, (uint32)root);
	sSmmu.Write(cb + 0x24, (uint32)(root >> 32));
	// MAIR: 0 write-back, 1 non-cacheable, 2 device
	sSmmu.Write(cb + 0x38, 0xffu | 0x44u << 8 | 0x04u << 16);
	sSmmu.Write(cb + 0x3c, 0);
	sSmmu.Write(cb + 0x58, 0xffffffff);		// FSR
	sSmmu.Write(cb + 0x618, 0);				// TLBIALL
	sSmmu.Write(cb + 0x7f0, 0);				// TLBSYNC
	sSmmu.Wait(cb + 0x7f4, 1, 0, 100000);
	// SCTLR: on, TEX remap, access flags, faults reported
	sSmmu.Write(cb + 0x0, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 6);

	for (size_t s = 0; s < B_COUNT_OF(kStreams); s++) {
		uint32 smr = kSmrValid | kStreams[s][1] << 16 | kStreams[s][0];
		int32 free = -1;
		for (uint32 i = 0; i < groups; i++) {
			uint32 value = sSmmu.Read(kSmmuSmr + 4 * i);
			if (value == smr) {
				free = i;
				break;
			}
			if ((value & kSmrValid) == 0 && free < 0)
				free = i;
		}
		if (free < 0) {
			ERROR("no free stream match entry\n");
			return B_BUSY;
		}
		sSmmu.Write(kSmmuS2cr + 4 * free, bank);	// translate
		memory_full_barrier();
		sSmmu.Write(kSmmuSmr + 4 * free, smr);
		INFO("stream %#" B_PRIx32 "/%#" B_PRIx32 " -> context bank %" B_PRIu32
			" (entry %" B_PRId32 ")\n", kStreams[s][0], kStreams[s][1], bank,
			free);
	}
	memory_full_barrier();
	sSmmu.Write(0x68, 0);		// TLBIALLNSNH
	sSmmu.Write(0x70, 0);		// sTLBGSYNC
	sSmmu.Wait(0x74, 1, 0, 100000);
	INFO("apps SMMU: page %#" B_PRIx32 ", %" B_PRIu32 " context banks at %#"
		B_PRIx32 ", using %" B_PRIu32 "\n", sSmmuPage, contexts,
		sSmmuContexts, bank);
	return B_OK;
}


static void
context_report(const char* when)
{
	if (sContextBank < 0)
		return;
	uint32 cb = smmu_context(sContextBank);
	INFO("%s: context bank FSR %#" B_PRIx32 " FAR %#" B_PRIx32 "%08" B_PRIx32
		" FSYNR0 %#" B_PRIx32 " CBFRSYNRA %#" B_PRIx32 "\n", when,
		sSmmu.Read(cb + 0x58), sSmmu.Read(cb + 0x64), sSmmu.Read(cb + 0x60),
		sSmmu.Read(cb + 0x68), sSmmu.Read(sSmmuPage + 0x400 + 4 * sContextBank));
}


/*!	The apps SMMU faults streams it has no match for, and the firmware left
	the codec's without one: free stream match entries for them, passing
	them through (the firmware and the buffers are addressed physically).
*/
static status_t
smmu_bypass()
{
	register_window smmu;
	if (smmu.Map("qcom iris smmu", kAppsSmmuBase, 0x1000) != B_OK)
		return B_NO_MEMORY;
	uint32 groups = smmu.Read(kSmmuIdr0) & 0xff;
	status_t status = B_OK;
	for (size_t s = 0; s < B_COUNT_OF(kStreams) && status == B_OK; s++) {
		uint32 smr = kSmrValid | kStreams[s][1] << 16 | kStreams[s][0];
		int32 free = -1;
		bool present = false;
		for (uint32 i = 0; i < groups; i++) {
			uint32 value = smmu.Read(kSmmuSmr + 4 * i);
			if (value == smr) {
				present = true;
				break;
			}
			if ((value & kSmrValid) == 0 && free < 0)
				free = i;
		}
		if (present)
			continue;
		if (free < 0) {
			ERROR("no free stream match entry for stream %#" B_PRIx32 "\n",
				kStreams[s][0]);
			status = B_BUSY;
			break;
		}
		smmu.Write(kSmmuS2cr + 4 * free, kS2crBypass);
		memory_full_barrier();
		smmu.Write(kSmmuSmr + 4 * free, smr);
		INFO("stream %#" B_PRIx32 "/%#" B_PRIx32 " passes the apps SMMU "
			"(entry %" B_PRId32 ")\n", kStreams[s][0], kStreams[s][1], free);
	}
	memory_full_barrier();
	smmu.Unmap();
	return status;
}


//	#pragma mark - HFI queues


static hfi_queue_header&
queue_header(uint32 queue)
{
	return sQueueTable->q_hdr[queue];
}


static uint8*
queue_memory(uint32 queue)
{
	return sQueues + sizeof(hfi_queue_table_header) + queue * IFACEQ_QUEUE_SIZE;
}


/*!	The queue table, three queues and the SFR, below the firmware's DMA
	limit, uncached: iris_hfi_queues_init().
*/
static status_t
queues_init()
{
	size_t queueBytes = ROUNDUP(sizeof(hfi_queue_table_header)
		+ IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ, B_PAGE_SIZE);
	sQueuesSize = ROUNDUP(queueBytes + SFR_SIZE, 1024 * 1024);

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = kDmaLimit;
	physicalRestrictions.alignment = 1024 * 1024;
	sQueueArea = create_area_etc(B_SYSTEM_TEAM, "qcom iris queues",
		sQueuesSize, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0,
		0, &virtualRestrictions, &physicalRestrictions, (void**)&sQueues);
	if (sQueueArea < 0)
		return sQueueArea;
	physical_entry entry;
	get_memory_map(sQueues, B_PAGE_SIZE, &entry, 1);
	sQueuesPhysical = entry.address;
	// clean the zeroed lines out before the mapping becomes uncached
	for (addr_t line = (addr_t)sQueues; line < (addr_t)sQueues + sQueuesSize;
			line += 64)
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	memory_full_barrier();
	status_t status = vm_set_area_memory_type(sQueueArea, sQueuesPhysical,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK)
		return status;
	memset(sQueues, 0, sQueuesSize);

	status = sSpace.Map(sQueuesPhysical, sQueuesPhysical, sQueuesSize, false);
	if (status != B_OK)
		return status;

	sQueueTable = (hfi_queue_table_header*)sQueues;
	for (uint32 q = 0; q < IFACEQ_NUMQ; q++) {
		hfi_queue_header& header = queue_header(q);
		header.status = 1;
		header.start_addr = sQueuesPhysical + sizeof(hfi_queue_table_header)
			+ q * IFACEQ_QUEUE_SIZE;
		header.header_type = IFACEQ_DFLT_QHDR;
		header.queue_type = q;
		header.q_size = IFACEQ_QUEUE_SIZE / 4;
		header.rx_wm = 1;
		header.tx_wm = 1;
		header.rx_req = q == IFACEQ_DBGQ_ID ? 0 : 1;
	}
	sQueueTable->version = 0;
	sQueueTable->device_addr = sQueuesPhysical;
	strlcpy(sQueueTable->name, "iris-hfi-queues", sizeof(sQueueTable->name));
	sQueueTable->size = sizeof(hfi_queue_table_header);
	sQueueTable->qhdr0_offset = sizeof(hfi_queue_table_header)
		- IFACEQ_NUMQ * sizeof(hfi_queue_header);
	sQueueTable->qhdr_size = sizeof(hfi_queue_header);
	sQueueTable->num_q = IFACEQ_NUMQ;
	sQueueTable->num_active_q = IFACEQ_NUMQ;
	// the SFR's size in its first word, for the firmware
	*(uint32*)(sQueues + queueBytes) = SFR_SIZE;
	memory_full_barrier();
	INFO("queues at %#" B_PRIxPHYSADDR " (%#zx)\n", sQueuesPhysical,
		sQueuesSize);
	return B_OK;
}


static status_t
queue_write(uint32 queue, const void* packet, uint32 size)
{
	hfi_queue_header& header = queue_header(queue);
	uint8* memory = queue_memory(queue);
	uint32 writeIndex = header.write_idx * 4;
	uint32 readIndex = header.read_idx * 4;
	uint32 space = writeIndex < readIndex ? readIndex - writeIndex
		: IFACEQ_QUEUE_SIZE - (writeIndex - readIndex);
	if (space <= size || (size & 3) != 0)
		return B_NO_MEMORY;
	header.tx_req = 0;
	uint32 next = writeIndex + size;
	if (next < IFACEQ_QUEUE_SIZE)
		memcpy(memory + writeIndex, packet, size);
	else {
		uint32 residue = next - IFACEQ_QUEUE_SIZE;
		memcpy(memory + writeIndex, packet, size - residue);
		memcpy(memory, (const uint8*)packet + size - residue, residue);
		next = residue;
	}
	memory_full_barrier();
	header.write_idx = next / 4;
	memory_full_barrier();
	return B_OK;
}


/*!	One packet off the queue into \a packet (up to \a size bytes), its size
	in \a _size; B_WOULD_BLOCK when there is none.
*/
static status_t
queue_read(uint32 queue, void* packet, uint32 size, uint32* _size)
{
	hfi_queue_header& header = queue_header(queue);
	uint8* memory = queue_memory(queue);
	uint32 writeIndex = header.write_idx * 4;
	uint32 readIndex = header.read_idx * 4;
	uint32 request = queue == IFACEQ_MSGQ_ID ? 1 : 0;
	if (readIndex == writeIndex) {
		header.rx_req = request;
		memory_full_barrier();
		return B_WOULD_BLOCK;
	}
	uint32 packetSize = *(uint32*)(memory + readIndex);
	if (packetSize == 0 || readIndex >= IFACEQ_QUEUE_SIZE)
		return B_BAD_DATA;
	uint32 next = readIndex + packetSize;
	status_t status = B_OK;
	if (packetSize <= size) {
		if (next < IFACEQ_QUEUE_SIZE)
			memcpy(packet, memory + readIndex, packetSize);
		else {
			uint32 residue = next - IFACEQ_QUEUE_SIZE;
			memcpy(packet, memory + readIndex, packetSize - residue);
			memcpy((uint8*)packet + packetSize - residue, memory, residue);
			next = residue;
		}
	} else {
		next = writeIndex;
		status = B_BUFFER_OVERFLOW;
	}
	header.rx_req = request;
	header.read_idx = next / 4;
	memory_full_barrier();
	*_size = packetSize;
	return status;
}


static void
raise_interrupt()
{
	sIris.Write(CPU_IC_SOFTINT, 1);
}


static int32
iris_interrupt(void*)
{
	uint32 status = sIris.Read(WRAPPER_INTR_STATUS);
	if ((status & (kIntrA2h | kIntrA2hWatchdog)) == 0)
		return B_UNHANDLED_INTERRUPT;
	sIris.Write(CPU_CS_A2HSOFTINTCLR, 1);
	if ((status & kIntrA2hWatchdog) != 0)
		dprintf("qcom_iris: watchdog interrupt\n");
	atomic_add(&sInterrupts, 1);
	sMessageCondition.NotifyAll();
	return B_INVOKE_SCHEDULER;
}


//	#pragma mark - bring-up


static status_t
firmware_boot()
{
	// iris_vpu_setup_ucregion_memory_map()
	size_t queueBytes = ROUNDUP(sizeof(hfi_queue_table_header)
		+ IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ, B_PAGE_SIZE);
	sIris.Write(UC_REGION_ADDR, (uint32)sQueuesPhysical);
	sIris.Write(UC_REGION_SIZE, sQueuesSize);
	sIris.Write(QTBL_ADDR, (uint32)sQueuesPhysical);
	sIris.Write(QTBL_INFO, 1);
	sIris.Write(SFR_ADDR, (uint32)(sQueuesPhysical + queueBytes));

	sIris.Write(CTRL_INIT, 1);
	sIris.Write(CPU_CS_SCIACMDARG3, 1);
	uint32 status = 0;
	bigtime_t timeout = system_time() + 100000;
	while ((status = sIris.Read(CTRL_STATUS)) == 0) {
		if (system_time() > timeout) {
			ERROR("the firmware does not boot\n");
			return B_TIMED_OUT;
		}
		spin(50);
	}
	if ((status & kCtrlErrorMask) == 0x4) {
		ERROR("the firmware refuses the UC region\n");
		return B_ERROR;
	}
	sIris.Write(CPU_CS_H2XSOFTINTEN, 1);
	sIris.Write(CPU_CS_X2RPMH, 0);
	INFO("firmware booted, CTRL_STATUS %#" B_PRIx32 "\n", status);
	return B_OK;
}


static status_t
firmware_load()
{
	int node = fdt_node_offset_by_compatible(gFDT, -1, "qcom,sc8280xp-iris");
	int length;
	const fdt32_t* regions = node >= 0
		? (const fdt32_t*)fdt_getprop(gFDT, node, "memory-region", &length)
		: NULL;
	int memory = regions != NULL
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(regions[0])) : -1;
	const fdt32_t* reg = memory >= 0
		? (const fdt32_t*)fdt_getprop(gFDT, memory, "reg", &length) : NULL;
	if (reg == NULL || length < 16) {
		ERROR("no firmware memory in the device tree\n");
		return B_ENTRY_NOT_FOUND;
	}
	phys_addr_t regionBase = (phys_addr_t)fdt32_to_cpu(reg[0]) << 32
		| fdt32_to_cpu(reg[1]);
	size_t regionSize = (size_t)fdt32_to_cpu(reg[3]);

	size_t size;
	uint8* image = (uint8*)load_firmware(IRIS_FIRMWARE, &size);
	if (image == NULL)
		return B_ENTRY_NOT_FOUND;
	status_t status = qcom_pas_load("video firmware", IRIS_PAS_ID, image, size,
		regionBase, regionSize);
	free(image);
	if (status == B_OK)
		status = qcom_pas_auth_and_reset("video firmware", IRIS_PAS_ID);
	if (status != B_OK)
		return status;

	int64 result = qcom_scm_call(QCOM_SCM_SVC_MP, QCOM_SCM_MP_VIDEO_VAR,
		QCOM_SCM_ARGS(4), kCpStart, kCpSize, kCpNonpixelStart,
		kCpNonpixelSize);
	if (result != 0) {
		ERROR("video memory protection failed: %" B_PRId64 "\n", result);
		qcom_pas_shutdown(IRIS_PAS_ID);
		return B_ERROR;
	}
	INFO("firmware authenticated and started\n");
	return B_OK;
}


/*!	HFI's system init (iris_hfi_core_init()): SYS_INIT, the image version,
	codec power plane control; then the firmware's answers.
*/
static status_t
hfi_core_init()
{
	uint32 init[3] = { 12, HFI_CMD_SYS_INIT, HFI_VIDEO_ARCH_OX };
	uint32 version[4] = { 16, HFI_CMD_SYS_GET_PROPERTY, 1,
		HFI_PROPERTY_SYS_IMAGE_VERSION };
	uint32 power[5] = { 20, HFI_CMD_SYS_SET_PROPERTY, 1,
		HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL, 1 };
	if (queue_write(IFACEQ_CMDQ_ID, init, sizeof(init)) != B_OK
		|| queue_write(IFACEQ_CMDQ_ID, version, sizeof(version)) != B_OK
		|| queue_write(IFACEQ_CMDQ_ID, power, sizeof(power)) != B_OK) {
		return B_NO_MEMORY;
	}
	raise_interrupt();

	bool initDone = false, versionDone = false;
	bigtime_t deadline = system_time() + 1000000;
	while (!initDone || !versionDone) {
		static uint32 packet[IFACEQ_CORE_PKT_SIZE / 4];
		uint32 size;
		status_t status = queue_read(IFACEQ_MSGQ_ID, packet, sizeof(packet),
			&size);
		if (status == B_WOULD_BLOCK) {
			ConditionVariableEntry entry;
			sMessageCondition.Add(&entry);
			if (queue_read(IFACEQ_MSGQ_ID, packet, sizeof(packet), &size)
					== B_WOULD_BLOCK
				&& entry.Wait(B_ABSOLUTE_TIMEOUT, deadline) == B_TIMED_OUT) {
				ERROR("no answer to SYS_INIT (CTRL_STATUS %#" B_PRIx32 ", %"
					B_PRId32 " interrupts)\n", sIris.Read(CTRL_STATUS),
					sInterrupts);
				return B_TIMED_OUT;
			}
			continue;
		}
		if (status != B_OK) {
			ERROR("message queue: %s (packet of %" B_PRIu32 " bytes)\n",
				strerror(status), size);
			return status;
		}
		switch (packet[1]) {
			case HFI_MSG_SYS_INIT:
				INFO("SYS_INIT done: error %#" B_PRIx32 ", %" B_PRIu32
					" properties\n", packet[2], packet[3]);
				sSysInitProperties = packet[3];
				initDone = true;
				if (packet[2] != 0)
					return B_ERROR;
				break;
			case HFI_MSG_SYS_PROPERTY_INFO:
				if (size > 16 && packet[3] == HFI_PROPERTY_SYS_IMAGE_VERSION) {
					char text[128] = {};
					memcpy(text, (uint8*)packet + 16, min_c(size - 16,
						sizeof(text) - 1));
					for (size_t i = 0; i < sizeof(text) - 1; i++) {
						if (text[i] != 0 && (text[i] < ' ' || text[i] > '~'))
							text[i] = ' ';
					}
					INFO("firmware image: %s\n", text);
					strlcpy(sFirmwareVersion, text, sizeof(sFirmwareVersion));
				}
				versionDone = true;
				break;
			default:
				INFO("message %#" B_PRIx32 " (%" B_PRIu32 " bytes)\n",
					packet[1], size);
				break;
		}
	}
	return B_OK;
}


/*!	The apps SMMU's global fault state: a stream it has no match for shows
	up here, with the stream ID and the address it tried.
*/
static void
smmu_report(const char* when)
{
	register_window smmu;
	if (smmu.Map("qcom iris smmu", kAppsSmmuBase, 0x1000) != B_OK)
		return;
	// sGFSR 0x48, sGFSYNR0..2 0x50..0x58, sGFAR 0x40
	INFO("%s: apps SMMU sGFSR %#" B_PRIx32 " sGFSYNR %#" B_PRIx32 " %#" B_PRIx32
		" %#" B_PRIx32 " sGFAR %#" B_PRIx32 "%08" B_PRIx32 "\n", when,
		smmu.Read(0x48), smmu.Read(0x50), smmu.Read(0x54), smmu.Read(0x58),
		smmu.Read(0x44), smmu.Read(0x40));
	smmu.Unmap();
}


static status_t
bring_up()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,sc8280xp-iris") < 0)
		return B_DEVICE_NOT_FOUND;

	if (sGcc.Map("qcom iris gcc", kGccBase, 0x1f4200) != B_OK)
		return B_NO_MEMORY;
	status_t status = power_up();
	if (status != B_OK)
		return status;
	// translated, never bypassed or unmatched: either crashes the SoC once
	// the firmware runs
	(void)smmu_bypass;
	status = smmu_attach();
	if (status != B_OK)
		return status;

	if (sIris.Map("qcom iris", kIrisBase, kIrisSize) != B_OK)
		return B_NO_MEMORY;
	sIris.Write(WRAPPER_PRESET, 0);
	// interrupts from the firmware and its watchdog
	sIris.Clear(WRAPPER_INTR_MASK, kIntrA2h | kIntrA2hWatchdog);
	sMessageCondition.Init(&sMessageCondition, "qcom iris messages");
	status = install_io_interrupt_handler(kIrisInterrupt, iris_interrupt, NULL,
		0);
	if (status != B_OK)
		return status;
	sInterruptInstalled = true;

	status = queues_init();
	if (status == B_OK)
		status = firmware_load();
	context_report("after the PAS");
	if (status == B_OK)
		status = firmware_boot();
	context_report("after the boot");
	if (status == B_OK)
		status = hfi_core_init();
	context_report("after SYS_INIT");
	smmu_report("after SYS_INIT");
	return status;
}


static status_t
iris_start()
{
	MutexLocker locker(sLock);
	if (sStartStatus == B_NO_INIT) {
		sStartStatus = bring_up();
		if (sStartStatus != B_OK)
			ERROR("bring-up failed: %s\n", strerror(sStartStatus));
		else
			INFO("video codec running\n");
	}
	return sStartStatus;
}


//	#pragma mark - device


/*	The open that owns the codec, and the buffers it allocated. */

struct IrisBuffer {
	IrisBuffer*	next;		// by device address
	area_id		area;
	uint32		device;		// address
	size_t		size;
};

/*	Where the buffers are for the codec: from the end of what Linux keeps
	for content protection (0x25800000, iris_platform_sm8250.c) to the start
	of DRAM, below which nothing is identity mapped. */
static const uint32 kBufferWindow = 0x30000000;
static const uint32 kBufferWindowEnd = 0x80000000;

static void* sOwner;
static IrisBuffer* sBuffers;


static void
buffer_free(IrisBuffer* buffer)
{
	sSpace.Unmap(buffer->device, buffer->size);
	uint32 cb = smmu_context(sContextBank);
	sSmmu.Write(cb + 0x618, 0);				// TLBIALL
	sSmmu.Write(cb + 0x7f0, 0);				// TLBSYNC
	sSmmu.Wait(cb + 0x7f4, 1, 0, 100000);
	delete_area(buffer->area);
	free(buffer);
}


/*!	A buffer of pages anywhere, mapped for the codec at the first free
	place of kBufferWindow, and cloned into the caller's team.
*/
static status_t
buffer_alloc(qcom_iris_buffer* request)
{
	if (request->size == 0 || request->size > 768 * 1024 * 1024)
		return B_BAD_VALUE;
	size_t size = ROUNDUP(request->size, B_PAGE_SIZE);

	// the first gap in the window, sBuffers being sorted
	uint32 device = kBufferWindow;
	IrisBuffer** link = &sBuffers;
	while (*link != NULL) {
		if ((uint64)device + size <= (*link)->device)
			break;
		device = (*link)->device + ROUNDUP((*link)->size, 0x10000);
		link = &(*link)->next;
	}
	if ((uint64)device + size > kBufferWindowEnd)
		return B_NO_MEMORY;

	IrisBuffer* buffer = (IrisBuffer*)calloc(1, sizeof(IrisBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	void* address;
	buffer->area = create_area("qcom iris buffer", &address,
		B_ANY_KERNEL_ADDRESS, size, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (buffer->area < 0) {
		status_t status = buffer->area;
		free(buffer);
		return status;
	}
	memset(address, 0, size);
	buffer->device = device;
	buffer->size = size;

	status_t status = B_OK;
	for (size_t offset = 0; offset < size && status == B_OK;) {
		physical_entry entries[16];
		uint32 count = B_COUNT_OF(entries);
		status = get_memory_map_etc(B_SYSTEM_TEAM, (uint8*)address + offset,
			size - offset, entries, &count);
		// more runs than entries: the next call goes on from these
		if (status == B_BUFFER_OVERFLOW)
			status = B_OK;
		for (uint32 i = 0; status == B_OK && i < count; i++) {
			status = sSpace.Map(device + offset, entries[i].address,
				entries[i].size, true);
			offset += entries[i].size;
		}
		if (status == B_OK && count == 0)
			status = B_ERROR;
	}
	if (status != B_OK) {
		sSpace.Unmap(device, size);
		delete_area(buffer->area);
		free(buffer);
		return status;
	}

	void* userAddress = NULL;
	area_id clone = vm_clone_area(team_get_current_team_id(),
		"qcom iris buffer", &userAddress, B_RANDOMIZED_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, REGION_NO_PRIVATE_MAP, buffer->area, true);
	if (clone < 0) {
		buffer_free(buffer);
		return clone;
	}
	buffer->next = *link;
	*link = buffer;
	request->area = clone;
	request->address = userAddress;
	request->device_address = device;
	return B_OK;
}


static status_t
iris_open(const char* name, uint32 flags, void** _cookie)
{
	status_t status = iris_start();
	if (status != B_OK)
		return status;
	MutexLocker locker(sLock);
	if (sOwner != NULL)
		return B_BUSY;
	void* cookie = malloc(1);
	if (cookie == NULL)
		return B_NO_MEMORY;
	sOwner = cookie;
	*_cookie = cookie;
	return B_OK;
}


static status_t
iris_close(void* cookie)
{
	return B_OK;
}


static status_t
iris_free(void* cookie)
{
	MutexLocker locker(sLock);
	while (sBuffers != NULL) {
		IrisBuffer* buffer = sBuffers;
		sBuffers = buffer->next;
		buffer_free(buffer);
	}
	if (sOwner == cookie)
		sOwner = NULL;
	free(cookie);
	return B_OK;
}


static status_t
iris_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
iris_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
iris_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case QCOM_IRIS_GET_INFO:
		{
			qcom_iris_info info = {};
			info.version = QCOM_IRIS_VERSION;
			info.sys_init_properties = sSysInitProperties;
			strlcpy(info.firmware, sFirmwareVersion, sizeof(info.firmware));
			return user_memcpy(buffer, &info, min_c(length, sizeof(info)));
		}

		case QCOM_IRIS_SEND:
		{
			qcom_iris_packet request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_VALUE;
			}
			if (request.size < 8 || request.size > IFACEQ_CORE_PKT_SIZE
				|| (request.size & 3) != 0) {
				return B_BAD_VALUE;
			}
			uint32 packet[IFACEQ_CORE_PKT_SIZE / 4];
			if (user_memcpy(packet, request.data, request.size) != B_OK)
				return B_BAD_ADDRESS;
			if (packet[0] != request.size)
				return B_BAD_VALUE;
			MutexLocker locker(sLock);
			status_t status = queue_write(IFACEQ_CMDQ_ID, packet, request.size);
			if (status == B_OK)
				raise_interrupt();
			return status;
		}

		case QCOM_IRIS_RECEIVE:
		{
			qcom_iris_receive request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_VALUE;
			}
			static uint32 packet[IFACEQ_CORE_PKT_SIZE / 4];
			bigtime_t deadline = system_time() + request.timeout;
			MutexLocker locker(sLock);
			while (true) {
				uint32 size = 0;
				status_t status = queue_read(IFACEQ_MSGQ_ID, packet,
					sizeof(packet), &size);
				if (status == B_OK) {
					request.received = min_c(size, request.size);
					if (user_memcpy(request.buffer, packet, request.received)
							!= B_OK
						|| user_memcpy(buffer, &request, sizeof(request))
							!= B_OK) {
						return B_BAD_ADDRESS;
					}
					return size > request.size ? B_BUFFER_OVERFLOW : B_OK;
				}
				if (status != B_WOULD_BLOCK)
					return status;
				if (request.timeout <= 0 || system_time() >= deadline)
					return B_TIMED_OUT;
				ConditionVariableEntry entry;
				sMessageCondition.Add(&entry);
				locker.Unlock();
				status = entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT,
					deadline);
				locker.Lock();
				if (status == B_INTERRUPTED)
					return B_INTERRUPTED;
			}
		}

		case QCOM_IRIS_ALLOC:
		{
			qcom_iris_buffer request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(sLock);
			status_t status = buffer_alloc(&request);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case QCOM_IRIS_FREE:
		{
			uint32 deviceAddress;
			if (length != sizeof(deviceAddress)
				|| user_memcpy(&deviceAddress, buffer, sizeof(deviceAddress))
					!= B_OK) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(sLock);
			for (IrisBuffer** link = &sBuffers; *link != NULL;
					link = &(*link)->next) {
				if ((*link)->device != deviceAddress)
					continue;
				IrisBuffer* found = *link;
				*link = found->next;
				buffer_free(found);
				return B_OK;
			}
			return B_ENTRY_NOT_FOUND;
		}
	}
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sDeviceHooks = {
	iris_open,
	iris_close,
	iris_free,
	iris_control,
	iris_read,
	iris_write,
};


status_t
init_hardware()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,sc8280xp-iris") < 0)
		return B_DEVICE_NOT_FOUND;
	return B_OK;
}


status_t
init_driver()
{
	return B_OK;
}


void
uninit_driver()
{
	// unloaded (a new driver file): the firmware stops, so that the next
	// load can start it again; the memory it used stays (the codec's
	// address space may still name it)
	if (sStartStatus == B_OK) {
		qcom_pas_shutdown(IRIS_PAS_ID);
		INFO("firmware stopped\n");
	}
	if (sInterruptInstalled) {
		remove_io_interrupt_handler(kIrisInterrupt, iris_interrupt, NULL);
		sInterruptInstalled = false;
	}
	sStartStatus = B_NO_INIT;
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
