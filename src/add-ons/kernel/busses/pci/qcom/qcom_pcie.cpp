/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Host driver for the Qualcomm PCIe controllers (Synopsys DesignWare core
// with Qualcomm "PARF" glue), as found on the SC8280XP.
//
// The UEFI firmware trains the links of the slots it boots from; this driver
// takes the controller as the firmware leaves it (clocks, PHY, link) and only
// reprograms the outbound iATU: two configuration regions in the core's ECAM
// "shift" mode, then one region per memory/IO window of the device tree's
// "ranges". MSIs go to the GIC ITS named by "msi-map", INTx to the GIC SPIs
// of "interrupt-map".


#include <bus/FDT.h>
#include <bus/PCI.h>
#include <bus/PCIInterrupts.h>
#include <arch/generic/msi.h>
#include <KernelExport.h>
#include <AutoDeleterDrivers.h>
#include <AutoDeleterOS.h>
#include <new>
#include <string.h>

extern "C" {
#include <libfdt.h>
}

extern void* gFDT;


#define DRIVER_NAME "busses/pci/qcom_pcie/driver_v1"
#define INTX_MODULE_NAME "busses/pci/qcom_pcie/intx_v1"

//#define TRACE_QCOM_PCIE
#ifdef TRACE_QCOM_PCIE
#	define TRACE(x...) dprintf("qcom_pcie: " x)
#else
#	define TRACE(x...)
#endif
#define INFO(x...) dprintf("qcom_pcie: " x)


// DesignWare unrolled iATU (the SC8280XP has it in its own "atu" region)
static const uint32 kAtuRegionStride = 0x200;
static const uint32 kAtuCtrl1 = 0x00;
static const uint32 kAtuCtrl2 = 0x04;
static const uint32 kAtuLowerBase = 0x08;
static const uint32 kAtuUpperBase = 0x0c;
static const uint32 kAtuLowerLimit = 0x10;
static const uint32 kAtuLowerTarget = 0x14;
static const uint32 kAtuUpperTarget = 0x18;
static const uint32 kAtuUpperLimit = 0x20;

static const uint32 kAtuTypeMem = 0x0;
static const uint32 kAtuTypeIo = 0x2;
static const uint32 kAtuTypeCfg0 = 0x4;
static const uint32 kAtuTypeCfg1 = 0x5;
static const uint32 kAtuIncreaseRegionSize = 1 << 13;
static const uint32 kAtuEnable = 1u << 31;
static const uint32 kAtuCfgShiftMode = 1 << 28;

static const uint32 kAtuConfigRegions = 2;
static const uint32 kMaxAtuRegions = 16;

// PCIe capability of the root port, in DBI space
static const uint8 kPciCapabilityExpress = 0x10;
static const uint16 kLinkStatus = 0x12;
static const uint16 kLinkStatusActive = 1 << 13;

static const uint32 kMaxRanges = 4;

// PARF: where the firmware put the DBI and iATU. The Windows-oriented UEFI
// moves them into the ECAM window that its ACPI MCFG describes.
static const uint32 kParfDbiBase = 0x350;
static const uint32 kParfDbiBaseHigh = 0x354;
static const uint32 kParfSlaveSize = 0x358;
static const uint32 kParfSlaveSizeHigh = 0x35c;
static const uint32 kParfAtuBase = 0x634;
static const uint32 kParfAtuBaseHigh = 0x638;

// SC8280XP global clock controller: one register block per controller, with
// the power domain (GDSC) and the branch clocks the DBI and AXI need.
struct GccBlock {
	uint64 parf;
	uint32 block;
};
static const GccBlock kSc8280xpGcc[] = {
	{ 0x1c00000, 0x6b000 },	// pcie4
	{ 0x1c08000, 0xa2000 },	// pcie3b
	{ 0x1c10000, 0xa0000 },	// pcie3a
	{ 0x1c18000, 0x9e000 },	// pcie2b
	{ 0x1c20000, 0x9d000 },	// pcie2a
};
static const uint64 kSc8280xpGccBase = 0x100000;
static const uint32 kGdscr = 0x004;
static const uint32 kGdscPowerOn = 1u << 31;
static const uint32 kSlaveAxiCbcr = 0x014;
static const uint32 kMasterAxiCbcr = 0x01c;
static const uint32 kConfigAhbCbcr = 0x024;
static const uint32 kCbcrClockOff = 1u << 31;
static const uint32 kMaxIntxPins = 4;


static device_manager_info* sDeviceManager;


struct Controller {
	AreaDeleter dbiArea;
	AreaDeleter atuArea;
	AreaDeleter configArea;
	volatile uint8* dbi = NULL;
	volatile uint8* atu = NULL;
	volatile uint8* config = NULL;
	uint64 configPhys = 0;
	uint64 configSize = 0;
	uint32 atuRegions = 0;
	bool firmwareEcam = false;
	uint32 domain = 0;
	uint8 lastBus = 0xff;
	bool linkUp = false;

	pci_resource_range ranges[kMaxRanges];
	uint32 rangeCount = 0;

	uint32 intxIrq[kMaxIntxPins] = {};
};


static inline uint32
Read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
Write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


static bool
HasString(const char* data, int length, const char* value)
{
	while (data != NULL && length > 0) {
		const char* end = (const char*)memchr(data, 0, length);
		if (end == NULL)
			return false;
		if (strcmp(data, value) == 0)
			return true;
		length -= end - data + 1;
		data = end + 1;
	}
	return false;
}


static int
StringIndex(fdt_device_module_info* fdt, fdt_device* device, const char* property,
	const char* value)
{
	int length;
	const char* data = (const char*)fdt->get_prop(device, property, &length);
	for (int index = 0; data != NULL && length > 0; index++) {
		const char* end = (const char*)memchr(data, 0, length);
		if (end == NULL)
			return -1;
		if (strcmp(data, value) == 0)
			return index;
		length -= end - data + 1;
		data = end + 1;
	}
	return -1;
}


static bool
GetNamedReg(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint64* base, uint64* size)
{
	int index = StringIndex(fdt, device, "reg-names", name);
	return index >= 0 && fdt->get_reg(device, index, base, size);
}


static bool
GetFdt(device_node* node, fdt_device_module_info** fdt, fdt_device** device)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return false;
	}
	return sDeviceManager->get_driver(node, (driver_module_info**)fdt,
		(void**)device) == B_OK;
}


static bool
IsQcomPcie(device_node* node)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!GetFdt(node, &fdt, &device))
		return false;

	int length;
	const char* compatible = (const char*)fdt->get_prop(device, "compatible", &length);
	return HasString(compatible, length, "qcom,pcie-sc8280xp");
}


static uint32
GetCell(const void* data, uint32 index)
{
	return B_BENDIAN_TO_HOST_INT32(((const uint32*)data)[index]);
}


//	#pragma mark - iATU


static void
AtuProgram(Controller* controller, uint32 index, uint32 type, uint64 cpuAddress,
	uint64 pciAddress, uint64 size, uint32 ctrl2)
{
	volatile uint8* region = controller->atu + index * kAtuRegionStride;
	uint64 limit = cpuAddress + size - 1;

	Write32(region, kAtuLowerBase, (uint32)cpuAddress);
	Write32(region, kAtuUpperBase, (uint32)(cpuAddress >> 32));
	Write32(region, kAtuLowerLimit, (uint32)limit);
	Write32(region, kAtuUpperLimit, (uint32)(limit >> 32));
	Write32(region, kAtuLowerTarget, (uint32)pciAddress);
	Write32(region, kAtuUpperTarget, (uint32)(pciAddress >> 32));
	uint32 ctrl1 = type;
	if ((limit >> 32) != (cpuAddress >> 32))
		ctrl1 |= kAtuIncreaseRegionSize;
	Write32(region, kAtuCtrl1, ctrl1);
	Write32(region, kAtuCtrl2, kAtuEnable | ctrl2);

	// The core takes a few cycles to latch the region.
	for (int i = 0; i < 100; i++) {
		if ((Read32(region, kAtuCtrl2) & kAtuEnable) != 0)
			break;
		spin(10);
	}
}


static void
AtuDump(Controller* controller, const char* when)
{
	for (uint32 i = 0; i < controller->atuRegions; i++) {
		volatile uint8* region = controller->atu + i * kAtuRegionStride;
		uint32 ctrl2 = Read32(region, kAtuCtrl2);
		if ((ctrl2 & kAtuEnable) == 0)
			continue;
		INFO("%u %s iATU %" B_PRIu32 ": type %#" B_PRIx32 " ctrl2 %#" B_PRIx32
			" %#" B_PRIx32 "%08" B_PRIx32 "..%#" B_PRIx32 "%08" B_PRIx32
			" -> %#" B_PRIx32 "%08" B_PRIx32 "\n", controller->domain, when, i,
			Read32(region, kAtuCtrl1), ctrl2,
			Read32(region, kAtuUpperBase), Read32(region, kAtuLowerBase),
			Read32(region, kAtuUpperLimit), Read32(region, kAtuLowerLimit),
			Read32(region, kAtuUpperTarget), Read32(region, kAtuLowerTarget));
	}
}


static void
AtuSetup(Controller* controller)
{
	// Bus 1 (below the root port) with type 0 accesses, buses 2.. with type 1,
	// each bus 1 MiB into the "config" window: ECAM layout from bus 1 on.
	uint64 busCount = controller->configSize >> 20;
	if (busCount > (uint64)controller->lastBus + 1)
		busCount = (uint64)controller->lastBus + 1;
	AtuProgram(controller, 0, kAtuTypeCfg0, controller->configPhys + (1 << 20),
		0, 1 << 20, kAtuCfgShiftMode);
	if (busCount > 2) {
		AtuProgram(controller, 1, kAtuTypeCfg1, controller->configPhys + (2 << 20),
			0, (busCount - 2) << 20, kAtuCfgShiftMode);
	}

	uint32 index = kAtuConfigRegions;
	for (uint32 i = 0; i < controller->rangeCount; i++) {
		const pci_resource_range& range = controller->ranges[i];
		AtuProgram(controller, index++,
			range.type == B_IO_PORT ? kAtuTypeIo : kAtuTypeMem,
			range.host_address, range.pci_address, range.size, 0);
	}

	// Disable whatever the firmware left beyond our regions.
	for (; index < controller->atuRegions; index++) {
		volatile uint8* region = controller->atu + index * kAtuRegionStride;
		if ((Read32(region, kAtuCtrl2) & kAtuEnable) != 0)
			Write32(region, kAtuCtrl2, 0);
	}
}


//	#pragma mark - device tree


static void
ReadRanges(Controller* controller, fdt_device_module_info* fdt, fdt_device* device)
{
	int length;
	const void* prop = fdt->get_prop(device, "ranges", &length);
	if (prop == NULL)
		return;

	// <pci-flags pci-hi pci-lo cpu-hi cpu-lo size-hi size-lo>
	for (int offset = 0; offset + 28 <= length && controller->rangeCount < kMaxRanges;
			offset += 28) {
		const void* entry = (const uint8*)prop + offset;
		uint32 flags = GetCell(entry, 0);
		uint64 pciAddress = ((uint64)GetCell(entry, 1) << 32) | GetCell(entry, 2);
		uint64 cpuAddress = ((uint64)GetCell(entry, 3) << 32) | GetCell(entry, 4);
		uint64 size = ((uint64)GetCell(entry, 5) << 32) | GetCell(entry, 6);

		pci_resource_range range = {};
		range.host_address = cpuAddress;
		range.pci_address = pciAddress;
		range.size = size;
		switch ((flags >> 24) & 3) {
			case 1:
				range.type = B_IO_PORT;
				break;
			case 2:
				range.type = B_IO_MEMORY;
				range.address_type = PCI_address_type_32;
				break;
			case 3:
				range.type = B_IO_MEMORY;
				range.address_type = PCI_address_type_64;
				break;
			default:
				continue;
		}
		if ((flags & (1 << 30)) != 0)
			range.address_type |= PCI_address_prefetchable;

		controller->ranges[controller->rangeCount++] = range;
	}
}


static void
ReadIntxMap(Controller* controller, fdt_device_module_info* fdt, fdt_device* device)
{
	// <child-addr(3) pin phandle parent-addr(#address-cells) specifier(#interrupt-cells)>
	// The SC8280XP maps the four pins straight to GIC SPIs.
	int length;
	const void* map = fdt->get_prop(device, "interrupt-map", &length);
	if (map == NULL)
		return;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(fdt->get_bus(device),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return;
	}

	uint32 cells = length / 4;
	for (uint32 i = 0; i + 5 <= cells; ) {
		uint32 pin = GetCell(map, i + 3);
		uint32 phandle = GetCell(map, i + 4);
		device_node* parent = busModule->node_by_phandle(bus, phandle);
		fdt_device_module_info* parentModule;
		fdt_device* parentDevice;
		if (parent == NULL || !GetFdt(parent, &parentModule, &parentDevice))
			return;

		int propLength;
		const void* prop = parentModule->get_prop(parentDevice, "#address-cells",
			&propLength);
		uint32 addressCells = prop != NULL && propLength == 4 ? GetCell(prop, 0) : 0;
		prop = parentModule->get_prop(parentDevice, "#interrupt-cells", &propLength);
		uint32 interruptCells = prop != NULL && propLength == 4 ? GetCell(prop, 0) : 0;
		if (interruptCells != 3 || i + 5 + addressCells + interruptCells > cells)
			return;

		uint32 specifier = i + 5 + addressCells;
		if (GetCell(map, specifier) == 0 && pin >= 1 && pin <= kMaxIntxPins)
			controller->intxIrq[pin - 1] = GetCell(map, specifier + 1) + 32;

		i = specifier + interruptCells;
	}
}


static uint8
FindExpressCapability(Controller* controller)
{
	uint8 offset = Read32(controller->dbi, PCI_capabilities_ptr) & 0xfc;
	for (int i = 0; offset != 0 && i < 48; i++) {
		uint32 header = Read32(controller->dbi, offset);
		if ((header & 0xff) == kPciCapabilityExpress)
			return offset;
		offset = (header >> 8) & 0xfc;
	}
	return 0;
}


// Take over the configuration and memory windows the firmware programmed:
// two configuration regions in ECAM shift mode (bus 1 type 0, buses 2.. type
// 1) and memory/IO regions. Returns false if the firmware did not set up ECAM.
static bool
AdoptFirmwareWindows(Controller* controller)
{
	uint64 ecamBase = 0;
	uint64 ecamEnd = 0;
	for (uint32 i = 0; i < controller->atuRegions; i++) {
		volatile uint8* region = controller->atu + i * kAtuRegionStride;
		uint32 ctrl2 = Read32(region, kAtuCtrl2);
		if ((ctrl2 & kAtuEnable) == 0)
			continue;
		uint32 type = Read32(region, kAtuCtrl1) & 0x1f;
		uint64 base = ((uint64)Read32(region, kAtuUpperBase) << 32)
			| Read32(region, kAtuLowerBase);
		uint64 limit = ((uint64)Read32(region, kAtuUpperLimit) << 32)
			| Read32(region, kAtuLowerLimit);
		uint64 target = ((uint64)Read32(region, kAtuUpperTarget) << 32)
			| Read32(region, kAtuLowerTarget);
		if (limit < base)
			continue;

		if (type == kAtuTypeCfg0 && (ctrl2 & kAtuCfgShiftMode) != 0) {
			ecamBase = base - (1 << 20);
			if (limit + 1 > ecamEnd)
				ecamEnd = limit + 1;
		} else if (type == kAtuTypeCfg1 && (ctrl2 & kAtuCfgShiftMode) != 0) {
			if (limit + 1 > ecamEnd)
				ecamEnd = limit + 1;
		} else if ((type == kAtuTypeMem || type == kAtuTypeIo)
			&& controller->rangeCount < kMaxRanges) {
			pci_resource_range range = {};
			range.host_address = base;
			range.pci_address = target;
			range.size = limit - base + 1;
			if (type == kAtuTypeIo) {
				range.type = B_IO_PORT;
			} else {
				range.type = B_IO_MEMORY;
				range.address_type = target + range.size <= (1ull << 32)
					? PCI_address_type_32 : PCI_address_type_64;
			}
			controller->ranges[controller->rangeCount++] = range;
		}
	}

	if (ecamBase == 0 || ecamEnd <= ecamBase)
		return false;
	controller->configPhys = ecamBase;
	controller->configSize = ecamEnd - ecamBase;
	uint64 busCount = controller->configSize >> 20;
	if (busCount - 1 < controller->lastBus)
		controller->lastBus = busCount - 1;
	return true;
}


// Reading the DBI of a controller without power or clocks is a bus error that
// takes the whole SoC down, so look at the clock controller first.
static bool
ControllerRunning(fdt_device_module_info* fdt, fdt_device* device, uint32 domain)
{
	uint64 parf, parfSize;
	if (!GetNamedReg(fdt, device, "parf", &parf, &parfSize))
		return false;

	uint32 block = 0;
	for (size_t i = 0; i < B_COUNT_OF(kSc8280xpGcc); i++) {
		if (kSc8280xpGcc[i].parf == parf)
			block = kSc8280xpGcc[i].block;
	}
	if (block == 0) {
		INFO("%" B_PRIu32 ": unknown controller at %#" B_PRIx64 "\n", domain, parf);
		return false;
	}

	volatile uint8* gcc;
	AreaDeleter area(map_physical_memory("qcom pcie gcc", kSc8280xpGccBase + block,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
		(void**)&gcc));
	if (area.Get() < B_OK)
		return false;

	uint32 gdsc = Read32(gcc, kGdscr);
	uint32 ahb = Read32(gcc, kConfigAhbCbcr);
	uint32 master = Read32(gcc, kMasterAxiCbcr);
	uint32 slave = Read32(gcc, kSlaveAxiCbcr);
	bool running = (gdsc & kGdscPowerOn) != 0 && (ahb & kCbcrClockOff) == 0
		&& (master & kCbcrClockOff) == 0 && (slave & kCbcrClockOff) == 0;
	INFO("%" B_PRIu32 ": gdsc %#" B_PRIx32 " cfg_ahb %#" B_PRIx32 " mstr_axi %#"
		B_PRIx32 " slv_axi %#" B_PRIx32 ": %s\n", domain, gdsc, ahb, master, slave,
		running ? "running" : "left off by the firmware");
	return running;
}


//	#pragma mark - driver


static float
SupportsDevice(device_node* parent)
{
	return IsQcomPcie(parent) ? 1.0f : 0.0f;
}


static status_t
RegisterDevice(device_node* parent)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!IsQcomPcie(parent) || !GetFdt(parent, &fdt, &device))
		return B_NOT_SUPPORTED;

	device_attr attrs[7] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Qualcomm PCIe host"} },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"} },
		{ B_PCI_INTX_CONTROLLER_MODULE, B_STRING_TYPE, {.string = INTX_MODULE_NAME} },
	};

	// msi-map = <rid-base &its its-base length>: the ITS device ID of a
	// function is its-base + RID. The ITS sits below the GIC, which comes
	// after the PCIe controllers in the device tree, so its device node may
	// not exist yet: read its registers from the tree itself.
	int length;
	const void* map = fdt->get_prop(device, "msi-map", &length);
	if (map != NULL && length == 16 && GetCell(map, 0) == 0 && gFDT != NULL
		&& fdt->get_prop(device, "msi-map-mask", NULL) == NULL) {
		int its = fdt_node_offset_by_phandle(gFDT, GetCell(map, 1));
		int regLength;
		const uint32* reg = its >= 0
			? (const uint32*)fdt_getprop(gFDT, its, "reg", &regLength) : NULL;
		int gic = its >= 0 ? fdt_parent_offset(gFDT, its) : -1;
		const uint32* cells = gic >= 0
			? (const uint32*)fdt_getprop(gFDT, gic, "#address-cells", NULL) : NULL;
		uint32 addressCells = cells != NULL ? fdt32_to_cpu(*cells) : 2;
		if (reg != NULL && fdt_node_check_compatible(gFDT, its, "arm,gic-v3-its") == 0
			&& (addressCells == 1 || addressCells == 2)
			&& regLength >= (int)(addressCells * 4)) {
			uint64 itsBase = addressCells == 2
				? ((uint64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1])
				: fdt32_to_cpu(reg[0]);
			attrs[3] = { B_PCI_MSI_CONTROLLER_ADDRESS, B_UINT64_TYPE,
				{.ui64 = itsBase} };
			attrs[4] = { B_PCI_MSI_REQUESTER_BASE, B_UINT32_TYPE,
				{.ui32 = GetCell(map, 2)} };
			attrs[5] = { B_PCI_MSI_REQUESTER_COUNT, B_UINT32_TYPE,
				{.ui32 = GetCell(map, 3)} };
		}
	}

	return sDeviceManager->register_node(parent, DRIVER_NAME, attrs, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	DeviceNodePutter<&sDeviceManager> parent(sDeviceManager->get_parent_node(node));
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!GetFdt(parent.Get(), &fdt, &device))
		return B_NOT_SUPPORTED;

	ObjectDeleter<Controller> controller(new(std::nothrow) Controller);
	if (!controller.IsSet())
		return B_NO_MEMORY;

	uint64 dbiBase, dbiSize, atuBase, atuSize;
	if (!GetNamedReg(fdt, device, "dbi", &dbiBase, &dbiSize)
		|| !GetNamedReg(fdt, device, "atu", &atuBase, &atuSize)
		|| !GetNamedReg(fdt, device, "config", &controller->configPhys,
			&controller->configSize)) {
		INFO("missing dbi/atu/config registers\n");
		return B_BAD_DATA;
	}

	int length;
	const void* prop = fdt->get_prop(device, "linux,pci-domain", &length);
	if (prop != NULL && length == 4)
		controller->domain = GetCell(prop, 0);
	prop = fdt->get_prop(device, "bus-range", &length);
	if (prop != NULL && length == 8)
		controller->lastBus = GetCell(prop, 1);

	if (!ControllerRunning(fdt, device, controller->domain))
		return B_DEV_NOT_READY;

	uint64 parfBase, parfSize;
	if (!GetNamedReg(fdt, device, "parf", &parfBase, &parfSize))
		return B_BAD_DATA;
	volatile uint8* parf;
	AreaDeleter parfArea(map_physical_memory("qcom pcie parf", parfBase,
		ROUNDUP(parfSize, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA, (void**)&parf));
	if (parfArea.Get() < B_OK)
		return parfArea.Get();
	uint64 firmwareDbi = ((uint64)Read32(parf, kParfDbiBaseHigh) << 32)
		| Read32(parf, kParfDbiBase);
	uint64 firmwareAtu = ((uint64)Read32(parf, kParfAtuBaseHigh) << 32)
		| Read32(parf, kParfAtuBase);
	uint64 slaveSize = ((uint64)Read32(parf, kParfSlaveSizeHigh) << 32)
		| Read32(parf, kParfSlaveSize);
	INFO("%" B_PRIu32 ": parf dbi %#" B_PRIx64 " atu %#" B_PRIx64 " slave size %#"
		B_PRIx64 " (device tree: dbi %#" B_PRIx64 " atu %#" B_PRIx64 ")\n",
		controller->domain, firmwareDbi, firmwareAtu, slaveSize, dbiBase, atuBase);
	if (firmwareDbi != 0 && firmwareDbi != dbiBase) {
		dbiBase = firmwareDbi;
		dbiSize = 0x1000;
	}
	if (firmwareAtu != 0 && firmwareAtu != atuBase) {
		atuBase = firmwareAtu;
		atuSize = 0x1000;
	}

	controller->dbiArea.SetTo(map_physical_memory("qcom pcie dbi", dbiBase,
		ROUNDUP(dbiSize, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->dbi));
	if (controller->dbiArea.Get() < B_OK)
		return controller->dbiArea.Get();
	controller->atuArea.SetTo(map_physical_memory("qcom pcie atu", atuBase,
		ROUNDUP(atuSize, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->atu));
	if (controller->atuArea.Get() < B_OK)
		return controller->atuArea.Get();
	controller->atuRegions = ROUNDUP(atuSize, B_PAGE_SIZE) / kAtuRegionStride;
	if (controller->atuRegions > kMaxAtuRegions)
		controller->atuRegions = kMaxAtuRegions;

	uint32 id = Read32(controller->dbi, PCI_vendor_id);
	uint8 express = id != 0xffffffff ? FindExpressCapability(controller.Get()) : 0;
	uint16 linkStatus = express != 0
		? *(volatile uint16*)(controller->dbi + express + kLinkStatus) : 0;
	controller->linkUp = (linkStatus & kLinkStatusActive) != 0;
	INFO("%" B_PRIu32 ": root port %04" B_PRIx32 ":%04" B_PRIx32
		", link status %#x (%s, gen %u x%u)\n", controller->domain,
		id & 0xffff, id >> 16, linkStatus, controller->linkUp ? "up" : "down",
		linkStatus & 0xf, (linkStatus >> 4) & 0x3f);
	if (id == 0xffffffff)
		return B_DEV_NOT_READY;
	AtuDump(controller.Get(), "firmware");

	ReadIntxMap(controller.Get(), fdt, device);
	controller->firmwareEcam = AdoptFirmwareWindows(controller.Get());
	if (!controller->firmwareEcam) {
		controller->rangeCount = 0;
		ReadRanges(controller.Get(), fdt, device);
	}
	for (uint32 i = 0; i < controller->rangeCount; i++) {
		INFO("%" B_PRIu32 ": %s window %#" B_PRIx64 " -> %#" B_PRIx64 " size %#"
			B_PRIx64 "\n", controller->domain,
			controller->ranges[i].type == B_IO_PORT ? "io" : "memory",
			controller->ranges[i].host_address, controller->ranges[i].pci_address,
			controller->ranges[i].size);
	}

	if (!controller->linkUp) {
		// Nothing behind the port: keep the root port visible, but don't
		// touch the configuration window (accesses would fault).
		controller->lastBus = 0;
	} else {
		controller->configArea.SetTo(map_physical_memory("qcom pcie config",
			controller->configPhys, controller->configSize,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&controller->config));
		if (controller->configArea.Get() < B_OK)
			return controller->configArea.Get();
	}
	INFO("%" B_PRIu32 ": configuration %#" B_PRIx64 " size %#" B_PRIx64
		" (%s), buses 0..%u\n", controller->domain, controller->configPhys,
		controller->configSize, controller->firmwareEcam ? "firmware ECAM"
			: "device tree", controller->lastBus);

	if (!controller->firmwareEcam) {
		AtuSetup(controller.Get());
		AtuDump(controller.Get(), "driver");
	}
	TRACE("%" B_PRIu32 ": INTx %u %u %u %u\n", controller->domain,
		controller->intxIrq[0], controller->intxIrq[1], controller->intxIrq[2],
		controller->intxIrq[3]);

	*cookie = controller.Detach();
	return B_OK;
}


static void
UninitDriver(void* cookie)
{
	delete (Controller*)cookie;
}


static volatile uint8*
ConfigAddress(Controller* controller, uint8 bus, uint8 device, uint8 function,
	uint16 offset)
{
	if (bus == 0)
		return device == 0 && function == 0 ? controller->dbi + offset : NULL;
	if (controller->config == NULL || bus > controller->lastBus)
		return NULL;
	// A type 0 access is claimed by the single device below the root port
	// whatever its device number.
	if (bus == 1 && device != 0)
		return NULL;
	return controller->config + ((uint32)bus << 20) + ((uint32)device << 15)
		+ ((uint32)function << 12) + offset;
}


static status_t
ReadConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32* value)
{
	volatile uint8* address = ConfigAddress((Controller*)cookie, bus, device,
		function, offset);
	if (address == NULL || offset >= 4096 || (offset & (size - 1)) != 0) {
		*value = 0xffffffff;
		return address == NULL ? B_OK : B_BAD_VALUE;
	}
	switch (size) {
		case 1: *value = *(volatile uint8*)address; break;
		case 2: *value = *(volatile uint16*)address; break;
		case 4: *value = *(volatile uint32*)address; break;
		default: return B_BAD_VALUE;
	}
	return B_OK;
}


static status_t
WriteConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32 value)
{
	volatile uint8* address = ConfigAddress((Controller*)cookie, bus, device,
		function, offset);
	if (address == NULL)
		return B_OK;
	if (offset >= 4096 || (offset & (size - 1)) != 0)
		return B_BAD_VALUE;
	switch (size) {
		case 1: *(volatile uint8*)address = value; break;
		case 2: *(volatile uint16*)address = value; break;
		case 4: *(volatile uint32*)address = value; break;
		default: return B_BAD_VALUE;
	}
	return B_OK;
}


static pci_controller_module_info sController = {
	.info = {
		.info = {
			.name = DRIVER_NAME,
		},
		.supports_device = SupportsDevice,
		.register_device = RegisterDevice,
		.init_driver = InitDriver,
		.uninit_driver = UninitDriver,
	},
	.read_pci_config = ReadConfig,
	.write_pci_config = WriteConfig,
	.get_max_bus_devices = [](void*, int32* count) {
		*count = 32;
		return B_OK;
	},
	.read_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8*) {
		return B_NOT_SUPPORTED;
	},
	.write_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8) {
		return B_NOT_SUPPORTED;
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		Controller* controller = (Controller*)cookie;
		if (index >= controller->rangeCount)
			return B_BAD_INDEX;
		*range = controller->ranges[index];
		return B_OK;
	},
	.finalize = [](void*) { return B_OK; }
};


static status_t
GetIntxIrq(void* cookie, uint8 bus, uint8 device, uint8 function, uint8 pin,
	uint32* irq)
{
	Controller* controller = (Controller*)cookie;
	if (irq == NULL || pin < 1 || pin > kMaxIntxPins)
		return B_BAD_VALUE;
	// Swizzle through the bridges as far as the root port only sees the slot:
	// the device tree maps the root's four pins.
	*irq = controller->intxIrq[(pin - 1 + device) % kMaxIntxPins];
	return *irq != 0 ? B_OK : B_NOT_SUPPORTED;
}


static pci_intx_controller_module_info sIntxController = {
	.info = { .name = INTX_MODULE_NAME },
	.get_irq = GetIntxIrq,
	.set_enabled = [](void* cookie, uint8 bus, uint8 device, uint8 function,
			uint8 pin, bool enabled) {
		uint32 irq;
		return GetIntxIrq(cookie, bus, device, function, pin, &irq);
	}
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

_EXPORT module_info* modules[] = {
	(module_info*)&sController,
	(module_info*)&sIntxController,
	NULL
};
