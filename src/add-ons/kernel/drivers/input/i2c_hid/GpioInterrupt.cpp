/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	GpioInt interrupts of I2C HID devices on AMD and Intel GPIO controllers.


#include "GpioInterrupt.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <arch/int.h>
#include <lock.h>
#include <util/AutoLock.h>

extern "C" {
#	include "acpi.h"
}

#include "Driver.h"
#include "IntelGpioTables.h"


struct GpioController {
	GpioController*		next;
	char				path[64];
	int32				irq;
	spinlock			lock;
	GpioInterrupt*		interrupts;
	uint32				spurious;

						GpioController();
	virtual				~GpioController();

	// Finds the pin and checks that it can be used; fills in the
	// interrupt's location.
	virtual	status_t	Locate(GpioInterrupt* interrupt) = 0;
	// Programs the pin's trigger and leaves its interrupt masked; sets
	// fCanInterrupt.
	virtual	void		Configure(GpioInterrupt* interrupt) = 0;
	virtual	void		SetUnmasked(GpioInterrupt* interrupt,
							bool unmasked) = 0;
	virtual	bool		PinHigh(const GpioInterrupt* interrupt) const = 0;
	virtual	int32		Handle() = 0;

			GpioInterrupt*	Find(uint8 community, uint8 reg, uint8 bit) const;
	static	int32		HandleInterrupt(void* data);
};


static mutex sControllerLock = MUTEX_INITIALIZER("i2c_hid gpio");
static GpioController* sControllers;


struct gpio_int_resource {
	bool		found;
	uint32		pin;
	bool		levelTriggered;
	bool		activeLow;
	char		source[64];
};


static const uint32 kMaxMemoryResources = 8;

struct controller_resources {
	uint64		base[kMaxMemoryResources];
	uint32		length[kMaxMemoryResources];
	uint32		count;
	int32		irq;
	bool		levelTriggered;
	bool		activeLow;
};


static acpi_status
parse_gpio_int(ACPI_RESOURCE* resource, void* context)
{
	gpio_int_resource* gpio = (gpio_int_resource*)context;
	if (gpio->found || resource->Type != ACPI_RESOURCE_TYPE_GPIO)
		return AE_OK;

	ACPI_RESOURCE_GPIO& data = resource->Data.Gpio;
	if (data.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT
		|| data.PinTableLength == 0 || data.ResourceSource.StringPtr == NULL)
		return AE_OK;

	gpio->found = true;
	gpio->pin = data.PinTable[0];
	gpio->levelTriggered = data.Triggering == ACPI_LEVEL_SENSITIVE;
	gpio->activeLow = data.Polarity == ACPI_ACTIVE_LOW;
	strlcpy(gpio->source, data.ResourceSource.StringPtr,
		sizeof(gpio->source));
	return AE_OK;
}


static void
add_memory(controller_resources* controller, uint64 base, uint64 length)
{
	if (controller->count >= kMaxMemoryResources || length == 0)
		return;
	controller->base[controller->count] = base;
	controller->length[controller->count] = (uint32)length;
	controller->count++;
}


static acpi_status
parse_controller(ACPI_RESOURCE* resource, void* context)
{
	controller_resources* controller = (controller_resources*)context;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			add_memory(controller, resource->Data.FixedMemory32.Address,
				resource->Data.FixedMemory32.AddressLength);
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			add_memory(controller, resource->Data.Memory32.Minimum,
				resource->Data.Memory32.AddressLength);
			break;
		case ACPI_RESOURCE_TYPE_ADDRESS32:
			if (resource->Data.Address32.ResourceType == ACPI_MEMORY_RANGE) {
				add_memory(controller,
					resource->Data.Address32.Address.Minimum,
					resource->Data.Address32.Address.AddressLength);
			}
			break;
		case ACPI_RESOURCE_TYPE_ADDRESS64:
			if (resource->Data.Address64.ResourceType == ACPI_MEMORY_RANGE) {
				add_memory(controller,
					resource->Data.Address64.Address.Minimum,
					resource->Data.Address64.Address.AddressLength);
			}
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (controller->irq < 0
				&& resource->Data.ExtendedIrq.InterruptCount > 0) {
				controller->irq = resource->Data.ExtendedIrq.Interrupts[0];
				controller->levelTriggered
					= resource->Data.ExtendedIrq.Triggering
						== ACPI_LEVEL_SENSITIVE;
				controller->activeLow
					= resource->Data.ExtendedIrq.Polarity == ACPI_ACTIVE_LOW;
			}
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (controller->irq < 0 && resource->Data.Irq.InterruptCount > 0) {
				controller->irq = resource->Data.Irq.Interrupts[0];
				controller->levelTriggered = resource->Data.Irq.Triggering
					== ACPI_LEVEL_SENSITIVE;
				controller->activeLow
					= resource->Data.Irq.Polarity == ACPI_ACTIVE_LOW;
			}
			break;
	}
	return AE_OK;
}


//	#pragma mark - GpioController


GpioController::GpioController()
	:
	next(NULL),
	irq(-1),
	interrupts(NULL),
	spurious(0)
{
	path[0] = '\0';
	B_INITIALIZE_SPINLOCK(&lock);
}


GpioController::~GpioController()
{
}


GpioInterrupt*
GpioController::Find(uint8 community, uint8 reg, uint8 bit) const
{
	for (GpioInterrupt* interrupt = interrupts; interrupt != NULL;
			interrupt = interrupt->fNext) {
		if (interrupt->fCommunity == community && interrupt->fRegister == reg
			&& interrupt->fBit == bit)
			return interrupt;
	}
	return NULL;
}


/*static*/ int32
GpioController::HandleInterrupt(void* data)
{
	return ((GpioController*)data)->Handle();
}


//	#pragma mark - AmdGpioController


// Pin registers of the AMD FCH GPIO controller (Linux: pinctrl-amd.h). One
// 32-bit register per pin; three registers in the pin range are not pins.
enum {
	kAmdLevelTrigger		= 1 << 8,
	kAmdActiveLevelShift	= 9,
	kAmdActiveLevelMask		= 3 << kAmdActiveLevelShift,
	kAmdActiveLow			= 1 << kAmdActiveLevelShift,
	kAmdInterruptEnable		= 1 << 11,
	kAmdInterruptUnmasked	= 1 << 12,
		// pinctrl-amd calls this INTERRUPT_MASK; set means delivered
	kAmdPinState			= 1 << 16,
	kAmdInterruptStatus		= 1 << 28,
	kAmdWakeStatus			= 1 << 29,
	kAmdPending				= kAmdInterruptStatus | kAmdWakeStatus,
};

static const uint32 kAmdMasterRegister = 0xfc;
static const uint32 kAmdMasterEndOfInterrupt = 1 << 29;
static const uint32 kAmdStatusRegister0 = 0x2f8;
static const uint32 kAmdStatusRegister1 = 0x2fc;
static const uint32 kAmdStatusBits = 46;
	// each status bit stands for four pins
static const uint32 kAmdPinCount = kAmdStatusBits * 4;

static const char* const kAmdControllerIDs[] = {
	"AMDI0030",
	"AMDI0031",
	"AMD0030",
	NULL
};


struct AmdGpioController : GpioController {
	area_id				area;
	volatile uint32*	registers;
	size_t				size;

						AmdGpioController();
	virtual				~AmdGpioController();

			status_t	Init(const controller_resources& resources);

			bool		IsPin(uint32 pin) const
							{ return pin < kAmdPinCount
								&& pin * 4 != kAmdMasterRegister
								&& (pin + 1) * 4 <= size; }
			uint32		Read(uint32 offset) const
							{ return registers[offset / 4]; }
			void		Write(uint32 offset, uint32 value)
							{ registers[offset / 4] = value; }

	virtual	status_t	Locate(GpioInterrupt* interrupt);
	virtual	void		Configure(GpioInterrupt* interrupt);
	virtual	void		SetUnmasked(GpioInterrupt* interrupt, bool unmasked);
	virtual	bool		PinHigh(const GpioInterrupt* interrupt) const;
	virtual	int32		Handle();
};


AmdGpioController::AmdGpioController()
	:
	area(-1),
	registers(NULL),
	size(0)
{
}


AmdGpioController::~AmdGpioController()
{
	if (area >= 0)
		delete_area(area);
}


status_t
AmdGpioController::Init(const controller_resources& resources)
{
	if (resources.count == 0 || resources.length[0] < 0x300)
		return B_BAD_DATA;

	size = resources.length[0];
	area = map_physical_memory("i2c_hid gpio controller", resources.base[0],
		size, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&registers);
	if (area < 0)
		return area;

	// Firmware may have left interrupts unmasked on pins nobody here
	// handles; once the controller's interrupt is enabled they would
	// assert it forever. Linux masks them all when it takes the
	// controller over, too.
	uint32 masked = 0;
	for (uint32 pin = 0; pin < kAmdPinCount; pin++) {
		if (!IsPin(pin))
			continue;
		uint32 value = Read(pin * 4);
		if ((value & kAmdInterruptUnmasked) != 0) {
			Write(pin * 4, value & ~(kAmdInterruptUnmasked | kAmdPending));
			masked++;
		}
	}

	TRACE_ALWAYS("GPIO controller %s (AMD) at %#" B_PRIx64 ", %" B_PRIu32
		" firmware interrupt%s masked\n", path, resources.base[0], masked,
		masked == 1 ? "" : "s");
	return B_OK;
}


status_t
AmdGpioController::Locate(GpioInterrupt* interrupt)
{
	if (!IsPin(interrupt->fPin))
		return B_BAD_VALUE;
	interrupt->fCommunity = 0;
	interrupt->fRegister = 0;
	interrupt->fBit = 0;
	interrupt->fPadOffset = interrupt->fPin * 4;
	return B_OK;
}


void
AmdGpioController::Configure(GpioInterrupt* interrupt)
{
	uint32 offset = interrupt->fPin * 4;
	InterruptsSpinLocker locker(lock);
	uint32 value = Read(offset);
	value &= ~(kAmdLevelTrigger | kAmdActiveLevelMask | kAmdInterruptUnmasked
		| kAmdPending);
	if (interrupt->fLevelTriggered)
		value |= kAmdLevelTrigger;
	if (interrupt->fActiveLow)
		value |= kAmdActiveLow;
	value |= kAmdInterruptEnable;
	Write(offset, value);
	interrupt->fCanInterrupt = true;
}


void
AmdGpioController::SetUnmasked(GpioInterrupt* interrupt, bool unmasked)
{
	uint32 offset = interrupt->fPin * 4;
	InterruptsSpinLocker locker(lock);
	uint32 value = Read(offset) & ~kAmdPending;
	if (unmasked)
		value |= kAmdInterruptEnable | kAmdInterruptUnmasked;
	else
		value &= ~kAmdInterruptUnmasked;
	Write(offset, value);
}


bool
AmdGpioController::PinHigh(const GpioInterrupt* interrupt) const
{
	return (Read(interrupt->fPin * 4) & kAmdPinState) != 0;
}


int32
AmdGpioController::Handle()
{
	uint64 status = Read(kAmdStatusRegister1);
	status = (status << 32) | Read(kAmdStatusRegister0);
	status &= (1ULL << kAmdStatusBits) - 1;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	sem_id wake[8];
	uint32 wakeCount = 0;
	bool handled = false;

	SpinLocker locker(lock);
	for (uint32 bit = 0; bit < kAmdStatusBits; bit++) {
		if ((status & (1ULL << bit)) == 0)
			continue;

		for (uint32 pin = bit * 4; pin < bit * 4 + 4; pin++) {
			if (!IsPin(pin))
				continue;

			uint32 value = Read(pin * 4);
			if ((value & kAmdPending) == 0)
				continue;

			GpioInterrupt* interrupt = interrupts;
			while (interrupt != NULL && interrupt->fPin != pin)
				interrupt = interrupt->fNext;

			if (interrupt != NULL) {
				// Masked until its device has been read; the level
				// trigger would raise it again right away otherwise.
				if ((value & kAmdInterruptUnmasked) != 0 && wakeCount < 8)
					wake[wakeCount++] = interrupt->fSemaphore;
			} else if ((value & kAmdInterruptUnmasked) != 0) {
				// Nobody handles this pin: keep it from storming.
				spurious++;
			}

			// writing the status bits back clears them
			Write(pin * 4, value & ~kAmdInterruptUnmasked);
			handled = true;
		}
	}

	Write(kAmdMasterRegister,
		Read(kAmdMasterRegister) | kAmdMasterEndOfInterrupt);
	locker.Unlock();

	for (uint32 i = 0; i < wakeCount; i++)
		release_sem_etc(wake[i], 1, B_DO_NOT_RESCHEDULE);

	if (wakeCount > 0)
		return B_INVOKE_SCHEDULER;
	return handled ? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
}


//	#pragma mark - IntelGpioController


// Registers of the Intel GPIO controllers (Linux: pinctrl-intel.c). Each
// community has its own registers: the pads' configuration from PADBAR on,
// and per group of up to 32 pads one register for each of ownership,
// interrupt status and interrupt enable.
static const uint32 kIntelRevision = 0x000;
static const uint32 kIntelPadBar = 0x00c;
static const uint32 kIntelPadOwnBits = 4;

enum {
	kIntelRxEventShift		= 25,
	kIntelRxEventMask		= 3 << kIntelRxEventShift,
	kIntelRxEventLevel		= 0 << kIntelRxEventShift,
	kIntelRxEventEdge		= 1 << kIntelRxEventShift,
	kIntelRxInvert			= 1 << 23,
	kIntelRouteIoxApic		= 1 << 20,
	kIntelRouteSci			= 1 << 19,
	kIntelRouteSmi			= 1 << 18,
	kIntelRouteNmi			= 1 << 17,
	kIntelPadModeMask		= 0xf << 10,
	kIntelRxDisable			= 1 << 9,
	kIntelTxDisable			= 1 << 8,
	kIntelRxState			= 1 << 1,
};

static const uint32 kIntelMaxCommunities = 8;


struct IntelCommunityRegisters {
	const IntelCommunity*	info;
	area_id					area;
	volatile uint8*			registers;
	uint32					size;
	uint32					padBar;
	uint32					padStride;
};


struct IntelGpioController : GpioController {
	const IntelGpioSoc*		soc;
	IntelCommunityRegisters	communities[kIntelMaxCommunities];
	uint32					communityCount;

							IntelGpioController(const IntelGpioSoc* soc);
	virtual					~IntelGpioController();

			status_t		Init(const controller_resources& resources);

			uint32			Read(uint32 community, uint32 offset) const
								{ return *(volatile uint32*)
									(communities[community].registers
										+ offset); }
			void			Write(uint32 community, uint32 offset,
								uint32 value)
								{ *(volatile uint32*)
									(communities[community].registers
										+ offset) = value; }

	virtual	status_t		Locate(GpioInterrupt* interrupt);
	virtual	void			Configure(GpioInterrupt* interrupt);
	virtual	void			SetUnmasked(GpioInterrupt* interrupt,
								bool unmasked);
	virtual	bool			PinHigh(const GpioInterrupt* interrupt) const;
	virtual	int32			Handle();
};


static const IntelGpioSoc*
find_intel_soc(const char* hid, char** cidList, const char* uid)
{
	for (const IntelGpioSoc* soc = kIntelGpioSocs; soc->hid != NULL; soc++) {
		bool match = hid != NULL && strcmp(hid, soc->hid) == 0;
		for (int i = 0; !match && cidList[i] != NULL; i++)
			match = strcmp(cidList[i], soc->hid) == 0;
		if (!match)
			continue;
		if (soc->uid == NULL || (uid != NULL && strcmp(uid, soc->uid) == 0))
			return soc;
	}
	return NULL;
}


IntelGpioController::IntelGpioController(const IntelGpioSoc* soc)
	:
	soc(soc),
	communityCount(0)
{
	memset(communities, 0, sizeof(communities));
	for (uint32 i = 0; i < kIntelMaxCommunities; i++)
		communities[i].area = -1;
}


IntelGpioController::~IntelGpioController()
{
	for (uint32 i = 0; i < kIntelMaxCommunities; i++) {
		if (communities[i].area >= 0)
			delete_area(communities[i].area);
	}
}


status_t
IntelGpioController::Init(const controller_resources& resources)
{
	if (soc->communityCount > kIntelMaxCommunities)
		return B_BAD_DATA;

	for (uint32 i = 0; i < soc->communityCount; i++) {
		IntelCommunityRegisters& community = communities[i];
		community.info = &kIntelCommunities[soc->firstCommunity + i];
		uint32 bar = community.info->bar;
		if (bar >= resources.count) {
			ERROR("GPIO controller %s: no memory resource %" B_PRIu32
				" for community %" B_PRIu32 "\n", path, bar, i);
			return B_BAD_DATA;
		}

		community.size = resources.length[bar];
		community.area = map_physical_memory("i2c_hid gpio community",
			resources.base[bar], community.size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&community.registers);
		if (community.area < 0)
			return community.area;
		communityCount = i + 1;

		// All ones: the community is not there (hidden by the firmware).
		uint32 revision = Read(i, kIntelRevision);
		if (revision == 0xffffffff) {
			ERROR("GPIO controller %s: community %" B_PRIu32 " is not "
				"there\n", path, i);
			return B_DEVICE_NOT_FOUND;
		}
		// From revision 0x94 on, a pad has four configuration registers.
		community.padStride = (revision >> 16) >= 0x94 ? 16 : 8;
		community.padBar = Read(i, kIntelPadBar);

		const IntelCommunity& info = *community.info;
		uint32 lastGroupRegister = 0;
		for (uint32 g = 0; g < info.groupCount; g++) {
			const IntelPadGroup& group
				= kIntelPadGroups[info.firstGroup + g];
			if (group.regNum > lastGroupRegister)
				lastGroupRegister = group.regNum;
		}
		uint64 padEnd = (uint64)community.padBar
			+ (uint64)info.pinCount * community.padStride;
		uint32 registerEnd = max_c(info.gpiIs, info.gpiIe)
			+ (lastGroupRegister + 1) * 4;
		if (padEnd > community.size || registerEnd > community.size) {
			ERROR("GPIO controller %s: community %" B_PRIu32 " does not fit "
				"its %#" B_PRIx32 " bytes (pads at %#" B_PRIx32 ")\n", path, i,
				community.size, community.padBar);
			return B_BAD_DATA;
		}
	}

	// Firmware may have left interrupts enabled that nobody here handles;
	// once the controller's interrupt is installed they would assert it
	// forever. Linux masks and clears them all when it takes the controller
	// over, too.
	uint32 masked = 0;
	for (uint32 i = 0; i < communityCount; i++) {
		const IntelCommunity& info = *communities[i].info;
		for (uint32 g = 0; g < info.groupCount; g++) {
			const IntelPadGroup& group = kIntelPadGroups[info.firstGroup + g];
			uint32 groupMask = group.size >= 32
				? 0xffffffff : (1u << group.size) - 1;
			uint32 enabled = Read(i, info.gpiIe + group.regNum * 4);
			if ((enabled & groupMask) != 0) {
				Write(i, info.gpiIe + group.regNum * 4, enabled & ~groupMask);
				masked += __builtin_popcount(enabled & groupMask);
			}
			Write(i, info.gpiIs + group.regNum * 4, groupMask);
		}
	}

	TRACE_ALWAYS("GPIO controller %s (Intel %s%s%s, %" B_PRIu32 " communit%s) "
		"at %#" B_PRIx64 ", %" B_PRIu32 " firmware interrupt%s masked\n", path,
		soc->hid, soc->uid != NULL ? " uid " : "",
		soc->uid != NULL ? soc->uid : "", communityCount,
		communityCount == 1 ? "y" : "ies", resources.base[0], masked,
		masked == 1 ? "" : "s");
	return B_OK;
}


status_t
IntelGpioController::Locate(GpioInterrupt* interrupt)
{
	// The GpioInt pin number counts the controller's GPIOs: each group of
	// pads has its own range of them (Linux: intel_gpio_to_pin()).
	for (uint32 i = 0; i < communityCount; i++) {
		const IntelCommunity& info = *communities[i].info;
		for (uint32 g = 0; g < info.groupCount; g++) {
			const IntelPadGroup& group = kIntelPadGroups[info.firstGroup + g];
			if (group.gpioBase == kIntelNoGpio
				|| interrupt->fPin < (uint32)group.gpioBase
				|| interrupt->fPin >= (uint32)group.gpioBase + group.size)
				continue;

			uint32 bit = interrupt->fPin - group.gpioBase;
			uint32 pad = group.base + bit;
			interrupt->fCommunity = i;
			interrupt->fRegister = group.regNum;
			interrupt->fBit = bit;
			interrupt->fPadOffset = communities[i].padBar
				+ (pad - info.pinBase) * communities[i].padStride;

			// The pad has to belong to the host: others (the ME, the
			// sensor hub) may own some.
			if (info.padOwn != 0) {
				uint32 offset = info.padOwn + group.padOwnNum * 4
					+ bit / 8 * 4;
				uint32 owner = (Read(i, offset)
					>> (bit % 8 * kIntelPadOwnBits)) & 0xf;
				if (owner != 0) {
					TRACE_ALWAYS("GPIO %s pin %" B_PRIu32 " (pad %" B_PRIu32
						"): owned by someone else (%" B_PRIu32 ")\n", path,
						interrupt->fPin, pad, owner);
					return B_NOT_ALLOWED;
				}
			}

			// A GpioInt pin is an input in GPIO mode; anything else means
			// the pin was not found right, and the pad is left alone.
			uint32 config = Read(i, interrupt->fPadOffset);
			if ((config & (kIntelPadModeMask | kIntelRxDisable)) != 0) {
				TRACE_ALWAYS("GPIO %s pin %" B_PRIu32 " (pad %" B_PRIu32
					"): not a GPIO input (%#" B_PRIx32 "), left alone\n", path,
					interrupt->fPin, pad, config);
				return B_BAD_VALUE;
			}
			return B_OK;
		}
	}
	return B_BAD_VALUE;
}


void
IntelGpioController::Configure(GpioInterrupt* interrupt)
{
	const IntelCommunity& info = *communities[interrupt->fCommunity].info;
	uint32 community = interrupt->fCommunity;
	uint32 bit = 1u << interrupt->fBit;

	// In ACPI mode the hardware does not set the pad's interrupt status
	// (Linux refuses such a pad as an interrupt); its level still reads.
	bool hostMode = info.hostSwOwn == 0
		|| (Read(community, info.hostSwOwn + interrupt->fRegister * 4)
			& bit) != 0;
	bool locked = info.padCfgLock != 0
		&& (Read(community, info.padCfgLock + interrupt->fRegister * 8)
			& bit) != 0;

	uint32 trigger = (interrupt->fLevelTriggered
			? kIntelRxEventLevel : kIntelRxEventEdge)
		| (interrupt->fActiveLow ? kIntelRxInvert : 0);

	InterruptsSpinLocker locker(lock);
	uint32 config = Read(community, interrupt->fPadOffset);
	bool triggerSet;
	if (!hostMode) {
		// The firmware's: Linux does not touch it either (its SCI routing
		// may be how the machine wakes up).
		triggerSet = false;
	} else if (!locked) {
		// As Linux does: an input, no SCI/SMI/NMI or direct I/O APIC
		// routing, the trigger of the GpioInt resource.
		uint32 value = config & ~(kIntelRxEventMask | kIntelRxInvert
			| kIntelRouteIoxApic | kIntelRouteSci | kIntelRouteSmi
			| kIntelRouteNmi);
		value |= kIntelTxDisable | trigger;
		if (value != config)
			Write(community, interrupt->fPadOffset, value);
		triggerSet = true;
	} else {
		triggerSet = (config & (kIntelRxEventMask | kIntelRxInvert))
			== trigger;
	}

	// masked, nothing pending
	Write(community, info.gpiIs + interrupt->fRegister * 4, bit);
	Write(community, info.gpiIe + interrupt->fRegister * 4,
		Read(community, info.gpiIe + interrupt->fRegister * 4) & ~bit);
	locker.Unlock();

	interrupt->fCanInterrupt = hostMode && triggerSet;
	if (!interrupt->fCanInterrupt) {
		TRACE_ALWAYS("GPIO %s pin %" B_PRIu32 ": its interrupt cannot be "
			"used (%s), watching its level instead\n", path, interrupt->fPin,
			!hostMode ? "ACPI mode" : "configuration locked");
	}
}


void
IntelGpioController::SetUnmasked(GpioInterrupt* interrupt, bool unmasked)
{
	const IntelCommunity& info = *communities[interrupt->fCommunity].info;
	uint32 community = interrupt->fCommunity;
	uint32 bit = 1u << interrupt->fBit;
	uint32 enable = info.gpiIe + interrupt->fRegister * 4;

	InterruptsSpinLocker locker(lock);
	// a stale status first, so that only what comes now counts
	Write(community, info.gpiIs + interrupt->fRegister * 4, bit);
	uint32 value = Read(community, enable);
	if (unmasked)
		value |= bit;
	else
		value &= ~bit;
	Write(community, enable, value);
}


bool
IntelGpioController::PinHigh(const GpioInterrupt* interrupt) const
{
	// the pad's level before any inversion
	return (Read(interrupt->fCommunity, interrupt->fPadOffset)
		& kIntelRxState) != 0;
}


int32
IntelGpioController::Handle()
{
	sem_id wake[8];
	uint32 wakeCount = 0;
	bool handled = false;

	SpinLocker locker(lock);
	for (uint32 i = 0; i < communityCount; i++) {
		const IntelCommunity& info = *communities[i].info;
		for (uint32 g = 0; g < info.groupCount; g++) {
			const IntelPadGroup& group = kIntelPadGroups[info.firstGroup + g];
			uint32 statusRegister = info.gpiIs + group.regNum * 4;
			uint32 enableRegister = info.gpiIe + group.regNum * 4;
			uint32 enabled = Read(i, enableRegister);
			uint32 pending = Read(i, statusRegister) & enabled;
			if (group.size < 32)
				pending &= (1u << group.size) - 1;
			if (pending == 0)
				continue;

			// Masked until the device has been read: a level trigger would
			// raise it again right away otherwise. Then acknowledged.
			Write(i, enableRegister, enabled & ~pending);
			Write(i, statusRegister, pending);
			handled = true;

			for (uint32 bit = 0; bit < 32; bit++) {
				if ((pending & (1u << bit)) == 0)
					continue;
				GpioInterrupt* interrupt = Find(i, group.regNum, bit);
				if (interrupt != NULL) {
					if (wakeCount < 8)
						wake[wakeCount++] = interrupt->fSemaphore;
				} else
					spurious++;
			}
		}
	}
	locker.Unlock();

	for (uint32 i = 0; i < wakeCount; i++)
		release_sem_etc(wake[i], 1, B_DO_NOT_RESCHEDULE);

	if (wakeCount > 0)
		return B_INVOKE_SCHEDULER;
	return handled ? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
}


//	#pragma mark - controllers


static status_t
create_controller(acpi_module_info* acpi, const char* path,
	GpioController** _controller)
{
	char* hid = NULL;
	char* cidList[8] = { NULL };
	char* uid = NULL;
	if (acpi->get_device_info(path, &hid, cidList, 8, &uid, NULL) != B_OK)
		return B_NOT_SUPPORTED;

	bool amd = false;
	for (int i = 0; kAmdControllerIDs[i] != NULL; i++) {
		if (hid != NULL && strcmp(hid, kAmdControllerIDs[i]) == 0)
			amd = true;
		for (int j = 0; cidList[j] != NULL; j++) {
			if (strcmp(cidList[j], kAmdControllerIDs[i]) == 0)
				amd = true;
		}
	}
	const IntelGpioSoc* intel = amd ? NULL : find_intel_soc(hid, cidList, uid);

	if (!amd && intel == NULL) {
		TRACE_ALWAYS("GPIO controller %s (%s%s%s) is not supported\n", path,
			hid != NULL ? hid : "no _HID", uid != NULL ? " uid " : "",
			uid != NULL ? uid : "");
	}

	free(hid);
	free(uid);
	for (int j = 0; cidList[j] != NULL; j++)
		free(cidList[j]);

	if (!amd && intel == NULL)
		return B_NOT_SUPPORTED;

	acpi_handle handle;
	if (acpi->get_handle(NULL, path, &handle) != B_OK)
		return B_ENTRY_NOT_FOUND;

	controller_resources resources = {};
	resources.irq = -1;
	status_t status = acpi->walk_resources(handle, (char*)"_CRS",
		(acpi_walk_resources_callback)parse_controller, &resources);
	if (status != B_OK || resources.count == 0 || resources.irq < 0) {
		ERROR("GPIO controller %s: unusable resources (%" B_PRIu32 " memory, "
			"irq %" B_PRId32 ")\n", path, resources.count, resources.irq);
		return B_BAD_DATA;
	}

	GpioController* controller;
	if (amd)
		controller = new(std::nothrow) AmdGpioController;
	else
		controller = new(std::nothrow) IntelGpioController(intel);
	if (controller == NULL)
		return B_NO_MEMORY;

	strlcpy(controller->path, path, sizeof(controller->path));
	controller->irq = resources.irq;

	if (amd)
		status = static_cast<AmdGpioController*>(controller)->Init(resources);
	else {
		status = static_cast<IntelGpioController*>(controller)
			->Init(resources);
	}
	if (status != B_OK) {
		ERROR("GPIO controller %s: %s\n", path, strerror(status));
		delete controller;
		return status;
	}

	if (resources.levelTriggered || resources.activeLow) {
		arch_int_configure_io_interrupt(resources.irq,
			resources.levelTriggered ? B_LEVEL_TRIGGERED : B_EDGE_TRIGGERED,
			resources.activeLow ? B_LOW_ACTIVE_POLARITY
				: B_HIGH_ACTIVE_POLARITY);
	}

	status = install_io_interrupt_handler(resources.irq,
		&GpioController::HandleInterrupt, controller, 0);
	if (status != B_OK) {
		ERROR("GPIO controller %s: cannot install interrupt %" B_PRId32 ": "
			"%s\n", path, resources.irq, strerror(status));
		delete controller;
		return status;
	}

	TRACE_ALWAYS("GPIO controller %s: irq %" B_PRId32 " (%s, %s)\n", path,
		resources.irq, resources.levelTriggered ? "level" : "edge",
		resources.activeLow ? "low" : "high");

	*_controller = controller;
	return B_OK;
}


static status_t
get_controller(acpi_module_info* acpi, const char* path,
	GpioController** _controller)
{
	for (GpioController* controller = sControllers; controller != NULL;
			controller = controller->next) {
		if (strcmp(controller->path, path) == 0) {
			*_controller = controller;
			return B_OK;
		}
	}

	GpioController* controller;
	status_t status = create_controller(acpi, path, &controller);
	if (status != B_OK)
		return status;

	controller->next = sControllers;
	sControllers = controller;
	*_controller = controller;
	return B_OK;
}


static void
put_controller(GpioController* controller)
{
	if (controller->interrupts != NULL)
		return;

	GpioController** link = &sControllers;
	while (*link != NULL && *link != controller)
		link = &(*link)->next;
	if (*link != NULL)
		*link = controller->next;

	remove_io_interrupt_handler(controller->irq,
		&GpioController::HandleInterrupt, controller);
	delete controller;
}


//	#pragma mark - GpioInterrupt


GpioInterrupt::GpioInterrupt(GpioController* controller, uint32 pin,
	bool levelTriggered, bool activeLow)
	:
	fController(controller),
	fNext(NULL),
	fPin(pin),
	fLevelTriggered(levelTriggered),
	fActiveLow(activeLow),
	fCanInterrupt(false),
	fRegistered(false),
	fSemaphore(-1),
	fCommunity(0),
	fRegister(0),
	fBit(0),
	fPadOffset(0)
{
}


GpioInterrupt::~GpioInterrupt()
{
	if (!fRegistered) {
		if (fSemaphore >= 0)
			delete_sem(fSemaphore);
		return;
	}

	MutexLocker locker(sControllerLock);
	Mask();

	{
		InterruptsSpinLocker spinLocker(fController->lock);
		GpioInterrupt** link = &fController->interrupts;
		while (*link != NULL && *link != this)
			link = &(*link)->fNext;
		if (*link != NULL)
			*link = fNext;
	}

	put_controller(fController);
	if (fSemaphore >= 0)
		delete_sem(fSemaphore);
}


/*static*/ status_t
GpioInterrupt::Create(acpi_module_info* acpi, acpi_handle device,
	GpioInterrupt** _interrupt)
{
	gpio_int_resource gpio = {};
	status_t status = acpi->walk_resources(device, (char*)"_CRS",
		(acpi_walk_resources_callback)parse_gpio_int, &gpio);
	if (status != B_OK)
		return status;
	if (!gpio.found)
		return B_ENTRY_NOT_FOUND;

	MutexLocker locker(sControllerLock);

	GpioController* controller;
	status = get_controller(acpi, gpio.source, &controller);
	if (status != B_OK)
		return status;

	for (GpioInterrupt* other = controller->interrupts; other != NULL;
			other = other->fNext) {
		if (other->fPin == gpio.pin)
			return B_BUSY;
	}

	GpioInterrupt* interrupt = new(std::nothrow) GpioInterrupt(controller,
		gpio.pin, gpio.levelTriggered, gpio.activeLow);
	if (interrupt == NULL) {
		put_controller(controller);
		return B_NO_MEMORY;
	}

	status = controller->Locate(interrupt);
	if (status != B_OK) {
		TRACE_ALWAYS("GPIO %s pin %" B_PRIu32 " cannot be used: %s\n",
			gpio.source, gpio.pin, strerror(status));
		delete interrupt;
		put_controller(controller);
		return status;
	}

	interrupt->fSemaphore = create_sem(0, "i2c_hid gpio interrupt");
	if (interrupt->fSemaphore < 0) {
		status = interrupt->fSemaphore;
		delete interrupt;
		put_controller(controller);
		return status;
	}

	controller->Configure(interrupt);

	{
		InterruptsSpinLocker spinLocker(controller->lock);
		interrupt->fNext = controller->interrupts;
		controller->interrupts = interrupt;
	}
	interrupt->fRegistered = true;

	TRACE_ALWAYS("interrupt on %s pin %" B_PRIu32 " (%s, active %s)%s\n",
		gpio.source, gpio.pin, gpio.levelTriggered ? "level" : "edge",
		gpio.activeLow ? "low" : "high",
		interrupt->fCanInterrupt ? "" : ", level watched");

	*_interrupt = interrupt;
	return B_OK;
}


bool
GpioInterrupt::Asserted() const
{
	bool high = fController->PinHigh(this);
	return fActiveLow ? !high : high;
}


status_t
GpioInterrupt::Wait(bigtime_t timeout)
{
	fController->SetUnmasked(this, true);
	return acquire_sem_etc(fSemaphore, 1, B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
		timeout);
}


void
GpioInterrupt::Mask()
{
	fController->SetUnmasked(this, false);
}


const char*
GpioInterrupt::ControllerName() const
{
	return fController->path;
}
