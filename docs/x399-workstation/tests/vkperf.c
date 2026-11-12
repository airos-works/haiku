/*
 * vkperf: what a Vulkan driver on air/OS costs per memory type, allocation,
 * submission and wait. Loads libvulkan.so.1 at run time (no link-time loader),
 * so it runs on any image with a Vulkan driver: NVK on the GTX 1070, RADV on
 * the Framework laptops.
 *
 * Prints the memory heaps and types, then:
 *  - GPU fill and copy rates and CPU write/read rates for 64 MiB buffers of
 *    each kind of memory (a type that claims DEVICE_LOCAL but fills at bus
 *    speed is in system memory);
 *  - the round trip of a tiny submit and wait;
 *  - for long jobs, how long vkQueueSubmit itself takes and how much CPU it
 *    and vkWaitForFences use (a submit that takes as long as the GPU work is
 *    synchronous);
 *  - frames of ~5 ms CPU work and ~5 ms GPU work with two in flight: about
 *    5 ms a frame when CPU and GPU overlap, 10 ms when submission is
 *    synchronous;
 *  - what allocating + binding + mapping, and freeing, a buffer costs.
 * VKPERF_UUID_ONLY=1 prints the device and its pipelineCacheUUID and stops;
 * VKPERF_ALLOC_ONLY=1 measures the allocation costs only.
 *
 * Build with the air/OS SDK (no Vulkan SDK needed, only Mesa's headers):
 *   . /data1/airos/sdk/x86_64/env.sh
 *   ${CROSS}gcc -O2 --sysroot=$SYSROOT -I<mesa>/include -o vkperf vkperf.c
 * usage: vkperf [device index]   (VK_ICD_FILENAMES picks the driver)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

static PFN_vkGetInstanceProcAddr gipa;
#define F(name) static PFN_##name name
F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceMemoryProperties); F(vkGetPhysicalDeviceQueueFamilyProperties);
F(vkCreateDevice); F(vkGetDeviceQueue); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkCmdFillBuffer); F(vkCmdCopyBuffer);
F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkCreateBuffer);
F(vkGetBufferMemoryRequirements); F(vkAllocateMemory); F(vkFreeMemory); F(vkBindBufferMemory);
F(vkMapMemory); F(vkUnmapMemory); F(vkDestroyBuffer); F(vkResetCommandBuffer);
F(vkQueueWaitIdle); F(vkCmdPipelineBarrier);

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memProps;
static bigtime_t sLastSubmitWall, sLastSubmitCpu;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static bigtime_t thread_cpu(void)
{
	thread_info info;
	get_thread_info(find_thread(NULL), &info);
	return info.user_time + info.kernel_time;
}

static void flags_string(VkMemoryPropertyFlags f, char *out)
{
	out[0] = 0;
	if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) strcat(out, "DEVICE_LOCAL ");
	if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) strcat(out, "HOST_VISIBLE ");
	if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) strcat(out, "COHERENT ");
	if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) strcat(out, "CACHED ");
	if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) strcat(out, "LAZY ");
	if (out[0] == 0) strcpy(out, "(none) ");
}

struct buf {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *map;
	VkDeviceSize size;
};

static int find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
		if ((bits & (1u << i)) && (f & want) == want && (f & avoid) == 0)
			return i;
	}
	return -1;
}

/* time of each step of make_buf()/free_buf(), for alloc_cost() */
static bigtime_t stepAllocate, stepBind, stepMap, stepUnmap, stepFree;

static int make_buf(struct buf *b, VkDeviceSize size, int type)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		| VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b->buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b->buffer, &req);
	if (!(req.memoryTypeBits & (1u << type))) {
		vkDestroyBuffer(dev, b->buffer, NULL);
		return -1;
	}
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	bigtime_t t0 = system_time();
	VkResult r = vkAllocateMemory(dev, &ai, NULL, &b->memory);
	bigtime_t t1 = system_time();
	stepAllocate += t1 - t0;
	if (r != VK_SUCCESS) {
		vkDestroyBuffer(dev, b->buffer, NULL);
		return -1;
	}
	CHECK(vkBindBufferMemory(dev, b->buffer, b->memory, 0));
	bigtime_t t2 = system_time();
	stepBind += t2 - t1;
	b->map = NULL;
	if (memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		CHECK(vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map));
	stepMap += system_time() - t2;
	b->size = size;
	return 0;
}

static void free_buf(struct buf *b)
{
	bigtime_t t0 = system_time();
	if (b->map)
		vkUnmapMemory(dev, b->memory);
	bigtime_t t1 = system_time();
	stepUnmap += t1 - t0;
	vkDestroyBuffer(dev, b->buffer, NULL);
	vkFreeMemory(dev, b->memory, NULL);
	stepFree += system_time() - t1;
}

static void begin(void)
{
	CHECK(vkResetCommandBuffer(cmd, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CHECK(vkBeginCommandBuffer(cmd, &bi));
}

static void barrier(void)
{
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &mb, 0, NULL, 0, NULL);
}

/* submit, wait; returns wall us, *cpu = this thread's CPU us during the wait */
static bigtime_t submit_wait(bigtime_t *waitWall, bigtime_t *waitCpu)
{
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkResetFences(dev, 1, &fence));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	bigtime_t cpuS = thread_cpu();
	bigtime_t start = system_time();
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	bigtime_t submitted = system_time();
	bigtime_t cpu0 = thread_cpu();
	sLastSubmitWall = submitted - start;
	sLastSubmitCpu = cpu0 - cpuS;
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
	bigtime_t end = system_time();
	if (waitCpu)
		*waitCpu = thread_cpu() - cpu0;
	if (waitWall)
		*waitWall = end - submitted;
	return end - start;
}

static void gpu_fill(struct buf *b, int rounds, const char *what)
{
	begin();
	for (int i = 0; i < rounds; i++) {
		vkCmdFillBuffer(cmd, b->buffer, 0, b->size, 0x01020304u + i);
		barrier();
	}
	bigtime_t ww, wc;
	bigtime_t us = submit_wait(&ww, &wc);
	printf("  GPU fill %-34s %8.2f ms  %6.2f GB/s   (submit %.1f ms wall %.1f CPU; wait %.1f wall %.1f CPU)\n",
		what, us / 1000.0, (double)b->size * rounds / us / 1e3, sLastSubmitWall / 1000.0,
		sLastSubmitCpu / 1000.0, ww / 1000.0, wc / 1000.0);
}

static void gpu_copy(struct buf *from, struct buf *to, int rounds, const char *what)
{
	begin();
	VkBufferCopy region = {0, 0, from->size < to->size ? from->size : to->size};
	for (int i = 0; i < rounds; i++) {
		vkCmdCopyBuffer(cmd, from->buffer, to->buffer, 1, &region);
		barrier();
	}
	bigtime_t us = submit_wait(NULL, NULL);
	printf("  GPU copy %-34s %8.2f ms  %6.2f GB/s\n", what, us / 1000.0,
		(double)region.size * rounds / us / 1e3);
}

static void cpu_rw(struct buf *b, const char *what)
{
	if (!b->map)
		return;
	size_t n = b->size;
	char *tmp = malloc(n);
	memset(tmp, 1, n);
	bigtime_t t0 = system_time();
	for (int i = 0; i < 3; i++)
		memcpy(b->map, tmp, n);
	bigtime_t t1 = system_time();
	volatile uint64_t sum = 0;
	for (int i = 0; i < 1; i++)
		memcpy(tmp, b->map, n);
	bigtime_t t2 = system_time();
	sum += tmp[0];
	printf("  CPU write %-33s %8.2f ms  %6.2f GB/s;  read %8.2f ms  %6.2f GB/s\n", what,
		(t1 - t0) / 3000.0, (double)n * 3 / (t1 - t0) / 1e3, (t2 - t1) / 1000.0,
		(double)n / (t2 - t1) / 1e3);
	free(tmp);
}

/*	Makes \a count buffers and then frees them all; with \a touch, the CPU
	writes every page of each right after mapping it (that time is counted
	with the allocation); with \a oneByOne, each is freed before the next is
	made. */
static void alloc_cost_etc(int type, VkDeviceSize size, int count, const char *what,
	int touch, int oneByOne)
{
	struct buf *bufs = calloc(count, sizeof(*bufs));
	bigtime_t madeTime = 0, freedTime = 0, touched = 0;
	int made = 0;
	stepAllocate = stepBind = stepMap = stepUnmap = stepFree = 0;
	for (int i = 0; i < count; i++) {
		bigtime_t t0 = system_time();
		if (make_buf(&bufs[i], size, type) != 0)
			break;
		if (touch && bufs[i].map != NULL) {
			bigtime_t t1 = system_time();
			for (VkDeviceSize offset = 0; offset < size; offset += 4096)
				((volatile char *)bufs[i].map)[offset] = 1;
			touched += system_time() - t1;
		}
		madeTime += system_time() - t0;
		made++;
		if (oneByOne) {
			t0 = system_time();
			free_buf(&bufs[i]);
			freedTime += system_time() - t0;
		}
	}
	if (!oneByOne) {
		bigtime_t t0 = system_time();
		for (int i = 0; i < made; i++)
			free_buf(&bufs[i]);
		freedTime = system_time() - t0;
	}
	if (made == 0) {
		printf("  allocate %-34s failed\n", what);
	} else {
		printf("  allocate+bind+map%s %-*s %7.1f us each, free %7.1f us each (%d%s)\n",
			touch ? "+touch" : "", touch ? 19 : 25, what, (double)madeTime / made,
			(double)freedTime / made, made, oneByOne ? ", one at a time" : "");
		printf("      allocate %.1f, bind %.1f, map %.1f", (double)stepAllocate / made,
			(double)stepBind / made, (double)stepMap / made);
		if (touch)
			printf(", touch %.1f", (double)touched / made);
		printf("; unmap %.1f, free %.1f us\n", (double)stepUnmap / made,
			(double)stepFree / made);
	}
	free(bufs);
}

static void alloc_cost(int type, VkDeviceSize size, int count, const char *what)
{
	alloc_cost_etc(type, size, count, what, 0, 0);
}

int main(int argc, char **argv)
{
	int devIndex = argc > 1 ? atoi(argv[1]) : 0;
	setvbuf(stdout, NULL, _IOLBF, 0);
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib)
		lib = dlopen("libvulkan.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "no Vulkan loader: %s\n", dlerror());
		return 2;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	app.pApplicationName = "vkperf";
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
#define L(name) name = (PFN_##name)gipa(inst, #name)
	L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
	L(vkGetPhysicalDeviceMemoryProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
	L(vkCreateDevice); L(vkGetDeviceQueue); L(vkCreateCommandPool); L(vkAllocateCommandBuffers);
	L(vkBeginCommandBuffer); L(vkEndCommandBuffer); L(vkCmdFillBuffer); L(vkCmdCopyBuffer);
	L(vkQueueSubmit); L(vkCreateFence); L(vkWaitForFences); L(vkResetFences); L(vkCreateBuffer);
	L(vkGetBufferMemoryRequirements); L(vkAllocateMemory); L(vkFreeMemory);
	L(vkBindBufferMemory); L(vkMapMemory); L(vkUnmapMemory); L(vkDestroyBuffer);
	L(vkResetCommandBuffer); L(vkQueueWaitIdle); L(vkCmdPipelineBarrier);

	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	if (devIndex >= (int)n) {
		fprintf(stderr, "only %u devices\n", n);
		return 2;
	}
	VkPhysicalDevice pd = pds[devIndex];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s (driver version %#x)\n", props.deviceName, props.driverVersion);
	printf("pipelineCacheUUID: ");
	for (int i = 0; i < VK_UUID_SIZE; i++)
		printf("%02x", props.pipelineCacheUUID[i]);
	printf("\n");
	if (getenv("VKPERF_UUID_ONLY") != NULL)
		return 0;
	vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
	for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
		printf("  heap %u: %llu MiB%s\n", i,
			(unsigned long long)(memProps.memoryHeaps[i].size >> 20),
			memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "");
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
		char s[128];
		flags_string(memProps.memoryTypes[i].propertyFlags, s);
		printf("  type %u: heap %u, %s\n", i, memProps.memoryTypes[i].heapIndex, s);
	}

	uint32_t qn = 16;
	VkQueueFamilyProperties qf[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t family = 0;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	bigtime_t t0 = system_time();
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	printf("vkCreateDevice: %.1f ms\n", (system_time() - t0) / 1000.0);
	vkGetDeviceQueue(dev, family, 0, &queue);
	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = family;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	/* memory types to compare */
	VkBufferCreateInfo probe = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	probe.size = 4096;
	probe.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		| VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer pb;
	CHECK(vkCreateBuffer(dev, &probe, NULL, &pb));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, pb, &req);
	vkDestroyBuffer(dev, pb, NULL);
	int local = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	int localVisible = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		| VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
	int host = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	int hostCached = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	printf("types used: device-local %d, device-local+host-visible %d, host %d, host cached %d\n",
		local, localVisible, host, hostCached);

	if (getenv("VKPERF_ALLOC_ONLY") != NULL)
		goto allocation_cost;

	const VkDeviceSize size = 64ull << 20;
	struct buf bl = {0}, blv = {0}, bh = {0}, bhc = {0};
	int haveL = local >= 0 && make_buf(&bl, size, local) == 0;
	int haveLV = localVisible >= 0 && make_buf(&blv, size, localVisible) == 0;
	int haveH = host >= 0 && make_buf(&bh, size, host) == 0;
	int haveHC = hostCached >= 0 && hostCached != host && make_buf(&bhc, size, hostCached) == 0;

	printf("\nwarm up (2 s of fills)\n");
	if (haveL) {
		bigtime_t end = system_time() + 2000000;
		while (system_time() < end) {
			begin();
			for (int i = 0; i < 4; i++) {
				vkCmdFillBuffer(cmd, bl.buffer, 0, bl.size, i);
				barrier();
			}
			submit_wait(NULL, NULL);
		}
	}

	printf("\nbandwidth, 64 MiB buffers\n");
	if (haveL) gpu_fill(&bl, 8, "device-local");
	if (haveLV) gpu_fill(&blv, 8, "device-local host-visible");
	if (haveH) gpu_fill(&bh, 2, "host-visible (system memory)");
	if (haveHC) gpu_fill(&bhc, 2, "host-visible cached");
	if (haveL && haveH) gpu_copy(&bh, &bl, 4, "host-visible -> device-local");
	if (haveL && haveH) gpu_copy(&bl, &bh, 2, "device-local -> host-visible");
	if (haveL && haveLV) gpu_copy(&blv, &bl, 4, "dl+hv -> device-local");
	if (haveH) cpu_rw(&bh, "host-visible");
	if (haveHC) cpu_rw(&bhc, "host-visible cached");
	if (haveLV) cpu_rw(&blv, "device-local host-visible");

	printf("\nround trips (empty-ish submit: one 4-byte fill)\n");
	{
		struct buf tiny;
		if (make_buf(&tiny, 4096, local >= 0 ? local : host) == 0) {
			bigtime_t best = 1 << 30, total = 0, cpuTotal = 0, ww, wc;
			int rounds = 500;
			for (int i = 0; i < rounds; i++) {
				begin();
				vkCmdFillBuffer(cmd, tiny.buffer, 0, 4, i);
				bigtime_t us = submit_wait(&ww, &wc);
				total += us;
				cpuTotal += wc;
				if (us < best)
					best = us;
			}
			printf("  submit + wait: mean %.1f us, best %lld us; wait CPU mean %.1f us\n",
				(double)total / rounds, (long long)best, (double)cpuTotal / rounds);
			free_buf(&tiny);
		}
	}

	printf("\nwaiting on long GPU work (CPU burnt by the waiting thread)\n");
	if (haveL) {
		for (int r = 1; r <= 64; r *= 4) {
			char what[64];
			snprintf(what, sizeof(what), "device-local x%d", r * 8);
			gpu_fill(&bl, r * 8, what);
		}
	}
	if (haveH) gpu_fill(&bh, 8, "host-visible x8");

	printf("\nCPU and GPU overlap: frames of ~5 ms CPU work and ~5 ms GPU work, two in flight\n");
	if (haveL) {
		/* GPU work per frame: fills sized to take about 5 ms (VKPERF_FILLS
		 * fixes the number, for comparing drivers) */
		begin();
		for (int k = 0; k < 8; k++) {
			vkCmdFillBuffer(cmd, bl.buffer, 0, bl.size, k);
			barrier();
		}
		bigtime_t eight = submit_wait(NULL, NULL);
		int fills = eight > 0 ? (int)((5000 * 8 + eight / 2) / eight) : 1;
		if (getenv("VKPERF_FILLS") != NULL)
			fills = atoi(getenv("VKPERF_FILLS"));
		if (fills < 1)
			fills = 1;
		VkCommandBuffer frameCmd[2];
		VkFence frameFence[2];
		VkCommandBufferAllocateInfo fai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		fai.commandPool = pool;
		fai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		fai.commandBufferCount = 2;
		CHECK(vkAllocateCommandBuffers(dev, &fai, frameCmd));
		VkFenceCreateInfo sfi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		sfi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CHECK(vkCreateFence(dev, &sfi, NULL, &frameFence[0]));
		CHECK(vkCreateFence(dev, &sfi, NULL, &frameFence[1]));
		const int frames = 100;
		bigtime_t start = system_time(), cpu0 = thread_cpu(), waited = 0;
		for (int f = 0; f < frames; f++) {
			const int i = f & 1;
			bigtime_t w0 = system_time();
			CHECK(vkWaitForFences(dev, 1, &frameFence[i], VK_TRUE, 10000000000ull));
			waited += system_time() - w0;
			CHECK(vkResetFences(dev, 1, &frameFence[i]));
			/* the application's own work for this frame */
			bigtime_t busy = system_time() + 5000;
			volatile uint64_t spin = 0;
			while (system_time() < busy)
				spin++;
			CHECK(vkResetCommandBuffer(frameCmd[i], 0));
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			CHECK(vkBeginCommandBuffer(frameCmd[i], &bi));
			for (int k = 0; k < fills; k++)
				vkCmdFillBuffer(frameCmd[i], bl.buffer, 0, bl.size, f + k);
			CHECK(vkEndCommandBuffer(frameCmd[i]));
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &frameCmd[i];
			CHECK(vkQueueSubmit(queue, 1, &si, frameFence[i]));
		}
		CHECK(vkWaitForFences(dev, 2, frameFence, VK_TRUE, 10000000000ull));
		bigtime_t total = system_time() - start;
		printf("  %d frames (%d fills of 64 MiB each): %.2f ms per frame, %.1f fps; "
			"waiting %.2f ms per frame, thread CPU %.2f ms per frame\n", frames, fills,
			total / 1000.0 / frames, frames * 1e6 / total, waited / 1000.0 / frames,
			(thread_cpu() - cpu0) / 1000.0 / frames);
	}

	if (haveL) free_buf(&bl);
	if (haveLV) free_buf(&blv);
	if (haveH) free_buf(&bh);
	if (haveHC) free_buf(&bhc);
	CHECK(vkQueueWaitIdle(queue));

allocation_cost:
	printf("\nallocation cost\n");
	if (host >= 0) alloc_cost(host, 64 << 10, 200, "64 KiB host-visible");
	if (host >= 0) alloc_cost(host, 1 << 20, 100, "1 MiB host-visible");
	if (local >= 0) alloc_cost(local, 64 << 10, 200, "64 KiB device-local");
	if (local >= 0) alloc_cost(local, 4 << 20, 100, "4 MiB device-local");
	if (localVisible >= 0) alloc_cost(localVisible, 1 << 20, 50, "1 MiB dl+hv");
	if (host >= 0) alloc_cost_etc(host, 1 << 20, 100, "1 MiB host-visible", 1, 0);
	if (host >= 0) alloc_cost_etc(host, 1 << 20, 100, "1 MiB host-visible", 0, 1);
	if (host >= 0) alloc_cost_etc(host, 8 << 20, 20, "8 MiB host-visible", 1, 1);

	printf("\ndone\n");
	return 0;
}
