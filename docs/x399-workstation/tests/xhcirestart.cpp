// xhcirestart <vendor:device> [ms] [noop]: reset and run again an xHCI
// controller that has stopped (a Host System Error), reusing the DCBAA, event
// ring and command ring Haiku's xhci driver gave it, with interrupts off so
// that the driver does not get involved; then log USBSTS, USBCMD and every
// PORTSC change for [ms] milliseconds (default 500). With "noop[=ms]", queue
// a No Op command that long after the start (default 50 ms) and report
// whether it completes.
// Only for a controller the driver has given up on: this takes it over.
#include <Drivers.h>
#include <OS.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	POKE_GET_NTH_PCI_INFO = B_DEVICE_OP_CODES_END + 7,
	POKE_MAP_MEMORY = B_DEVICE_OP_CODES_END + 9,
	POKE_UNMAP_MEMORY
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; area_id area; const char* name;
	phys_addr_t physical_address; size_t size; uint32 flags; uint32 protection;
	void* address; } mem_map_args;

static int sPoke;
static volatile uint8* sRegs;
static uint32 sOp, sRt, sDb;

static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }

static volatile uint32*
map(phys_addr_t address, size_t size)
{
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t length = ((address - page) + size + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	mem_map_args args = { POKE_SIGNATURE, -1, "xhcirestart", page, length,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, NULL };
	if (ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args)) < 0 || args.area < 0)
		return NULL;
	return (volatile uint32*)((uint8*)args.address + (address - page));
}

static bool
wait(uint32 reg, uint32 mask, uint32 value, const char* what)
{
	bigtime_t until = system_time() + 1000000;
	while (system_time() < until) {
		if ((rd(reg) & mask) == value)
			return true;
		snooze(100);
	}
	printf("timed out waiting for %s (%#x)\n", what, rd(reg));
	return false;
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <vendor:device> [ms] [noop]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	bigtime_t duration = (argc > 2 ? atoi(argv[2]) : 500) * 1000LL;
	bool noop = argc > 3 && strncmp(argv[3], "noop", 4) == 0;
	double noopAt = argc > 3 && argv[3][4] == '=' ? atof(argv[3] + 5) : 50;

	sPoke = open("/dev/misc/poke", O_RDWR);
	pci_info info;
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = info.vendor_id == vendor && info.device_id == deviceID;
	}
	if (!found)
		return 1;
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	sRegs = (volatile uint8*)map(physical, 0x10000);
	sOp = rd(0) & 0xff;
	sRt = rd(0x18) & ~0x1f;
	sDb = rd(0x14) & ~0x3;
	uint32 ports = rd(4) >> 24;

	uint32 dcbaaLow = rd(sOp + 0x30), dcbaaHigh = rd(sOp + 0x34);
	uint32 config = rd(sOp + 0x38);
	uint32 erstsz = rd(sRt + 0x28);
	uint32 erstbaLow = rd(sRt + 0x30), erstbaHigh = rd(sRt + 0x34);
	phys_addr_t erst = erstbaLow | (phys_addr_t)erstbaHigh << 32;
	volatile uint32* segment = map(erst, 16);
	phys_addr_t eventRing = segment[0] | (phys_addr_t)segment[1] << 32;
	uint32 events = segment[2] & 0xffff;
	phys_addr_t commandRing = eventRing + events * 16;
	volatile uint32* ring = map(eventRing, (events + 16) * 16);
	volatile uint32* commands = ring + events * 4;
	printf("as found: USBCMD %#x USBSTS %#x, DCBAA %#x%08x, ERST %#llx,"
		" events %#llx (%u), commands %#llx\n", rd(sOp), rd(sOp + 4),
		dcbaaHigh, dcbaaLow, (unsigned long long)erst,
		(unsigned long long)eventRing, events,
		(unsigned long long)commandRing);

	wr(sOp, 0);
	wait(sOp + 4, 1, 1, "halt");
	wr(sOp, 2);
	wait(sOp, 2, 0, "reset");
	wait(sOp + 4, 1 << 11, 0, "controller not ready");

	memset((void*)ring, 0, (events + 16) * 16);
	commands[15 * 4 + 0] = (uint32)commandRing;
	commands[15 * 4 + 1] = (uint32)(commandRing >> 32);
	commands[15 * 4 + 3] = (6 << 10) | 2 | 1;	// Link, Toggle Cycle, cycle

	wr(sOp + 0x38, config);
	wr(sOp + 0x30, dcbaaLow);
	wr(sOp + 0x34, dcbaaHigh);
	wr(sRt + 0x28, erstsz);
	wr(sRt + 0x38, (uint32)eventRing);
	wr(sRt + 0x3c, (uint32)(eventRing >> 32));
	wr(sRt + 0x30, erstbaLow);
	wr(sRt + 0x34, erstbaHigh);
	wr(sOp + 0x18, (uint32)commandRing | 1);
	wr(sOp + 0x1c, (uint32)(commandRing >> 32));
	wr(sRt + 0x20, 0);	// IMAN: interrupts off

	uint32 lastStatus = rd(sOp + 4);
	uint32 lastPort[16];
	for (uint32 p = 0; p < ports && p < 16; p++)
		lastPort[p] = rd(sOp + 0x400 + 0x10 * p);
	printf("before run: USBSTS %#x\n", lastStatus);

	bigtime_t start = system_time();
	wr(sOp, 1);	// run, interrupts off
	bool queued = false;
	uint32 seenEvents = 0;
	while (system_time() - start < duration) {
		double ms = (system_time() - start) / 1000.0;
		uint32 status = rd(sOp + 4);
		if (status != lastStatus) {
			printf("%8.3f ms USBSTS %#x -> %#x (USBCMD %#x)\n", ms, lastStatus,
				status, rd(sOp));
			lastStatus = status;
		}
		for (uint32 p = 0; p < ports && p < 16; p++) {
			uint32 sc = rd(sOp + 0x400 + 0x10 * p);
			if (sc != lastPort[p]) {
				printf("%8.3f ms port %u: %#x -> %#x\n", ms, p + 1, lastPort[p],
					sc);
				lastPort[p] = sc;
			}
		}
		while (seenEvents < events && (ring[seenEvents * 4 + 3] & 1) != 0) {
			volatile uint32* t = ring + seenEvents * 4;
			printf("%8.3f ms event[%u]: type %u code %u slot %u ptr %#x%08x\n",
				ms, seenEvents, (t[3] >> 10) & 0x3f, t[2] >> 24, t[3] >> 24,
				t[1], t[0]);
			seenEvents++;
		}
		if (noop && !queued && ms >= noopAt) {
			commands[0] = commands[1] = commands[2] = 0;
			commands[3] = (23 << 10) | 1;	// No Op command, cycle 1
			wr(sDb, 0);
			queued = true;
			printf("%8.3f ms queued a No Op command\n", ms);
		}
		snooze(20);
	}
	printf("end: USBCMD %#x USBSTS %#x\n", rd(sOp), rd(sOp + 4));
	return 0;
}
