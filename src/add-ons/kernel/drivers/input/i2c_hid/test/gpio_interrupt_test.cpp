// Host test of i2c_hid's GpioInterrupt: fake ACPI, fake MMIO, fake interrupts.
// Built and run by run.sh (with -DTEST_NEW: AMD and Intel; without: the AMD part only,
// for comparing with an older GpioInterrupt.cpp).
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "GpioInterrupt.h"
#include <arch/int.h>
#include <util/AutoLock.h>

static int sFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); sFailures++; } } while (0)

// --- kernel stubs ---------------------------------------------------------

static bool sQuiet = false;
extern "C" int dprintf(const char* format, ...)
{
	if (sQuiet)
		return 0;
	va_list args;
	va_start(args, format);
	printf("  | ");
	int r = vprintf(format, args);
	va_end(args);
	return r;
}

const char* test_strerror(status_t s)
{
	switch (s) {
		case B_OK: return "ok";
		case B_BAD_VALUE: return "bad value";
		case B_NOT_ALLOWED: return "not allowed";
		case B_BAD_DATA: return "bad data";
		case B_NOT_SUPPORTED: return "not supported";
		case B_DEVICE_NOT_FOUND: return "device not found";
		case B_BUSY: return "busy";
		default: return "error";
	}
}

struct Region { uint64 base; size_t size; uint8* memory; };
static std::vector<Region> sRegions;
static int sAreas = 0;

static uint8* region(uint64 base, size_t size)
{
	for (auto& r : sRegions)
		if (r.base == base)
			return r.memory;
	Region r = { base, size, (uint8*)calloc(1, size) };
	sRegions.push_back(r);
	return r.memory;
}

extern "C" area_id map_physical_memory(const char*, phys_addr_t base, size_t size, uint32, uint32,
	void** address)
{
	for (auto& r : sRegions) {
		if (r.base == base) {
			assert(size <= r.size);
			*address = r.memory;
			return ++sAreas;
		}
	}
	return B_ERROR;
}
static int sAreasDeleted = 0;
extern "C" status_t delete_area(area_id) { sAreasDeleted++; return B_OK; }

struct Handler { int32 irq; interrupt_handler handler; void* data; };
static std::vector<Handler> sHandlers;
extern "C" status_t install_io_interrupt_handler(int32 irq, interrupt_handler h, void* d, uint32)
{
	sHandlers.push_back({irq, h, d});
	return B_OK;
}
extern "C" status_t remove_io_interrupt_handler(int32 irq, interrupt_handler h, void* d)
{
	for (size_t i = 0; i < sHandlers.size(); i++) {
		if (sHandlers[i].irq == irq && sHandlers[i].handler == h && sHandlers[i].data == d) {
			sHandlers.erase(sHandlers.begin() + i);
			return B_OK;
		}
	}
	printf("FAIL removing a handler never installed\n");
	sFailures++;
	return B_ERROR;
}
static int32 raise_irq(int32 irq)
{
	int32 result = B_UNHANDLED_INTERRUPT;
	for (auto& h : sHandlers) {
		if (h.irq == irq) {
			int32 r = h.handler(h.data);
			if (r > result)
				result = r;
		}
	}
	return result;
}

static std::map<sem_id, int32> sSems;
static sem_id sNextSem = 1;
extern "C" sem_id create_sem(int32 count, const char*) { sSems[sNextSem] = count; return sNextSem++; }
extern "C" status_t delete_sem(sem_id id) { assert(sSems.count(id)); sSems.erase(id); return B_OK; }
extern "C" status_t release_sem_etc(sem_id id, int32 count, uint32)
{ assert(sSems.count(id)); sSems[id] += count; return B_OK; }
extern "C" status_t acquire_sem_etc(sem_id id, int32 count, uint32, bigtime_t)
{
	assert(sSems.count(id));
	if (sSems[id] < count)
		return B_TIMED_OUT;
	sSems[id] -= count;
	return B_OK;
}

static int sTrigger = 0, sPolarity = 0, sConfiguredIrq = -1;
void arch_int_configure_io_interrupt(int32 irq, int trigger, int polarity)
{ sConfiguredIrq = irq; sTrigger = trigger; sPolarity = polarity; }

void mutex_lock(mutex* m)
{
	if (m->locked) { printf("FAIL recursive mutex lock (would deadlock)\n"); sFailures++; abort(); }
	m->locked = 1;
}
void mutex_unlock(mutex* m) { assert(m->locked); m->locked = 0; }
void test_spin_lock(spinlock* s)
{
	if (s->v) { printf("FAIL recursive spinlock (would deadlock)\n"); sFailures++; abort(); }
	s->v = 1;
}
void test_spin_unlock(spinlock* s) { assert(s->v); s->v = 0; }

// --- fake ACPI --------------------------------------------------------------

struct FakeDevice {
	std::string hid, uid;
	std::vector<std::pair<uint64, uint32>> memory;
	int32 irq; bool level, low;
	// for an I2C HID device: its GpioInt
	bool gpioInt; std::string source; uint16 pin; bool gpioLevel, gpioLow;
};
static std::map<std::string, FakeDevice> sDevices;

static status_t fake_get_handle(acpi_handle, const char* path, acpi_handle* handle)
{
	auto it = sDevices.find(path);
	if (it == sDevices.end())
		return B_ENTRY_NOT_FOUND;
	*handle = (acpi_handle)&it->second;
	return B_OK;
}

static status_t fake_get_device_info(const char* path, char** hid, char** cidList, size_t,
	char** uid, char** cls)
{
	auto it = sDevices.find(path);
	if (it == sDevices.end())
		return B_ENTRY_NOT_FOUND;
	*hid = strdup(it->second.hid.c_str());
	cidList[0] = NULL;
	if (uid != NULL)
		*uid = it->second.uid.empty() ? NULL : strdup(it->second.uid.c_str());
	if (cls != NULL)
		*cls = NULL;
	return B_OK;
}

static status_t fake_walk_resources(acpi_handle handle, char* method,
	acpi_walk_resources_callback callback, void* context)
{
	FakeDevice* device = (FakeDevice*)handle;
	assert(strcmp(method, "_CRS") == 0);
	ACPI_RESOURCE resource;
	for (auto& m : device->memory) {
		memset(&resource, 0, sizeof(resource));
		resource.Type = ACPI_RESOURCE_TYPE_FIXED_MEMORY32;
		resource.Data.FixedMemory32.Address = (uint32)m.first;
		resource.Data.FixedMemory32.AddressLength = m.second;
		callback(&resource, context);
	}
	if (device->irq >= 0) {
		memset(&resource, 0, sizeof(resource));
		resource.Type = ACPI_RESOURCE_TYPE_EXTENDED_IRQ;
		resource.Data.ExtendedIrq.InterruptCount = 1;
		resource.Data.ExtendedIrq.Interrupts[0] = device->irq;
		resource.Data.ExtendedIrq.Triggering = device->level ? ACPI_LEVEL_SENSITIVE : ACPI_EDGE_SENSITIVE;
		resource.Data.ExtendedIrq.Polarity = device->low ? ACPI_ACTIVE_LOW : ACPI_ACTIVE_HIGH;
		callback(&resource, context);
	}
	if (device->gpioInt) {
		static uint16 pinTable[1];
		pinTable[0] = device->pin;
		memset(&resource, 0, sizeof(resource));
		resource.Type = ACPI_RESOURCE_TYPE_GPIO;
		resource.Data.Gpio.ConnectionType = ACPI_RESOURCE_GPIO_TYPE_INT;
		resource.Data.Gpio.Triggering = device->gpioLevel ? ACPI_LEVEL_SENSITIVE : ACPI_EDGE_SENSITIVE;
		resource.Data.Gpio.Polarity = device->gpioLow ? ACPI_ACTIVE_LOW : ACPI_ACTIVE_HIGH;
		resource.Data.Gpio.PinTableLength = 1;
		resource.Data.Gpio.PinTable = pinTable;
		resource.Data.Gpio.ResourceSource.StringPtr = (char*)device->source.c_str();
		resource.Data.Gpio.ResourceSource.StringLength = device->source.size() + 1;
		callback(&resource, context);
	}
	return B_OK;
}

static acpi_module_info sACPI = { fake_get_handle, fake_get_device_info, fake_walk_resources };

static uint32& reg32(uint64 base, uint32 offset)
{
	for (auto& r : sRegions)
		if (r.base == base)
			return *(uint32*)(r.memory + offset);
	abort();
}

static FakeDevice touchpad(const char* source, uint16 pin, bool level = true, bool low = true)
{
	FakeDevice d = {};
	d.hid = "PNP0C50";
	d.irq = -1;
	d.gpioInt = true;
	d.source = source;
	d.pin = pin;
	d.gpioLevel = level;
	d.gpioLow = low;
	return d;
}

// --- AMD: the old and the new implementation must do the same --------------

static void dump_amd(const char* step, uint64 base, uint32 pin)
{
	printf("AMD %-28s pin reg %08x  master %08x\n", step, reg32(base, pin * 4), reg32(base, 0xfc));
}

static void test_amd()
{
	const uint64 base = 0xfed81500;
	region(base, 0x400);
	FakeDevice controller = {};
	controller.hid = "AMDI0030";
	controller.memory.push_back({base, 0x400});
	controller.irq = 7;
	controller.level = true;
	controller.low = true;
	sDevices["\\_SB.GPIO"] = controller;
	sDevices["\\_SB.I2CD.TPAD"] = touchpad("\\_SB.GPIO", 8);

	// firmware left pin 20's interrupt unmasked and pin 8 in some state
	reg32(base, 20 * 4) = (1 << 11) | (1 << 12) | (1 << 28);
	reg32(base, 8 * 4) = 0x00010000; // high (deasserted for active low)

	GpioInterrupt* interrupt = NULL;
	acpi_handle handle;
	fake_get_handle(NULL, "\\_SB.I2CD.TPAD", &handle);
	status_t status = GpioInterrupt::Create(&sACPI, handle, &interrupt);
	printf("AMD create: %s\n", test_strerror(status));
	CHECK(status == B_OK, "AMD create");
	if (status != B_OK)
		return;
	printf("AMD pin 20 after takeover %08x\n", reg32(base, 20 * 4));
	dump_amd("configured", base, 8);
	printf("AMD asserted (high): %d\n", interrupt->Asserted());
	reg32(base, 8 * 4) &= ~0x00010000;
	printf("AMD asserted (low): %d\n", interrupt->Asserted());
	status = interrupt->Wait(1000);
	printf("AMD wait without interrupt: %s\n", test_strerror(status));
	dump_amd("unmasked", base, 8);

	// the controller raises it: status bit for pins 8-11 is bit 2
	reg32(base, 8 * 4) |= 1 << 28;
	reg32(base, 0x2f8) = 1 << 2;
	int32 r = raise_irq(7);
	printf("AMD interrupt handler: %d\n", r);
	dump_amd("after handler", base, 8);
	status = interrupt->Wait(1000);
	printf("AMD wait after interrupt: %s\n", test_strerror(status));
	interrupt->Mask();
	dump_amd("masked", base, 8);
	reg32(base, 0x2f8) = 0;
	printf("AMD unrelated interrupt: %d\n", raise_irq(7));
	printf("AMD pin %u on %s\n", interrupt->Pin(), interrupt->ControllerName());
	delete interrupt;
	printf("AMD handlers left after delete: %zu, areas deleted %d\n", sHandlers.size(), sAreasDeleted);
	dump_amd("deleted", base, 8);
}

#ifdef TEST_NEW

// --- Intel ------------------------------------------------------------------

enum {
	kPadMode = 0xf << 10, kRxDis = 1 << 9, kTxDis = 1 << 8, kRxState = 1 << 1,
	kRxEvMask = 3 << 25, kRxEvEdge = 1 << 25, kRxInv = 1 << 23, kIoxApic = 1 << 20, kSci = 1 << 19,
};

struct IntelCommunityFake { uint64 base; uint32 padown, lock, hostown, is, ie, padbar, stride; };

static void setup_community(const IntelCommunityFake& c, uint32 revision)
{
	region(c.base, 0x10000);
	reg32(c.base, 0) = revision;
	reg32(c.base, 0xc) = c.padbar;
}

static GpioInterrupt* create(const char* device, status_t expect, const char* what)
{
	GpioInterrupt* interrupt = NULL;
	acpi_handle handle;
	fake_get_handle(NULL, device, &handle);
	status_t status = GpioInterrupt::Create(&sACPI, handle, &interrupt);
	CHECK(status == expect, "%s: create returned %s, expected %s", what,
		test_strerror(status), test_strerror(expect));
	return status == B_OK ? interrupt : NULL;
}

static void test_jasper_lake()
{
	printf("\n=== Jasper Lake (INT34C8): GPP_E pad 208, ACPI pin 295\n");
	// four communities (0, 1, 4, 5), one memory resource each
	IntelCommunityFake c[4];
	for (int i = 0; i < 4; i++)
		c[i] = { 0xfd6a0000ull + i * 0x10000, 0x20, 0x80, 0xc0, 0x100, 0x120, 0x600, 16 };
	FakeDevice controller = {};
	controller.hid = "INT34C8";
	for (int i = 0; i < 4; i++) {
		setup_community(c[i], 0x00950000);
		controller.memory.push_back({c[i].base, 0x10000});
	}
	controller.irq = 14;
	controller.level = true;
	controller.low = true;
	sDevices["\\_SB.GPI0"] = controller;

	// GPP_E: community index 2 (JSL community 4), pads 201..224, GPIOs 288..
	// pin 295 -> pad 208, pad number in community 208 - 195 = 13, register 1, bit 7
	const IntelCommunityFake& e = c[2];
	const uint32 pad = e.padbar + 13 * 16;
	reg32(e.base, pad) = kRxEvEdge | kIoxApic | kRxState;	// GPIO mode, RX+TX on
	reg32(e.base, e.hostown + 4) = 1 << 7;					// host (GPIO driver) mode
	// firmware left interrupts enabled in community 0, register 2
	reg32(c[0].base, c[0].ie + 2 * 4) = 0x00000005;

	sDevices["\\_SB.PCI0.I2C0.TPD0"] = touchpad("\\_SB.GPI0", 295);
	GpioInterrupt* interrupt = create("\\_SB.PCI0.I2C0.TPD0", B_OK, "JSL");
	if (interrupt == NULL)
		return;

	uint32 config = reg32(e.base, pad);
	CHECK((config & kRxEvMask) == 0, "level trigger: %08x", config);
	CHECK((config & kRxInv) != 0, "inverted for active low: %08x", config);
	CHECK((config & kIoxApic) == 0, "direct I/O APIC routing cleared: %08x", config);
	CHECK((config & kTxDis) != 0, "TX disabled: %08x", config);
	CHECK((config & (kPadMode | kRxDis)) == 0, "still a GPIO input: %08x", config);
	CHECK(reg32(c[0].base, c[0].ie + 2 * 4) == 0, "firmware interrupts masked");
	CHECK(interrupt->CanInterrupt(), "can interrupt");
	CHECK(sConfiguredIrq == 14 && sTrigger == B_LEVEL_TRIGGERED && sPolarity == B_LOW_ACTIVE_POLARITY,
		"irq 14 configured level low");
	// no other pad touched
	for (uint32 p = 0; p < 30; p++) {
		if (p != 13)
			CHECK(reg32(e.base, e.padbar + p * 16) == 0, "pad %u untouched", p);
	}

	// level: RX state high = deasserted (active low)
	CHECK(!interrupt->Asserted(), "high reads deasserted");
	reg32(e.base, pad) &= ~kRxState;
	CHECK(interrupt->Asserted(), "low reads asserted");

	CHECK(interrupt->Wait(1000) == B_TIMED_OUT, "no interrupt yet");
	CHECK(reg32(e.base, e.ie + 4) == (1u << 7), "unmasked: IE %08x", reg32(e.base, e.ie + 4));

	// the GPIO controller raises its interrupt for our pad
	reg32(e.base, e.is + 4) = 1 << 7;
	int32 r = raise_irq(14);
	CHECK(r == B_INVOKE_SCHEDULER, "handler wakes the reader: %d", r);
	CHECK(reg32(e.base, e.ie + 4) == 0, "masked by the handler: IE %08x", reg32(e.base, e.ie + 4));
	CHECK(reg32(e.base, e.is + 4) == (1u << 7), "acknowledged (W1C write of the bit)");
	CHECK(interrupt->Wait(1000) == B_OK, "the wait returns");
	CHECK(reg32(e.base, e.ie + 4) == (1u << 7), "unmasked again");

	// a pad nobody waits for, enabled behind our back: masked, not woken
	reg32(c[1].base, c[1].ie + 0) = 1 << 3;
	reg32(c[1].base, c[1].is + 0) = 1 << 3;
	reg32(e.base, e.is + 4) = 0;
	r = raise_irq(14);
	CHECK(r == B_HANDLED_INTERRUPT, "spurious handled: %d", r);
	CHECK(reg32(c[1].base, c[1].ie + 0) == 0, "spurious pad masked");
	reg32(c[1].base, c[1].is + 0) = 0;
	CHECK(raise_irq(14) == B_UNHANDLED_INTERRUPT, "nothing pending: unhandled");

	// a second device on the same pin is refused
	sDevices["\\_SB.PCI0.I2C1.TPD1"] = touchpad("\\_SB.GPI0", 295);
	create("\\_SB.PCI0.I2C1.TPD1", B_BUSY, "same pin twice");

	delete interrupt;
	CHECK(sHandlers.empty(), "handler removed with the last interrupt");
	CHECK(reg32(e.base, e.ie + 4) == 0, "masked when deleted");
}

static void test_jasper_lake_refusals()
{
	printf("\n=== Jasper Lake: pads that must be left alone\n");
	const uint64 base0 = 0xfd6a0000ull, base2 = 0xfd6c0000ull;
	const uint32 padbar = 0x600;

	// a native-function pad (PMODE 1): wrong mapping or not a GPIO, untouched
	uint32 pad = padbar + 14 * 16;	// pin 296
	reg32(base2, pad) = 1 << 10;
	sDevices["\\_SB.T1"] = touchpad("\\_SB.GPI0", 296);
	create("\\_SB.T1", B_BAD_VALUE, "native function pad");
	CHECK(reg32(base2, pad) == (1 << 10), "native pad untouched: %08x", reg32(base2, pad));
	CHECK(sHandlers.empty(), "controller released after a refusal");

	// RX disabled: not an input
	reg32(base2, pad) = kRxDis;
	create("\\_SB.T1", B_BAD_VALUE, "RX disabled pad");
	CHECK(reg32(base2, pad) == kRxDis, "untouched");

	// owned by the ME: PAD_OWN nibble for pad offset 7 in group 1 of community 2
	// group E: padOwnNum 1, bit 7 -> register 0x20 + 4 + 0, nibble 7
	reg32(base2, padbar + 13 * 16) = 0;
	reg32(base2, 0x20 + 4) = 1u << 28;
	sDevices["\\_SB.T2"] = touchpad("\\_SB.GPI0", 295);
	create("\\_SB.T2", B_NOT_ALLOWED, "pad owned by someone else");
	reg32(base2, 0x20 + 4) = 0;

	// ACPI mode (HOSTSW_OWN clear): usable, level watched, the firmware's
	// configuration (SCI routing here) left as it is
	reg32(base2, 0xc0 + 4) = 0;
	reg32(base2, padbar + 13 * 16) = kRxEvEdge | kSci;
	GpioInterrupt* interrupt = create("\\_SB.T2", B_OK, "ACPI mode pad");
	if (interrupt != NULL) {
		CHECK(!interrupt->CanInterrupt(), "ACPI mode: no interrupt");
		CHECK(reg32(base2, padbar + 13 * 16) == (kRxEvEdge | kSci),
			"ACPI mode: not written (%08x)", reg32(base2, padbar + 13 * 16));
		delete interrupt;
	}

	// locked in the right trigger: interrupt usable, nothing written
	reg32(base2, 0xc0 + 4) = 1 << 7;
	reg32(base2, 0x80 + 1 * 8) = 1 << 7;
	reg32(base2, padbar + 13 * 16) = kRxInv | kSci;	// level, inverted, SCI routed
	interrupt = create("\\_SB.T2", B_OK, "locked, right trigger");
	if (interrupt != NULL) {
		CHECK(interrupt->CanInterrupt(), "locked right: interrupt");
		CHECK(reg32(base2, padbar + 13 * 16) == (kRxInv | kSci), "locked: not written");
		delete interrupt;
	}
	// locked in another trigger: level watched
	reg32(base2, padbar + 13 * 16) = kRxEvEdge;
	interrupt = create("\\_SB.T2", B_OK, "locked, other trigger");
	if (interrupt != NULL) {
		CHECK(!interrupt->CanInterrupt(), "locked wrong: no interrupt");
		CHECK(reg32(base2, padbar + 13 * 16) == kRxEvEdge, "locked: not written");
		delete interrupt;
	}
	reg32(base2, 0x80 + 1 * 8) = 0;

	// a GPIO number in no group (the SPI group has none; 330 is past GPP_F's 320..339? no: in it)
	sDevices["\\_SB.T3"] = touchpad("\\_SB.GPI0", 500);
	create("\\_SB.T3", B_BAD_VALUE, "pin in no group");
	(void)base0;
}

static void test_gemini_lake()
{
	printf("\n=== Gemini Lake (INT3453 uid 2, north): pin 37\n");
	const uint64 base = 0xd0c50000ull;
	IntelCommunityFake c = { base, 0x20, 0x80, 0xb0, 0x100, 0x110, 0x500, 8 };
	setup_community(c, 0x00930000);	// before revision 0x94: two registers per pad
	FakeDevice controller = {};
	controller.hid = "INT3453";
	controller.uid = "2";
	controller.memory.push_back({base, 0x10000});
	controller.irq = 14;
	controller.level = true;
	controller.low = true;
	sDevices["\\_SB.GPO1"] = controller;
	// the north community's ACPI pin 37: group 1 (32..63), bit 5, pad 37
	const uint32 pad = c.padbar + 37 * 8;
	reg32(base, pad) = kRxState;
	reg32(base, c.hostown + 4) = 1 << 5;
	sDevices["\\_SB.PCI0.I2C4.ELAN"] = touchpad("\\_SB.GPO1", 37);
	GpioInterrupt* interrupt = create("\\_SB.PCI0.I2C4.ELAN", B_OK, "GLK");
	if (interrupt == NULL)
		return;
	CHECK((reg32(base, pad) & (kRxEvMask | kRxInv)) == kRxInv, "level low: %08x", reg32(base, pad));
	CHECK(interrupt->CanInterrupt(), "can interrupt");
	interrupt->Wait(1000);
	CHECK(reg32(base, c.ie + 4) == (1u << 5), "IE register 1 bit 5: %08x", reg32(base, c.ie + 4));
	reg32(base, c.is + 4) = 1 << 5;
	CHECK(raise_irq(14) == B_INVOKE_SCHEDULER, "interrupt delivered");
	CHECK(interrupt->Wait(1000) == B_OK, "wait returns");
	delete interrupt;

	// the same HID with a UID the table does not know: not supported
	FakeDevice other = controller;
	other.uid = "9";
	other.memory[0].first = base + 0x10000;
	region(base + 0x10000, 0x10000);
	sDevices["\\_SB.GPO9"] = other;
	sDevices["\\_SB.T9"] = touchpad("\\_SB.GPO9", 3);
	create("\\_SB.T9", B_NOT_SUPPORTED, "unknown UID");

	// a community hidden by firmware (REVID all ones)
	reg32(base, 0) = 0xffffffff;
	create("\\_SB.PCI0.I2C4.ELAN", B_DEVICE_NOT_FOUND, "absent community");
	reg32(base, 0) = 0x00930000;

	// a controller nobody knows
	FakeDevice unknown = {};
	unknown.hid = "QCOM0C0C";
	unknown.memory.push_back({0x1000000, 0x1000});
	unknown.irq = 9;
	sDevices["\\_SB.GIO0"] = unknown;
	sDevices["\\_SB.TQ"] = touchpad("\\_SB.GIO0", 5);
	create("\\_SB.TQ", B_NOT_SUPPORTED, "unknown controller");
}


// Real firmware: the ASUS VivoBook E403NA (Apollo Lake N3350, 4 GB, eMMC),
// from its DSDT (linuxhw/ACPI E403NA/E3A7CFB90011). The touchpad ETPD
// (ELAN12xx, _CID PNP0C50) on I2C4 has GpioInt (Level, ActiveLow) pin 0x12
// on \_SB.GPO0, INT3452 _UID 1 ("North"), whose _CRS window is 0x4000 bytes.
// Its _CRS also calls SWON (0x00C50590, One), which sets the pad's
// HOSTSW_OWN bit by the firmware's own arithmetic: register 0x80 of the
// community, bit (0x590 - 0x500) >> 3 = 18 - so the pad's configuration is at
// 0x590, PADBAR 0x500 with 8 bytes a pad. Only those addresses are set here;
// a driver that computed others would fail the checks.
static void test_apollo_lake_e403na()
{
	printf("\n=== Apollo Lake, ASUS VivoBook E403NA firmware: GPO0 (INT3452 uid 1) pin 18\n");
	const uint64 base = 0xd0c70000ull;	// a register file of its own
	region(base, 0x4000);
	reg32(base, 0) = 0x00920000;	// below revision 0x94: two registers a pad
	reg32(base, 0xc) = 0x500;		// PADBAR
	reg32(base, 0x590) = kRxState;	// pad 18: GPIO input, line high (idle)
	reg32(base, 0x80) = 1u << 18;	// HOSTSW_OWN, as SWON leaves it

	FakeDevice controller = {};
	controller.hid = "INT3452";
	controller.uid = "1";
	controller.memory.push_back({base, 0x4000});
	controller.irq = 14;
	controller.level = true;
	controller.low = true;
	sDevices["\\_SB.GPO0"] = controller;
	sDevices["\\_SB.PCI0.I2C4.ETPD"] = touchpad("\\_SB.GPO0", 0x12);

	GpioInterrupt* interrupt = create("\\_SB.PCI0.I2C4.ETPD", B_OK, "E403NA");
	if (interrupt == NULL)
		return;
	CHECK(interrupt->CanInterrupt(), "host mode seen at 0x80 bit 18");
	uint32 config = reg32(base, 0x590);
	CHECK((config & (kRxEvMask | kRxInv)) == kRxInv, "pad at 0x590 set level/low: %08x", config);
	for (uint32 offset = 0x500; offset < 0x500 + 78 * 8; offset += 4) {
		if (offset != 0x590)
			CHECK(reg32(base, offset) == 0, "only the pad at 0x590 written (%#x)", offset);
	}
	CHECK(!interrupt->Asserted(), "idle line reads deasserted");
	reg32(base, 0x590) &= ~kRxState;
	CHECK(interrupt->Asserted(), "touch reads asserted");
	interrupt->Wait(1000);
	CHECK(reg32(base, 0x110) == (1u << 18), "GPI_IE 0x110 bit 18: %08x", reg32(base, 0x110));
	reg32(base, 0x100) = 1u << 18;	// GPI_IS
	CHECK(raise_irq(14) == B_INVOKE_SCHEDULER, "interrupt delivered");
	CHECK(reg32(base, 0x110) == 0, "masked until read");
	CHECK(interrupt->Wait(1000) == B_OK, "reader woken");
	delete interrupt;
}

// Real firmware: the Lenovo IdeaPad 330-15IGM (Gemini Lake), from its DSDT
// (linuxhw/ACPI 08CD7AA84FAC): \_SB.GPO1 is INT3453 _UID 1, "Northwest"
// (GPO0 is _UID 2, "North"), 0x4000 bytes; the touchpad TPD0 (ALPS0001,
// _CID PNP0C50) on I2C7 has GpioInt (Level, ActiveLow) pin 0x12 on GPO1.
static void test_gemini_lake_ideapad()
{
	printf("\n=== Gemini Lake, Lenovo IdeaPad 330-15IGM firmware: GPO1 (INT3453 uid 1) pin 18\n");
	const uint64 base = 0xd0c60000ull;
	region(base, 0x4000);
	reg32(base, 0) = 0x00930000;
	reg32(base, 0xc) = 0x600;
	reg32(base, 0x600 + 18 * 8) = kRxState;
	reg32(base, 0xb0) = 1u << 18;	// GLK HOSTSW_OWN, register 0 (pins 0-31)

	FakeDevice controller = {};
	controller.hid = "INT3453";
	controller.uid = "1";
	controller.memory.push_back({base, 0x4000});
	controller.irq = 14;
	controller.level = true;
	controller.low = true;
	sDevices["\\_SB.GPO1"] = controller;
	sDevices["\\_SB.PCI0.I2C7.TPD0"] = touchpad("\\_SB.GPO1", 0x12);

	GpioInterrupt* interrupt = create("\\_SB.PCI0.I2C7.TPD0", B_OK, "IdeaPad 330");
	if (interrupt == NULL)
		return;
	CHECK(interrupt->CanInterrupt(), "host mode");
	CHECK((reg32(base, 0x600 + 18 * 8) & (kRxEvMask | kRxInv)) == kRxInv, "pad 18 level/low");
	interrupt->Wait(1000);
	CHECK(reg32(base, 0x110) == (1u << 18), "GPI_IE bit 18");
	delete interrupt;
}
#endif

int main()
{
	test_amd();
#ifdef TEST_NEW
	test_jasper_lake();
	test_jasper_lake_refusals();
	test_gemini_lake();
	test_apollo_lake_e403na();
	test_gemini_lake_ideapad();
#endif
	printf("\n%d failure%s\n", sFailures, sFailures == 1 ? "" : "s");
	return sFailures != 0;
}
