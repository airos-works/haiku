/*
 * rmcost: what each nvidia_rm call that NVK makes per memory allocation costs
 * (airos-works/haiku#132). Talks to RM directly, as nvkmd_nvrm does, and
 * prints the mean and minimum time of:
 *  - an ioctl the Haiku driver answers without RM (NV_ESC_CARD_INFO);
 *  - RM's empty control (NV0000_CTRL_CMD_NULL) and the BAR1 query NVK makes
 *    before each mappable allocation;
 *  - video memory and system memory allocations and frees at 4 KiB, 64 KiB
 *    and 2 MiB GPU pages;
 *  - a GPU virtual range, its GPU mapping and unmapping;
 *  - a CPU mapping: open() of the device node, RM's map call, the driver's
 *    NV_HAIKU_MAP (the area clone or physical mapping), close(), and unmap.
 *
 * Build with the air/OS SDK and the RM headers NVK uses (O = the
 * open-gpu-kernel-modules checkout, N = mesa's src/nouveau/vulkan/nvkmd/nvrm):
 *   . /data1/airos/sdk/x86_64/env.sh
 *   ${CROSS}gcc -O2 --sysroot=$SYSROOT -I$N \
 *     -I$O/src/nvidia/arch/nvalloc/common/inc -I$O/src/nvidia/arch/nvalloc/unix/include \
 *     -I$O/src/common/sdk/nvidia/inc -I$O/src/nvidia/inc/kernel \
 *     -o rmcost rmcost.c $N/nvRmApi.c
 * usage: rmcost [repetitions, default 200]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <OS.h>

#include "nvRmApi.h"
#include "nv-haiku.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl50a0.h"
#include "class/cl90f1.h"
#include "ctrl/ctrl0000/ctrl0000base.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"

static NvRmApi rm;
static NvHandle hClient, hDevice, hSubdevice, hVaSpace;
static int reps = 200;

struct cost {
	const char *name;
	bigtime_t sum, min;
	int n;
};

static void add(struct cost *s, bigtime_t t)
{
	s->sum += t;
	if (s->n == 0 || t < s->min)
		s->min = t;
	s->n++;
}

static void print(struct cost *s)
{
	if (s->n == 0)
		printf("  %-46s (failed)\n", s->name);
	else
		printf("  %-46s %8.1f us mean %6lld us min (%d)\n", s->name, (double)s->sum / s->n,
			(long long)s->min, s->n);
}

#define TIME(stat, expr) do { bigtime_t t0_ = system_time(); expr; add(&(stat), system_time() - t0_); } while (0)

static NvU32 page_attr(uint32_t page)
{
	switch (page) {
		case 64 << 10: return DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
		case 2 << 20: return DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _HUGE);
		default: return DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB);
	}
}

static NvU32 alloc_mem(int sys, uint64_t size, uint32_t page, NvHandle *h)
{
	NV_MEMORY_ALLOCATION_PARAMS p = {
		.owner = hClient,
		.type = NVOS32_TYPE_IMAGE,
		.attr = page_attr(page),
		.attr2 = page == (2u << 20) ? DRF_DEF(OS32, _ATTR2, _PAGE_SIZE_HUGE, _2MB) : 0,
		.size = size,
	};
	if (sys) {
		p.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) | DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		p.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM)
			| DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		p.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}
	*h = 0;
	return nvRmApiAlloc(&rm, hDevice, h, sys ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &p);
}

static NvU32 alloc_va(uint64_t size, uint32_t page, NvHandle *h, uint64_t *addr)
{
	NV_MEMORY_ALLOCATION_PARAMS p = {
		.owner = hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.size = size,
		.alignment = page,
		.hVASpace = hVaSpace,
	};
	if (page == (64u << 10))
		p.attr |= DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
	else if (page == (2u << 20)) {
		p.attr |= DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _HUGE);
		p.attr2 |= DRF_DEF(OS32, _ATTR2, _PAGE_SIZE_HUGE, _2MB);
	}
	*h = 0;
	NvU32 st = nvRmApiAlloc(&rm, hDevice, h, NV50_MEMORY_VIRTUAL, &p);
	*addr = p.offset;
	return st;
}

/* allocation, GPU range, GPU mapping and the frees, as nvkmd does them */
static void gpu_alloc_costs(int sys, uint64_t size, uint32_t page)
{
	char names[7][64];
	const char *what = sys ? "system" : "video";
	snprintf(names[0], 64, "%s %llu KiB, %u KiB pages: alloc", what,
		(unsigned long long)(size >> 10), page >> 10);
	snprintf(names[1], 64, "  virtual range alloc");
	snprintf(names[2], 64, "  GPU map");
	snprintf(names[3], 64, "  GPU unmap");
	snprintf(names[4], 64, "  virtual range free");
	snprintf(names[5], 64, "  free");
	snprintf(names[6], 64, "  free (range freed with mapping)");
	struct cost s[7] = {{names[0]}, {names[1]}, {names[2]}, {names[3]}, {names[4]},
		{names[5]}, {names[6]}};
	for (int i = 0; i < reps; i++) {
		NvHandle hMem, hVirt;
		uint64_t addr;
		NvU32 st;
		TIME(s[0], st = alloc_mem(sys, size, page, &hMem));
		if (st != NV_OK) {
			printf("  %s: %#x\n", names[0], st);
			break;
		}
		TIME(s[1], st = alloc_va(size, page, &hVirt, &addr));
		if (st != NV_OK) {
			printf("  virtual: %#x\n", st);
			nvRmApiFree(&rm, hMem);
			break;
		}
		NvU64 dmaOffset = 0;
		NvU32 flags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
			| (sys ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
				: DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
		TIME(s[2], st = nvRmApiMapMemoryDma(&rm, hDevice, hVirt, hMem, 0, size, flags,
			&dmaOffset));
		if (st != NV_OK)
			printf("  map: %#x\n", st);
		if ((i & 1) == 0) {
			/* nvkmd frees the range, which unmaps it, then the memory */
			TIME(s[4], nvRmApiFree(&rm, hVirt));
			TIME(s[6], nvRmApiFree(&rm, hMem));
		} else {
			TIME(s[3], nvRmApiUnmapMemoryDma(&rm, hDevice, hVirt, hMem, 0, dmaOffset, size));
			nvRmApiFree(&rm, hVirt);
			TIME(s[5], nvRmApiFree(&rm, hMem));
		}
	}
	for (int i = 0; i < 7; i++)
		print(&s[i]);
}

/* the CPU mapping path of nvRmApiMapMemory()/nvRmApiUnmapMemory() */
static void cpu_map_costs(int sys, uint64_t size, uint32_t page)
{
	char head[64];
	snprintf(head, sizeof(head), "%s %llu KiB CPU mapping: open()", sys ? "system" : "video",
		(unsigned long long)(size >> 10));
	struct cost s[6] = {{head}, {"  RM map (NV_ESC_RM_MAP_MEMORY)"},
		{"  NV_HAIKU_MAP (area)"}, {"  close()"}, {"  delete_area()"},
		{"  RM unmap (NV_ESC_RM_UNMAP_MEMORY)"}};
	const char *node = sys ? NVRM_CTL_NODE_NAME : rm.nodeName;
	NvHandle hMem;
	NvU32 st = alloc_mem(sys, size, page, &hMem);
	if (st != NV_OK) {
		printf("  %s: alloc %#x\n", head, st);
		return;
	}
	for (int i = 0; i < reps; i++) {
		int fd;
		TIME(s[0], fd = open(node, O_RDWR | O_CLOEXEC));
		nv_ioctl_nvos33_parameters_with_fd p = {
			.params = {.hClient = hClient, .hDevice = hSubdevice, .hMemory = hMem,
				.offset = 0, .length = size},
			.fd = fd,
		};
		int ret;
		TIME(s[1], ret = ioctl(rm.fd, NV_HAIKU_BASE + NV_ESC_RM_MAP_MEMORY, &p, sizeof(p)));
		if (ret < 0 || p.params.status != NV_OK) {
			printf("  RM map: %d %#x\n", ret, p.params.status);
			close(fd);
			break;
		}
		nv_haiku_map_params mp = {.addressSpec = B_ANY_ADDRESS,
			.protection = B_READ_AREA | B_WRITE_AREA | (sys ? B_CLONEABLE_AREA : 0)};
		strcpy(mp.name, "rmcost");
		TIME(s[2], ret = ioctl(fd, NV_HAIKU_BASE + NV_HAIKU_MAP, &mp, sizeof(mp)));
		TIME(s[3], close(fd));
		if (ret < 0) {
			printf("  NV_HAIKU_MAP: %d\n", ret);
			break;
		}
		TIME(s[4], delete_area(ret));
		NVOS34_PARAMETERS up = {.hClient = hClient, .hDevice = hSubdevice, .hMemory = hMem,
			.pLinearAddress = p.params.pLinearAddress};
		TIME(s[5], ioctl(rm.fd, NV_HAIKU_BASE + NV_ESC_RM_UNMAP_MEMORY, &up, sizeof(up)));
	}
	nvRmApiFree(&rm, hMem);
	for (int i = 0; i < 6; i++)
		print(&s[i]);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		reps = atoi(argv[1]);
	setvbuf(stdout, NULL, _IOLBF, 0);
	rm.fd = open(NVRM_CTL_NODE_NAME, O_RDWR | O_CLOEXEC);
	if (rm.fd < 0) {
		perror("open " NVRM_CTL_NODE_NAME);
		return 1;
	}
	nv_ioctl_card_info_t ci[8];
	if (nvRmApiCardInfo(&rm, ci, sizeof(ci)) != NV_OK || !ci[0].valid) {
		fprintf(stderr, "no GPU\n");
		return 1;
	}
	static char devName[64];
	snprintf(devName, sizeof(devName), NVRM_ACTUAL_NODE_NAME, ci[0].minor_number);
	int devFd = open(devName, O_RDWR | O_CLOEXEC);
	rm.nodeName = devName;
	if (nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL) != NV_OK)
		return 1;
	rm.hClient = hClient;
	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = {.gpuId = ci[0].gpu_id};
	nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
	NV0080_ALLOC_PARAMETERS ap0080 = {.deviceId = id.deviceInstance, .hClientShare = hClient};
	NV2080_ALLOC_PARAMETERS ap2080 = {.subDeviceId = id.subDeviceInstance};
	if (nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &ap0080) != NV_OK
		|| nvRmApiAlloc(&rm, hDevice, &hSubdevice, NV20_SUBDEVICE_0, &ap2080) != NV_OK) {
		fprintf(stderr, "device/subdevice refused\n");
		return 1;
	}
	NV_VASPACE_ALLOCATION_PARAMETERS vap = {
		.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
		.vaSize = 1ull << 39,
	};
	if (nvRmApiAlloc(&rm, hDevice, &hVaSpace, FERMI_VASPACE_A, &vap) != NV_OK) {
		fprintf(stderr, "VA space refused\n");
		return 1;
	}

	printf("rmcost: %d repetitions each\n", reps);
	{
		struct cost card = {"ioctl answered by the driver (CARD_INFO)"};
		struct cost null = {"RM control NV0000_CTRL_CMD_NULL"};
		struct cost fb = {"RM control FB_GET_INFO_V2 (BAR1 free)"};
		struct cost oc = {"open()+close() of the GPU node"};
		for (int i = 0; i < reps * 5; i++) {
			TIME(card, nvRmApiCardInfo(&rm, ci, sizeof(ci)));
			TIME(null, nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_NULL, NULL, 0));
			NV2080_CTRL_FB_GET_INFO_V2_PARAMS info = {.fbInfoListSize = 1,
				.fbInfoList = {{.index = NV2080_CTRL_FB_INFO_INDEX_BAR1_AVAIL_SIZE}}};
			TIME(fb, nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &info,
				sizeof(info)));
			if (i < reps) {
				TIME(oc, close(open(devName, O_RDWR | O_CLOEXEC)));
			}
		}
		print(&card);
		print(&null);
		print(&fb);
		print(&oc);
	}

	gpu_alloc_costs(0, 64 << 10, 4096);
	gpu_alloc_costs(0, 64 << 10, 64 << 10);
	gpu_alloc_costs(0, 4 << 20, 4096);
	gpu_alloc_costs(0, 4 << 20, 64 << 10);
	gpu_alloc_costs(0, 4 << 20, 2 << 20);
	gpu_alloc_costs(1, 64 << 10, 4096);
	gpu_alloc_costs(1, 1 << 20, 4096);
	cpu_map_costs(1, 64 << 10, 4096);
	cpu_map_costs(1, 1 << 20, 4096);
	cpu_map_costs(0, 1 << 20, 64 << 10);

	nvRmApiFree(&rm, hVaSpace);
	nvRmApiFree(&rm, hSubdevice);
	nvRmApiFree(&rm, hDevice);
	nvRmApiFree(&rm, hClient);
	close(devFd);
	close(rm.fd);
	return 0;
}
