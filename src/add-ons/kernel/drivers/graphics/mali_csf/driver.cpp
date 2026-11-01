/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <driver_settings.h>
#include <lock.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "CsfReset.h"
#include "CsfRun.h"
#include "CsfCommands.h"
#include "CsfDevice.h"
#include "CsfClient.h"
#include "CsfRuntime.h"
#include "CsfSynchronization.h"


using namespace MaliCSF;

#define DRIVER_NAME "drivers/graphics/mali_csf/driver_v1"
#define DEVICE_NAME "drivers/graphics/mali_csf/device_v1"

static device_manager_info* sDeviceManager;
static mutex sHardwareLock = MUTEX_INITIALIZER("Mali CSF platform");

struct Controller {
	device_node* node;
	ResourceInfo resources;
	bool identityEnabled;
	bool resetEnabled;
	bool firmwareEnabled;
	bool commandsEnabled;
	bool shaderEnabled;
	bool identityNeedsRecovery;
};

struct OpenHandle {
	Controller* controller;
	void* client;
	void* synchronization;
};


#include "CsfHardware.h"


static status_t
ReadPlatform(const ResourceInfo& resources, PlatformSnapshot& output)
{
	if (!ResourcesMatch(resources))
		return B_NOT_SUPPORTED;

	// Only these two always-on controller pages. Never map the GPU window.
	void* clockAddress = NULL;
	AreaDeleter clockArea(map_physical_memory("Mali CRU observation",
		resources.clockBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA, &clockAddress));
	if (clockArea.Get() < B_OK)
		return clockArea.Get();
	void* powerAddress = NULL;
	AreaDeleter powerArea(map_physical_memory("Mali PMU observation",
		resources.powerBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA, &powerAddress));
	if (powerArea.Get() < B_OK)
		return powerArea.Get();
	const volatile uint32* clock = (const volatile uint32*)clockAddress;
	const volatile uint32* power = (const volatile uint32*)powerAddress;
	PlatformSnapshot snapshot = {};
	snapshot.version = kPlatformVersion;
	snapshot.flags = kPlatformReadOnly;
	snapshot.startedMicros = system_time();
	memory_read_barrier();
	for (unsigned i = 0; i < 3; i++)
		snapshot.clockSelect[i] = ReadPlatformRegister(clock, kClockSelectOffsets[i]);
	for (unsigned i = 0; i < 2; i++)
		snapshot.clockGate[i] = ReadPlatformRegister(clock, kClockGateOffsets[i]);
	snapshot.idleRequest = ReadPlatformRegister(power, kIdleRequestOffset);
	snapshot.idleAck = ReadPlatformRegister(power, kIdleAckOffset);
	snapshot.idleStatus = ReadPlatformRegister(power, kIdleStatusOffset);
	snapshot.powerRequest = ReadPlatformRegister(power, kPowerRequestOffset);
	snapshot.powerRepair = ReadPlatformRegister(power, kPowerRepairOffset);
	snapshot.finishedMicros = system_time();
	output = snapshot;
	return B_OK;
}


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL
			|| sDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* value) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return StringIndex(data, length, value) >= 0;
	}

	bool Cells(const char* property, uint32_t* cells, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return ReadCells(data, length, cells, count);
	}

	bool Names(const char* property, const char* const* names, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		size_t expected = 0;
		for (size_t i = 0; i < count; i++) {
			expected += strlen(names[i]) + 1;
			if (StringIndex(data, length, names[i]) != (int)i)
				return false;
		}
		return length >= 0 && (size_t)length == expected;
	}

	bool Reg(uint64_t& base, uint64_t& size) const
	{
		uint64 address, length;
		if (!fModule->get_reg(fDevice, 0, &address, &length))
			return false;
		base = address;
		size = length;
		return true;
	}

	bool Enabled() const
	{
		int length = 0;
		const void* status = fModule->get_prop(fDevice, "status", &length);
		return status == NULL || (length == 5 && memcmp(status, "okay", 5) == 0)
			|| (length == 3 && memcmp(status, "ok", 3) == 0);
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


static bool
BoardMatches(device_node* gpu)
{
	device_node* node = sDeviceManager->get_parent_node(gpu);
	while (node != NULL) {
		FdtNode fdt;
		if (!fdt.SetTo(node)) {
			sDeviceManager->put_node(node);
			return false;
		}
		bool found = strcmp(fdt.fModule->get_name(fdt.fDevice), "") == 0;
		if (found) {
			bool matches = fdt.HasString("compatible", "radxa,rock-5-itx");
			sDeviceManager->put_node(node);
			return matches;
		}
		device_node* parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->put_node(node);
		node = parent;
	}
	return false;
}


static bool
ReadResources(device_node* parent, ResourceInfo& output)
{
	ResourceInfo info = {};
	FdtNode gpu;
	if (!gpu.SetTo(parent) || !gpu.Enabled()
		|| !gpu.HasString("compatible", "rockchip,rk3588-mali")
		|| !gpu.HasString("compatible", "arm,mali-valhall-csf")
		|| !BoardMatches(parent) || !gpu.Reg(info.gpuBase, info.gpuSize)) {
		return false;
	}
	const char* const irqNames[] = {"job", "mmu", "gpu"};
	const char* const clockNames[] = {"core", "coregroup", "stacks"};
	uint32_t specifiers[12], clocks[6], power[2], supply;
	if (!gpu.Names("interrupt-names", irqNames, 3)
		|| !gpu.Names("clock-names", clockNames, 3)
		|| !gpu.Cells("interrupts", specifiers, 12)
		|| !gpu.Cells("clocks", clocks, 6)
		|| !gpu.Cells("power-domains", power, 2)
		|| !gpu.Cells("mali-supply", &supply, 1)
		|| gpu.fModule->get_prop(gpu.fDevice, "interrupts-extended", NULL) != NULL) {
		return false;
	}
	device_node* gicNode = NULL;
	for (unsigned i = 0; i < 3; i++) {
		device_node* controller;
		uint64 irq;
		if (specifiers[i * 4] != 0 || specifiers[i * 4 + 1] != 92 + i
			|| specifiers[i * 4 + 2] != 4 || specifiers[i * 4 + 3] != 0
			|| !gpu.fModule->get_interrupt(gpu.fDevice, i, &controller, &irq)
			|| controller == NULL || irq != 124 + i
			|| (gicNode != NULL && gicNode != controller)) {
			return false;
		}
		gicNode = controller;
		info.interrupts[i] = irq;
	}
	FdtNode gic;
	uint64_t gicSize;
	uint32_t cells;
	if (!gic.SetTo(gicNode) || !gic.HasString("compatible", "arm,gic-v3")
		|| !gic.Cells("#interrupt-cells", &cells, 1) || cells != 4
		|| !gic.Reg(info.interruptBase, gicSize) || gicSize != 0x10000) {
		return false;
	}
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(gpu.fModule->get_bus(gpu.fDevice),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK
		|| strcmp(busModule->info.info.name, "bus_managers/fdt/root/driver_v1") != 0) {
		return false;
	}
	if (clocks[0] == 0 || clocks[0] != clocks[2] || clocks[0] != clocks[4])
		return false;
	FdtNode clock;
	if (!clock.SetTo(busModule->node_by_phandle(bus, clocks[0]))
		|| !clock.Enabled() || !clock.HasString("compatible", "rockchip,rk3588-cru")
		|| !clock.Cells("#clock-cells", &cells, 1) || cells != 1
		|| !clock.Reg(info.clockBase, info.clockSize)) {
		return false;
	}
	for (unsigned i = 0; i < 3; i++)
		info.clockIds[i] = clocks[i * 2 + 1];
	device_node* powerNode = busModule->node_by_phandle(bus, power[0]);
	FdtNode powerController;
	if (!powerController.SetTo(powerNode) || !powerController.Enabled()
		|| !powerController.HasString("compatible", "rockchip,rk3588-power-controller")
		|| !powerController.Cells("#power-domain-cells", &cells, 1) || cells != 1) {
		return false;
	}
	device_node* pmuNode = sDeviceManager->get_parent_node(powerNode);
	FdtNode pmu;
	bool validPower = pmu.SetTo(pmuNode) && pmu.Enabled()
		&& pmu.HasString("compatible", "rockchip,rk3588-pmu")
		&& pmu.Reg(info.powerBase, info.powerSize);
	if (pmuNode != NULL)
		sDeviceManager->put_node(pmuNode);
	if (!validPower)
		return false;
	info.powerDomain = power[1];
	FdtNode regulator;
	if (!regulator.SetTo(busModule->node_by_phandle(bus, supply))
		|| !regulator.Enabled() || !regulator.HasString("regulator-name", "vdd_gpu_s0")
		|| !regulator.Cells("regulator-min-microvolt", &info.supplyMinMicrovolt, 1)
		|| !regulator.Cells("regulator-max-microvolt", &info.supplyMaxMicrovolt, 1)) {
		return false;
	}
	info.supplyPhandle = supply;
	strcpy(info.supplyName, "vdd_gpu_s0");
	strcpy(info.boardCompatible, "radxa,rock-5-itx");
	info.version = kResourceVersion;
	info.flags = kDescriptionValidated;
	if (!ResourcesMatch(info))
		return false;
	output = info;
	return true;
}


static device_node*
FindNamedChild(device_node* parent, const char* name)
{
	device_attr attributes[] = {
		{"fdt/name", B_STRING_TYPE, {.string = name}},
		{}
	};
	device_node* node = NULL;
	if (sDeviceManager->find_child_node(parent, attributes, &node) != B_OK)
		return NULL;
	device_node* actualParent = sDeviceManager->get_parent_node(node);
	bool directChild = actualParent == parent;
	if (actualParent != NULL)
		sDeviceManager->put_node(actualParent);
	if (!directChild) {
		sDeviceManager->put_node(node);
		return NULL;
	}
	return node;
}


static bool
ReadIdentityProfile(device_node* parent, const ResourceInfo& resources)
{
	ResourceInfo current;
	if (!ReadResources(parent, current) || memcmp(&current, &resources, sizeof(current)) != 0)
		return false;
	FdtNode gpu;
	uint32_t clocks[6], power[2];
	if (!gpu.SetTo(parent) || !gpu.Cells("clocks", clocks, 6)
		|| !gpu.Cells("power-domains", power, 2) || power[1] != 12) {
		return false;
	}
	device_node* root = sDeviceManager->get_parent_node(parent);
	while (root != NULL) {
		FdtNode node;
		if (!node.SetTo(root)) {
			sDeviceManager->put_node(root);
			return false;
		}
		if (strcmp(node.fModule->get_name(node.fDevice), "") == 0)
			break;
		device_node* next = sDeviceManager->get_parent_node(root);
		sDeviceManager->put_node(root);
		root = next;
	}
	if (root == NULL)
		return false;
	device_node* fixedNode = FindNamedChild(root, "clock-0");
	sDeviceManager->put_node(root);
	FdtNode fixed;
	uint32_t cells, frequency;
	const char* const outputNames[] = {"spll"};
	bool valid = fixed.SetTo(fixedNode) && fixed.Enabled()
		&& fixed.HasString("compatible", "fixed-clock")
		&& fixed.Names("clock-output-names", outputNames, 1)
		&& fixed.Cells("#clock-cells", &cells, 1) && cells == 0
		&& fixed.Cells("clock-frequency", &frequency, 1) && frequency == 702000000;
	if (fixedNode != NULL)
		sDeviceManager->put_node(fixedNode);
	if (!valid)
		return false;
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(gpu.fModule->get_bus(gpu.fDevice),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return false;
	}
	device_node* controller = busModule->node_by_phandle(bus, power[0]);
	if (controller == NULL)
		return false;
	device_node* domainNode = FindNamedChild(controller, "power-domain@12");
	FdtNode domain;
	uint32_t domainId, supply, domainClocks[6];
	valid = domain.SetTo(domainNode) && domain.Enabled()
		&& domain.Cells("reg", &domainId, 1) && domainId == 12
		&& domain.Cells("#power-domain-cells", &cells, 1) && cells == 0
		&& domain.Cells("clocks", domainClocks, 6)
		&& memcmp(clocks, domainClocks, sizeof(clocks)) == 0
		&& domain.Cells("domain-supply", &supply, 1) && supply == resources.supplyPhandle;
	if (domainNode != NULL)
		sDeviceManager->put_node(domainNode);
	FdtNode regulator;
	return valid && regulator.SetTo(busModule->node_by_phandle(bus, resources.supplyPhandle))
		&& regulator.fModule->get_prop(regulator.fDevice, "regulator-boot-on", NULL) != NULL;
}


static float
SupportsDevice(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	ResourceInfo resources;
	return ReadResources(parent, resources) ? 0.8f : 0;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attributes[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Mali CSF resource interface"}},
		{NULL}
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attributes, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	Controller* controller = (Controller*)calloc(1, sizeof(Controller));
	if (controller == NULL)
		return B_NO_MEMORY;
	device_node* parent = sDeviceManager->get_parent_node(node);
	bool valid = ReadResources(parent, controller->resources);
	void* settings = load_driver_settings("mali_csf");
	if (valid && settings != NULL) {
		const char* profile = get_driver_parameter(settings, "firmware_profile", "", "");
		bool shader = strcmp(profile, "rock5-itx-edk2-v1.1-gpu-shader") == 0;
		bool commands = shader || strcmp(profile, "rock5-itx-edk2-v1.1-gpu-commands") == 0;
		bool firmware = commands || strcmp(profile, "rock5-itx-edk2-v1.1-gpu-firmware") == 0;
		bool reset = firmware || strcmp(profile, "rock5-itx-edk2-v1.1-gpu-reset") == 0;
		controller->identityEnabled = (reset || strcmp(profile, "rock5-itx-edk2-v1.1-gpu-identity") == 0)
			&& ReadIdentityProfile(parent, controller->resources);
		controller->resetEnabled = reset && controller->identityEnabled;
		controller->firmwareEnabled = firmware && controller->identityEnabled;
		controller->commandsEnabled = commands && controller->identityEnabled;
		controller->shaderEnabled = shader && controller->identityEnabled;
	}
	if (settings != NULL)
		unload_driver_settings(settings);
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	if (!valid) {
		free(controller);
		return B_NOT_SUPPORTED;
	}
	controller->node = node;
	controller->identityNeedsRecovery = FirmwareMemoryRetained();
	dprintf("mali_csf: validated firmware resources at %#" B_PRIx64
		"; identity profile %s\n", controller->resources.gpuBase,
		controller->identityEnabled ? "enabled (inherited firmware supply)" : "disabled");
	*cookie = controller;
	return B_OK;
}


static void UninitDriver(void* cookie) { free(cookie); }
static status_t InitDevice(void* driver, void** device) { *device = driver; return B_OK; }
static void UninitDevice(void*) {}
static status_t Close(void* cookie)
{
	OpenHandle* opened = (OpenHandle*)cookie;
	MutexLocker locker(sHardwareLock);
	CloseQueues(opened->client, opened->controller->identityNeedsRecovery);
	CloseSyncClient(opened->synchronization);
	CloseClient(opened->client);
	return B_OK;
}
static status_t Free(void* cookie)
{
	FreeSyncClient(((OpenHandle*)cookie)->synchronization);
	FreeClient(((OpenHandle*)cookie)->client);
	free(cookie);
	return B_OK;
}


static status_t
PublishDevices(void* cookie)
{
	return sDeviceManager->publish_device(((Controller*)cookie)->node,
		"graphics/mali_csf/0", DEVICE_NAME);
}


static status_t
Open(void* cookie, const char*, int mode, void** handle)
{
	Controller* controller = (Controller*)cookie;
	int access = mode & O_ACCMODE;
	if (access != O_RDONLY && access != O_RDWR)
		return B_NOT_ALLOWED;
	if (access == O_RDWR && (!controller->shaderEnabled || geteuid() != 0))
		return B_NOT_ALLOWED;
	OpenHandle* opened = (OpenHandle*)calloc(1, sizeof(OpenHandle));
	if (opened == NULL)
		return B_NO_MEMORY;
	status_t status = OpenClient(access == O_RDWR, &opened->client);
	if (status != B_OK) {
		free(opened);
		return status;
	}
	opened->controller = controller;
	status = OpenSyncClient(access == O_RDWR, &opened->synchronization);
	if (status != B_OK) {
		FreeClient(opened->client); free(opened); return status;
	}
	*handle = opened;
	return B_OK;
}


static status_t
Read(void*, off_t, void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}


static status_t
Write(void*, off_t, const void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}


static status_t
Control(void* cookie, uint32 op, void* buffer, size_t length)
{
	OpenHandle* opened = (OpenHandle*)cookie;
	status_t access = AccessClient(opened->client);
	if (access != B_OK)
		return access;
	if ((op >= kGetClientInfo && op <= kGetBufferInfo) || (op >= kCreateVm && op <= kBindVm)
		|| (op >= kCreateHeap && op <= kGetHeapInfo))
		return ControlClient(opened->client, op, buffer, length);
	if (op >= kCreateSync && op <= kTransferSync)
		return ControlSync(opened->synchronization, op, buffer, length);
	Controller* controller = opened->controller;
	if (op >= kCreateQueue && op <= kGetQueueResetInfo) {
		if (!controller->shaderEnabled)
			return B_NOT_ALLOWED;
		if (op == kCreateQueue || op == kDestroyQueue) {
			MutexLocker locker(sHardwareLock);
			if (op == kCreateQueue && controller->identityNeedsRecovery)
				return B_BUSY;
			return ControlQueues(controller->resources, opened->client, op, buffer,
				length, controller->identityNeedsRecovery);
		}
		// A waiting application must not prevent another thread from closing
		// queues. These operations never modify controller recovery state.
		bool ignored = false;
		return ControlQueues(controller->resources, opened->client, op, buffer, length,
			ignored, opened->synchronization);
	}
	if (op == kCycleCommands || op == kCycleShader) {
		MutexLocker locker(sHardwareLock);
		if (op == kCycleShader ? !controller->shaderEnabled : !controller->commandsEnabled)
			return B_NOT_ALLOWED;
		if (controller->identityNeedsRecovery || FirmwareMemoryRetained())
			return B_BUSY;
		return RunCommandRequest(controller->resources, buffer, length,
			controller->identityNeedsRecovery, op == kCycleShader);
	}
	if (op == kCycleFirmware) {
		MutexLocker locker(sHardwareLock);
		if (!controller->firmwareEnabled)
			return B_NOT_ALLOWED;
		if (controller->identityNeedsRecovery || FirmwareMemoryRetained())
			return B_BUSY;
		return RunFirmwareRequest(controller->resources, buffer, length,
			controller->identityNeedsRecovery);
	}
	if (op == kCycleReset) {
		if (length != sizeof(ResetInfo))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		MutexLocker locker(sHardwareLock);
		if (!controller->resetEnabled)
			return B_NOT_ALLOWED;
		if (controller->identityNeedsRecovery || FirmwareMemoryRetained())
			return B_BUSY;
		ResetHardware hardware;
		status_t status = hardware.Init(controller->resources);
		if (status != B_OK)
			return status;
		ResetInfo info;
		CycleReset(hardware, info, controller->resources.interrupts[2]);
		controller->identityNeedsRecovery = (info.flags & kResetNeedsRecovery) != 0;
		dprintf("mali_csf: reset result=%u cleanup=%u flags=%#x irq=%u count=%u"
			" status=%#x power=%u restore=%u\n", info.result, info.cleanupResult,
			info.flags, info.interrupt, info.capture.count, info.capture.status,
			info.power.result, info.power.restoreResult);
		return user_memcpy(buffer, &info, sizeof(info));
	}
	if (op == kCycleIdentity) {
		if (length != sizeof(IdentityInfo))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		MutexLocker locker(sHardwareLock);
		if (!controller->identityEnabled)
			return B_NOT_ALLOWED;
		if (controller->identityNeedsRecovery || FirmwareMemoryRetained())
			return B_BUSY;
		IdentityHardware hardware;
		status_t status = hardware.Init(controller->resources);
		if (status != B_OK)
			return status;
		IdentityInfo info;
		CycleIdentity(hardware, info);
		controller->identityNeedsRecovery = (info.flags & kIdentityNeedsRecovery) != 0;
		dprintf("mali_csf: identity result=%u restore=%u flags=%#x gpu=%#x\n",
			info.result, info.restoreResult, info.flags, info.gpuID);
		return user_memcpy(buffer, &info, sizeof(info));
	}
	if (op == kGetPlatformSnapshot) {
		if (length != sizeof(PlatformSnapshot))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		MutexLocker locker(sHardwareLock);
		PlatformSnapshot snapshot;
		status_t status = ReadPlatform(controller->resources, snapshot);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &snapshot, sizeof(snapshot));
	}
	if (op != kGetResources)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(ResourceInfo))
		return B_BAD_VALUE;
	return user_memcpy(buffer, &controller->resources, sizeof(ResourceInfo));
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

// An application sees a refused request only as a failed allocation or a lost
// context. Keep the first few refusals in the syslog, with the request named.
static int32 sControlFailuresLogged;

static status_t
ControlLogged(void* cookie, uint32 op, void* buffer, size_t length)
{
	status_t status = Control(cookie, op, buffer, length);
	if (status != B_OK && status != B_TIMED_OUT && status != B_WOULD_BLOCK
		&& status != B_INTERRUPTED && atomic_add(&sControlFailuresLogged, 1) < 64) {
		dprintf("mali_csf: request 0x%" B_PRIx32 " (%" B_PRIuSIZE " bytes) failed: %s\n",
			op, length, strerror(status));
	}
	return status;
}

static device_module_info sDevice = {
	{DEVICE_NAME, 0, NULL},
	InitDevice, UninitDevice, NULL,
	Open, Close, Free, Read, Write, NULL, ControlLogged, NULL, NULL
};

static driver_module_info sDriver = {
	{DRIVER_NAME, 0, NULL},
	SupportsDevice, RegisterDevice, InitDriver, UninitDriver, PublishDevices,
	NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
