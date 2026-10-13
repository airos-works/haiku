// xhciregs: read-only dump of every xHCI controller's port and power
// management registers through /dev/misc/poke - which ports have a device,
// its speed and link state, USB 2 LPM (PORTPMSC/PORTHLPMC), the Supported
// Protocol capabilities, and interrupt moderation.
#include <Drivers.h>
#include <OS.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// from headers/private/drivers/poke.h
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

static volatile uint8* sBase;
static int sPoke;
static uint32 r32(uint32 offset) { return *(volatile uint32*)(sBase + offset); }

// Maps one page of physical memory and copies \a length bytes from it.
static bool
read_physical(phys_addr_t address, void* buffer, size_t length)
{
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	mem_map_args map = { POKE_SIGNATURE, -1, "xhcictx", page, B_PAGE_SIZE * 2,
		B_ANY_ADDRESS, B_READ_AREA, NULL };
	if (ioctl(sPoke, POKE_MAP_MEMORY, &map, sizeof(map)) < 0 || map.area < 0)
		return false;
	memcpy(buffer, (uint8*)map.address + (address - page), length);
	ioctl(sPoke, POKE_UNMAP_MEMORY, &map, sizeof(map));
	return true;
}

int
main()
{
	int fd = open("/dev/misc/poke", O_RDWR);
	if (fd < 0) { perror("poke"); return 1; }
	sPoke = fd;
	for (int index = 0; index < 256; index++) {
		pci_info info;
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(fd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		if (info.class_base != 0x0c || info.class_sub != 0x03
			|| info.class_api != 0x30)
			continue;
		phys_addr_t physical = info.u.h0.base_registers[0];
		if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
			physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
		mem_map_args map = { POKE_SIGNATURE, -1, "xhciregs", physical,
			info.u.h0.base_register_sizes[0], B_ANY_ADDRESS,
			B_READ_AREA, NULL };
		if (ioctl(fd, POKE_MAP_MEMORY, &map, sizeof(map)) < 0 || map.area < 0) {
			printf("%04x:%04x: cannot map\n", info.vendor_id, info.device_id);
			continue;
		}
		sBase = (volatile uint8*)map.address;
		uint32 capLength = r32(0) & 0xff;
		uint32 hcs1 = r32(4), hcc1 = r32(0x10), rtsoff = r32(0x18) & ~0x1f;
		uint32 ports = hcs1 >> 24;
		printf("== %04x:%04x at %02x:%02x.%x  ports %u  IMAN %#x IMOD %#x\n",
			info.vendor_id, info.device_id, info.bus, info.device,
			info.function, ports, r32(rtsoff + 0x20), r32(rtsoff + 0x24));
		{
			uint32 op = capLength;
			printf("   USBCMD %#x USBSTS %#x CRCR %#x%08x DNCTRL %#x CONFIG %#x"
				" ERSTSZ %u ERDP %#x%08x HCSPARAMS2 %#x HCCPARAMS2 %#x\n",
				r32(op), r32(op + 4), r32(op + 0x1c), r32(op + 0x18),
				r32(op + 0x14), r32(op + 0x38), r32(rtsoff + 0x28),
				r32(rtsoff + 0x3c), r32(rtsoff + 0x38), r32(8), r32(0x1c));
		}
		for (uint32 x = (hcc1 >> 16) << 2; x != 0;) {
			uint32 d0 = r32(x);
			if ((d0 & 0xff) == 2) {
				uint32 d2 = r32(x + 8);
				printf("   protocol USB %x.%02x ports %u-%u psic %u bits %#05x"
					"%s%s%s%s\n", d0 >> 24, (d0 >> 16) & 0xff, d2 & 0xff,
					(d2 & 0xff) + ((d2 >> 8) & 0xff) - 1, d2 >> 28,
					(d2 >> 16) & 0xfff,
					(d2 & (1 << 17)) ? " HSO" : "", (d2 & (1 << 18)) ? " IHI" : "",
					(d2 & (1 << 19)) ? " HLC" : "", (d2 & (1 << 20)) ? " BLC" : "");
			}
			uint32 next = (d0 >> 8) & 0xff;
			x = next ? x + (next << 2) : 0;
		}
		for (uint32 port = 1; port <= ports; port++) {
			uint32 base = capLength + 0x400 + 0x10 * (port - 1);
			uint32 sc = r32(base);
			if ((sc & 1) == 0) {
				if (getenv("XHCIREGS_ALL") != NULL) {
					printf("   port %2u: PORTSC %#010x (empty: power %u link %u)\n",
						port, sc, (sc >> 9) & 1, (sc >> 5) & 0xf);
				}
				continue;
			}
			printf("   port %2u: PORTSC %#010x speed %u link %u  PORTPMSC %#010x"
				" (HLE %u, L1S %u, BESL %u)  PORTLI %#x  PORTHLPMC %#x\n",
				port, sc, (sc >> 10) & 0xf, (sc >> 5) & 0xf, r32(base + 4),
				(r32(base + 4) >> 16) & 1, r32(base + 4) & 7,
				(r32(base + 4) >> 4) & 0xf, r32(base + 8), r32(base + 12));
		}
		// the controller's own copy of every device's contexts
		uint32 slots = hcs1 & 0xff;
		size_t contextSize = (hcc1 & (1 << 2)) ? 64 : 32;
		phys_addr_t dcbaa = r32(capLength + 0x30) & ~0x3fULL;
		dcbaa |= (phys_addr_t)r32(capLength + 0x34) << 32;
		for (uint32 slot = 1; slot <= slots; slot++) {
			uint64 contextAddress = 0;
			if (!read_physical(dcbaa + slot * 8, &contextAddress, 8)
				|| contextAddress == 0)
				continue;
			uint32 context[32 * 16];
			if (!read_physical(contextAddress, context, contextSize * 4))
				continue;
			uint32* sc = context;
			printf("   slot %u: route %#x speed %u entries %u port %u"
				" state %u address %u\n", slot, sc[0] & 0xfffff,
				(sc[0] >> 20) & 0xf, sc[0] >> 27, (sc[1] >> 16) & 0xff,
				sc[3] >> 27, sc[3] & 0xff);
			{
				// endpoint 0's ring: the dequeue pointer is in its first 256
				// bytes, which start a page (the device's rings are one area)
				uint32* ep0 = context + contextSize / 4;
				uint64 dequeue = ((uint64)ep0[3] << 32 | ep0[2]) & ~0xfULL;
				uint32 ring[16 * 4];
				if (read_physical(dequeue & ~0xfffULL, ring, sizeof(ring))) {
					printf("      ep0 ring (dequeue at slot %u):", (unsigned)((dequeue & 0xfff) / 16));
					for (int t = 0; t < 16; t++)
						printf(" %u%s", (ring[t * 4 + 3] >> 10) & 0x3f,
							(ring[t * 4 + 3] & 1) ? "" : "-");
					printf("\n");
				}
			}
			for (uint32 e = 1; e <= (sc[0] >> 27) && e < 4; e++) {
				uint32* ep = context + e * contextSize / 4;
				printf("      ep ctx %u: state %u interval %u mult %u |"
					" cerr %u type %u maxburst %u mps %u | avgtrb %u"
					" maxesit %u\n", e, ep[0] & 7, (ep[0] >> 16) & 0xff,
					(ep[0] >> 8) & 3, (ep[1] >> 1) & 3, (ep[1] >> 3) & 7,
					(ep[1] >> 8) & 0xff, ep[1] >> 16, ep[4] & 0xffff,
					ep[4] >> 16);
			}
		}
		ioctl(fd, POKE_UNMAP_MEMORY, &map, sizeof(map));
	}
	close(fd);
	return 0;
}
