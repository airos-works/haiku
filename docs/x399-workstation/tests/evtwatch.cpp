// evtwatch <vendor:device> <usb device> [count [length [descriptor]]]: time control transfers to
// one USB device while a second thread watches the controller's event ring
// in memory (read only, through /dev/misc/poke). Each transfer is printed
// with the moment the controller wrote its event, so the time the controller
// takes is told apart from the time Haiku takes to notice and complete it.
// MFINDEX (the controller's 125 us microframe counter) is sampled with every
// timestamp to show whether work lines up with frame boundaries.
// EVTWATCH_KICK="<slot> <dci> <period us>" in the environment also rings that
// endpoint's doorbell every period while the transfers run - harmless for an
// endpoint with nothing queued, and it shows whether the controller only
// returns to a waiting endpoint once a frame unless it is told to.
#include <USBKit.h>
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
static uint32 sRuntime;
static volatile uint32* sEvents;
static uint32 sEventCount;
static volatile bool sStop;
static uint32 sKickSlot, sKickTarget, sKickPeriod;
static int32 sKicks;

struct Sample {
	bigtime_t	time;
	uint32		mfindex;
	uint32		index;
	uint32		trb[4];
};
static Sample sSamples[4096];
static int32 sSampleCount;

static uint32 r32(uint32 offset) { return *(volatile uint32*)(sRegs + offset); }
static uint32 mfindex() { return r32(sRuntime) & 0x3fff; }

static void*
map(phys_addr_t address, size_t size, area_id* area)
{
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t length = ((address - page) + size + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	mem_map_args args = { POKE_SIGNATURE, -1, "evtwatch", page, length,
		B_ANY_ADDRESS, B_READ_AREA | (sKickPeriod != 0 ? B_WRITE_AREA : 0),
		NULL };
	if (ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args)) < 0 || args.area < 0)
		return NULL;
	*area = args.area;
	return (uint8*)args.address + (address - page);
}

static status_t
watcher(void*)
{
	uint32 last[208 * 4];
	for (uint32 i = 0; i < sEventCount * 4; i++)
		last[i] = sEvents[i];
	while (!sStop) {
		for (uint32 i = 0; i < sEventCount; i++) {
			uint32 flags = sEvents[i * 4 + 3];
			if (flags == last[i * 4 + 3])
				continue;
			bigtime_t now = system_time();
			uint32 frame = mfindex();
			for (int w = 0; w < 4; w++)
				last[i * 4 + w] = sEvents[i * 4 + w];
			if (sSampleCount < (int32)B_COUNT_OF(sSamples)) {
				Sample& sample = sSamples[sSampleCount++];
				sample.time = now;
				sample.mfindex = frame;
				sample.index = i;
				memcpy(sample.trb, &last[i * 4], 16);
			}
		}
	}
	return B_OK;
}

static status_t
kicker(void*)
{
	volatile uint32* doorbell = (volatile uint32*)(sRegs
		+ (*(volatile uint32*)(sRegs + 0x14) & ~0x3) + 4 * sKickSlot);
	while (!sStop) {
		*doorbell = sKickTarget;
		sKicks++;
		bigtime_t until = system_time() + sKickPeriod;
		while (system_time() < until && !sStop)
			;
	}
	return B_OK;
}

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <vendor:device> <usb device> [count]"
			" [length]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	int count = argc > 3 ? atoi(argv[3]) : 5;
	int length = argc > 4 ? atoi(argv[4]) : 18;
	// a fifth argument picks the descriptor: 1 device (default), 2 config
	uint16 descriptor = (argc > 5 ? atoi(argv[5]) : 1) << 8;

	const char* kick = getenv("EVTWATCH_KICK");
	if (kick != NULL)
		sscanf(kick, "%u %u %u", &sKickSlot, &sKickTarget, &sKickPeriod);

	sPoke = open("/dev/misc/poke", O_RDWR);
	if (sPoke < 0) {
		perror("poke");
		return 1;
	}
	pci_info info;
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = info.vendor_id == vendor && info.device_id == deviceID;
	}
	if (!found) {
		fprintf(stderr, "no controller %04x:%04x\n", vendor, deviceID);
		return 1;
	}
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	area_id regsArea;
	sRegs = (volatile uint8*)map(physical, info.u.h0.base_register_sizes[0],
		&regsArea);
	if (sRegs == NULL) {
		fprintf(stderr, "cannot map registers\n");
		return 1;
	}
	sRuntime = r32(0x18) & ~0x1f;
	phys_addr_t erst = r32(sRuntime + 0x30) | (phys_addr_t)r32(sRuntime + 0x34) << 32;
	area_id erstArea;
	volatile uint32* segment = (volatile uint32*)map(erst, 16, &erstArea);
	phys_addr_t ring = segment[0] | (phys_addr_t)segment[1] << 32;
	sEventCount = segment[2] & 0xffff;
	if (sEventCount > 208)
		sEventCount = 208;
	area_id ringArea;
	sEvents = (volatile uint32*)map(ring, sEventCount * 16, &ringArea);
	printf("event ring %#llx, %u entries, IMOD %#x, MFINDEX %u\n",
		(unsigned long long)ring, sEventCount, r32(sRuntime + 0x24), mfindex());

	BUSBDevice device(argv[2]);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", argv[2]);
		return 1;
	}

	thread_id thread = spawn_thread(watcher, "watcher", B_REAL_TIME_PRIORITY,
		NULL);
	resume_thread(thread);
	thread_id kickThread = -1;
	if (sKickPeriod != 0) {
		kickThread = spawn_thread(kicker, "kicker", B_REAL_TIME_PRIORITY, NULL);
		resume_thread(kickThread);
	}
	snooze(20000);

	struct Call { bigtime_t start, end; uint32 startFrame, endFrame; ssize_t got; };
	Call calls[256];
	if (count > 256)
		count = 256;
	uint8 buffer[4096];
	for (int i = 0; i < count; i++) {
		snooze(3000);
		calls[i].start = system_time();
		calls[i].startFrame = mfindex();
		calls[i].got = device.ControlTransfer(0x80, 6, descriptor, 0, length,
			buffer);
		calls[i].end = system_time();
		calls[i].endFrame = mfindex();
	}
	snooze(20000);
	sStop = true;
	status_t result;
	wait_for_thread(thread, &result);
	if (kickThread >= 0) {
		wait_for_thread(kickThread, &result);
		printf("rang slot %u target %u %d times\n", sKickSlot, sKickTarget,
			sKicks);
	}

	bigtime_t base = calls[0].start;
	int s = 0;
	for (int i = 0; i < count; i++) {
		printf("call %d: start %7lld us (uframe %5u)  end %7lld us (uframe %5u)"
			"  took %lld us, %zd bytes\n", i, calls[i].start - base,
			calls[i].startFrame, calls[i].end - base, calls[i].endFrame,
			calls[i].end - calls[i].start, calls[i].got);
		for (; s < sSampleCount && sSamples[s].time <= calls[i].end + 2000; s++) {
			const Sample& e = sSamples[s];
			printf("    event[%3u] at %7lld us (+%5lld, uframe %5u): type %2u"
				" slot %u ep %u code %u len %u ptr %#llx%s\n", e.index,
				e.time - base, e.time - calls[i].start, e.mfindex,
				(e.trb[3] >> 10) & 0x3f, e.trb[3] >> 24, (e.trb[3] >> 16) & 0x1f,
				e.trb[2] >> 24, e.trb[2] & 0xffffff,
				(unsigned long long)e.trb[0] | (unsigned long long)e.trb[1] << 32,
				(e.trb[3] & 4) ? " ED" : "");
		}
	}
	return 0;
}
