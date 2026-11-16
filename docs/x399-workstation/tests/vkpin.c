/*
 * vkpin: how much wired kernel memory a Vulkan memory allocation costs
 * (haiku#105). For each memory type, allocates COUNT blocks of SIZE bytes,
 * then counts the kernel team's "DMA Buffer" areas (one per nvidia_rm
 * system-memory allocation) before and after, and prints the extra areas and
 * bytes per allocation.
 *
 * Build with the air/OS SDK (no Vulkan SDK needed, only Mesa's headers):
 *   . /data1/airos/sdk/x86_64/env.sh
 *   ${CROSS}gcc -O2 --sysroot=$SYSROOT -I<mesa>/include -o vkpin vkpin.c
 * usage: vkpin [size in KiB, default 64] [count, default 32]
 * VKPIN_HOLD=<s> keeps the allocations for s seconds before freeing them.
 * VKPIN_TYPE=<n> tests memory type n only. With a large count, the
 * allocations stop at the first failure, so "vkpin 65536 2000" with
 * VKPIN_TYPE set to the system memory type fills RAM until the driver refuses.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

static PFN_vkGetInstanceProcAddr gipa;
#define F(name) static PFN_##name name
F(vkCreateInstance); F(vkEnumeratePhysicalDevices);
F(vkGetPhysicalDeviceMemoryProperties); F(vkGetPhysicalDeviceQueueFamilyProperties);
F(vkCreateDevice); F(vkAllocateMemory); F(vkFreeMemory); F(vkMapMemory);
F(vkDestroyDevice); F(vkDestroyInstance);
#undef F

static void
count_dma_areas(const char* name, int* count, uint64_t* bytes)
{
	area_info info;
	ssize_t cookie = 0;
	*count = 0;
	*bytes = 0;
	while (get_next_area_info(1, &cookie, &info) == B_OK) {
		if (strncmp(info.name, name, strlen(name)) == 0) {
			(*count)++;
			*bytes += info.size;
		}
	}
}

int
main(int argc, char** argv)
{
	uint64_t size = (argc > 1 ? strtoull(argv[1], NULL, 0) : 64) * 1024;
	int count = argc > 2 ? atoi(argv[2]) : 32;
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char* areaName = getenv("VKPIN_AREA") ? getenv("VKPIN_AREA") : "DMA Buffer";

	void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (lib == NULL) { fprintf(stderr, "no libvulkan.so.1\n"); return 1; }
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define L(name) name = (PFN_##name)gipa(inst, #name)
	VkInstance inst = NULL;
	L(vkCreateInstance);
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkpin", 1,
		NULL, 0, VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
	if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "instance\n"); return 1; }
	L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceMemoryProperties);
	L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkCreateDevice);
	L(vkDestroyInstance);
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || n == 0) { fprintf(stderr, "no device\n"); return 1; }
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci };
	VkDevice dev;
	if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "device\n"); return 1; }
#undef L
#define L(name) name = (PFN_##name)gipa(inst, #name)
	L(vkAllocateMemory); L(vkFreeMemory); L(vkMapMemory); L(vkDestroyDevice);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkDeviceMemory* mem = calloc(count, sizeof(*mem));

	printf("%d x %llu KiB per memory type, counting kernel areas whose names start with \"%s\"\n",
		count, (unsigned long long)size / 1024, areaName);
	const char* onlyType = getenv("VKPIN_TYPE");
	for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
		if (onlyType != NULL && (uint32_t)atoi(onlyType) != t)
			continue;
		bigtime_t start = system_time();
		VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
		int before, after, freed;
		uint64_t bBefore, bAfter, bFreed;
		count_dma_areas(areaName, &before, &bBefore);
		int made = 0;
		for (; made < count; made++) {
			VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, t };
			if (vkAllocateMemory(dev, &mai, NULL, &mem[made]) != VK_SUCCESS) break;
			if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
				void* p;
				if (vkMapMemory(dev, mem[made], 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS)
					memset(p, 0, 64);
			}
		}
		count_dma_areas(areaName, &after, &bAfter);
		if (getenv("VKPIN_HOLD") != NULL) {
			printf("  holding %d allocations\n", made);
			sleep(atoi(getenv("VKPIN_HOLD")));
		}
		for (int i = 0; i < made; i++)
			vkFreeMemory(dev, mem[i], NULL);
		count_dma_areas(areaName, &freed, &bFreed);
		printf("type %u (%s%s%s): %d allocated; +%d areas, +%llu KiB"
			" (%llu KiB per allocation); after free %+d areas\n", t,
			f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL " : "",
			f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HOST_VISIBLE " : "",
			f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "CACHED" : "",
			made, after - before, (unsigned long long)(bAfter - bBefore) / 1024,
			made ? (unsigned long long)(bAfter - bBefore) / 1024 / made : 0ULL,
			freed - before);
		system_info info;
		get_system_info(&info);
		printf("  %.1f s; RAM now %llu MiB used of %llu MiB\n",
			(system_time() - start) / 1e6,
			(unsigned long long)info.used_pages * B_PAGE_SIZE / 1048576,
			(unsigned long long)info.max_pages * B_PAGE_SIZE / 1048576);
		fflush(stdout);
	}
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return 0;
}
