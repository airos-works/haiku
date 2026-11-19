/*
 * vkbigpage: GPU page sizes on NVK/nvidia_rm (airos-works/haiku#132).
 *
 * The page size is NVK's choice (NVK_NVRM_PAGES=4k, 64k or 2m caps it), so
 * run this once per setting.
 *
 * vkbigpage sub   memory bound in parts at offsets that are 4 KiB aligned
 *                 but not big-page aligned, through the three paths that
 *                 give a resource its own GPU range:
 *                  - a buffer with DEVICE_ADDRESS_CAPTURE_REPLAY;
 *                  - an optimally tiled (block linear) image;
 *                  - a sparse residency buffer bound 4 KiB page by page
 *                    from two memory objects, with holes, then rebound.
 *                 Memory holds a known pattern; the GPU reads through each
 *                 binding and fills through it, and every word of the memory
 *                 and of the readback is checked. Prints FAIL lines and a
 *                 summary.
 * vkbigpage tlb   random 4-byte loads (by device address) over buffers of
 *                 16 MiB to 1 GiB (4 GiB where the driver allows such a
 *                 buffer), and random texel fetches over textures of 64 MiB
 *                 to 1 GiB: Gloads/s, best of 5 runs.
 * vkbigpage zero  for each memory type, whether a new 16 MiB allocation
 *                 comes back cleared after one that was filled was freed;
 *                 then host-visible blocks of 64 KiB to 4 MiB written by the
 *                 processor, freed and allocated again, which a driver that
 *                 keeps freed memory for reuse hands back.
 * VKBIGPAGE_LOCAL_TYPE=<n> uses memory type n as the device-local type.
 *
 * Shaders: shaders/randbuf.comp, shaders/randtex.comp, compiled with
 *   glslangValidator -V --target-env vulkan1.2 --vn spv_randbuf -o spv_randbuf.h shaders/randbuf.comp
 *   glslangValidator -V --target-env vulkan1.2 --vn spv_randtex -o spv_randtex.h shaders/randtex.comp
 * Build with the air/OS SDK (no Vulkan SDK needed, only Mesa's headers):
 *   . /data1/airos/sdk/x86_64/env.sh
 *   ${CROSS}gcc -O2 --sysroot=$SYSROOT -I<mesa>/include -I<dir of spv_*.h> \
 *     -o vkbigpage vkbigpage.c
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "spv_randbuf.h"
#include "spv_randtex.h"

static PFN_vkGetInstanceProcAddr gipa;
#define F(name) static PFN_##name name
F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceMemoryProperties); F(vkGetPhysicalDeviceQueueFamilyProperties);
F(vkGetPhysicalDeviceFeatures2);
F(vkCreateDevice); F(vkGetDeviceQueue); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkCmdFillBuffer); F(vkCmdCopyBuffer);
F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkCreateBuffer);
F(vkGetBufferMemoryRequirements); F(vkAllocateMemory); F(vkFreeMemory); F(vkBindBufferMemory);
F(vkMapMemory); F(vkUnmapMemory); F(vkDestroyBuffer); F(vkResetCommandBuffer);
F(vkQueueWaitIdle); F(vkCmdPipelineBarrier); F(vkQueueBindSparse);
F(vkCreateImage); F(vkDestroyImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory);
F(vkCmdClearColorImage); F(vkGetBufferDeviceAddress); F(vkCreateShaderModule);
F(vkDestroyShaderModule); F(vkCreatePipelineLayout); F(vkDestroyPipelineLayout);
F(vkCreateComputePipelines); F(vkDestroyPipeline); F(vkCmdBindPipeline);
F(vkCmdPushConstants); F(vkCmdDispatch); F(vkCreateDescriptorSetLayout);
F(vkDestroyDescriptorSetLayout); F(vkCreateDescriptorPool); F(vkDestroyDescriptorPool);
F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCmdBindDescriptorSets);
F(vkCreateImageView); F(vkDestroyImageView); F(vkCreateSampler); F(vkDestroySampler);
F(vkCmdWriteTimestamp); F(vkCreateQueryPool); F(vkGetQueryPoolResults);
F(vkCmdResetQueryPool); F(vkDestroyQueryPool);

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memProps;
static VkPhysicalDeviceProperties props;
static int sFailures;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static int find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
		if ((bits & (1u << i)) && (f & want) == want && (f & avoid) == 0)
			return i;
	}
	return -1;
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
	mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static bigtime_t submit_wait(void)
{
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkResetFences(dev, 1, &fence));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	bigtime_t start = system_time();
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 30000000000ull));
	return system_time() - start;
}

struct buf {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t *map;
	VkDeviceSize size;
};

static void make_buf(struct buf *b, VkDeviceSize size, int type, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b->buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b->buffer, &req);
	VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
	fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &fi : NULL;
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	CHECK(vkAllocateMemory(dev, &ai, NULL, &b->memory));
	CHECK(vkBindBufferMemory(dev, b->buffer, b->memory, 0));
	b->map = NULL;
	if (memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		CHECK(vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->map));
	b->size = size;
}

static void free_buf(struct buf *b)
{
	vkDestroyBuffer(dev, b->buffer, NULL);
	vkFreeMemory(dev, b->memory, NULL);
}

static void copy(VkBuffer from, VkDeviceSize fromOff, VkBuffer to, VkDeviceSize toOff,
	VkDeviceSize size)
{
	VkBufferCopy region = {fromOff, toOff, size};
	vkCmdCopyBuffer(cmd, from, to, 1, &region);
}

/* ---- sub: binds at offsets inside big pages ---------------------------- */

#define MEM_SIZE (8u << 20)
#define WORDS (MEM_SIZE / 4)

static int sLocal, sHost;
static struct buf sStaging, sReadback;
/* two memory objects with plain buffers over all of each */
static struct buf sMem[2];

static uint32_t pattern(int m, uint32_t word)
{
	return ((uint32_t)(m + 1) << 28) | word;
}

/* resets both memory objects to their patterns */
static void reset_memory(void)
{
	for (int m = 0; m < 2; m++) {
		for (uint32_t i = 0; i < WORDS; i++)
			sStaging.map[i] = pattern(m, i);
		begin();
		copy(sStaging.buffer, 0, sMem[m].buffer, 0, MEM_SIZE);
		submit_wait();
	}
}

/* copies memory object m into the readback buffer */
static void read_memory(int m)
{
	begin();
	copy(sMem[m].buffer, 0, sReadback.buffer, 0, MEM_SIZE);
	submit_wait();
}

static void fail(const char *test, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void fail(const char *test, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  FAIL %s: ", test);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	sFailures++;
}

/* checks memory object m: words in [from, to) must be \a inside (or the
 * pattern when inside is 0), all others the pattern; returns the bad count */
static int check_memory(const char *test, int m, uint32_t fromWord, uint32_t toWord,
	uint32_t inside)
{
	read_memory(m);
	int bad = 0;
	for (uint32_t i = 0; i < WORDS; i++) {
		uint32_t want = (i >= fromWord && i < toWord && inside != 0) ? inside : pattern(m, i);
		if (sReadback.map[i] != want) {
			if (bad < 3)
				fail(test, "memory %d word %#x (byte %#x) is %#x, expected %#x", m, i,
					i * 4, sReadback.map[i], want);
			bad++;
		}
	}
	if (bad > 3)
		fail(test, "... %d bad words in memory %d", bad, m);
	return bad;
}

static void sub_replay_buffer(VkDeviceSize offset, VkDeviceSize size)
{
	char test[64];
	snprintf(test, sizeof(test), "replay buffer at %#llx", (unsigned long long)offset);
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.flags = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		| VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	VkBuffer b;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b, &req);
	if (offset % req.alignment != 0) {
		printf("  %s: skipped, alignment %#llx\n", test, (unsigned long long)req.alignment);
		vkDestroyBuffer(dev, b, NULL);
		return;
	}
	VkResult r = vkBindBufferMemory(dev, b, sMem[0].memory, offset);
	if (r != VK_SUCCESS) {
		fail(test, "vkBindBufferMemory: %d", r);
		vkDestroyBuffer(dev, b, NULL);
		return;
	}
	/* read through it */
	begin();
	copy(b, 0, sReadback.buffer, 0, size);
	submit_wait();
	int bad = 0;
	for (uint32_t i = 0; i < size / 4; i++) {
		uint32_t want = pattern(0, offset / 4 + i);
		if (sReadback.map[i] != want) {
			if (bad < 3)
				fail(test, "read word %#x is %#x, expected %#x (memory byte %#x)", i,
					sReadback.map[i], want, (sReadback.map[i] & 0x0fffffff) * 4);
			bad++;
		}
	}
	/* write through it */
	begin();
	vkCmdFillBuffer(cmd, b, 0, size, 0xdeadbeef);
	submit_wait();
	bad += check_memory(test, 0, offset / 4, (offset + size) / 4, 0xdeadbeef);
	if (bad == 0)
		printf("  ok %s\n", test);
	vkDestroyBuffer(dev, b, NULL);
	reset_memory();
}

static void sub_image(VkDeviceSize wantOffset)
{
	char test[64];
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R32_UINT;
	ici.extent = (VkExtent3D){256, 256, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImage img;
	CHECK(vkCreateImage(dev, &ici, NULL, &img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, img, &req);
	VkDeviceSize offset = (wantOffset + req.alignment - 1) / req.alignment * req.alignment;
	snprintf(test, sizeof(test), "tiled image at %#llx (size %#llx, alignment %#llx)",
		(unsigned long long)offset, (unsigned long long)req.size,
		(unsigned long long)req.alignment);
	VkResult r = vkBindImageMemory(dev, img, sMem[0].memory, offset);
	if (r != VK_SUCCESS) {
		fail(test, "vkBindImageMemory: %d", r);
		vkDestroyImage(dev, img, NULL);
		return;
	}
	begin();
	VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	ib.srcAccessMask = 0;
	ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib.image = img;
	ib.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
	VkClearColorValue color = {.uint32 = {0xdeadbeef, 0, 0, 0}};
	vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
		&ib.subresourceRange);
	submit_wait();
	/* 256x256 R32 fills its whole size; nothing outside it may change */
	int bad = check_memory(test, 0, offset / 4, (offset + req.size) / 4, 0xdeadbeef);
	if (bad == 0)
		printf("  ok %s\n", test);
	vkDestroyImage(dev, img, NULL);
	reset_memory();
}

/* where page p of the sparse buffer comes from in round \a round: memory
 * object, 4 KiB page in it, or -1 for a hole */
static void sparse_source(int round, uint32_t p, int *m, int32_t *page)
{
	uint32_t h = (p * 2654435761u) ^ (round * 0x9e3779b9u);
	h ^= h >> 13;
	if (h % 5 == 0) {
		*m = -1;
		*page = -1;
		return;
	}
	*m = (h >> 3) & 1;
	/* scattered, distinct per memory object: page p maps to (p * 37 + round)
	 * modulo the 2048 pages */
	*page = (int32_t)((p * 37u + round * 101u) % (MEM_SIZE / 4096));
}

#define SPARSE_SIZE (1u << 20)
#define SPARSE_PAGES (SPARSE_SIZE / 4096)

static void sub_sparse(void)
{
	const char *test = "sparse buffer";
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
	bi.size = SPARSE_SIZE;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer b;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b, &req);
	printf("  sparse buffer: alignment %#llx\n", (unsigned long long)req.alignment);
	if (req.alignment > 4096) {
		printf("  sparse buffer: skipped (binds at %#llx granularity only)\n",
			(unsigned long long)req.alignment);
		vkDestroyBuffer(dev, b, NULL);
		return;
	}

	for (int round = 0; round < 3; round++) {
		/* bind every page (holes as unbinds, so rebinding replaces) */
		VkSparseMemoryBind *binds = calloc(SPARSE_PAGES, sizeof(*binds));
		for (uint32_t p = 0; p < SPARSE_PAGES; p++) {
			int m;
			int32_t page;
			sparse_source(round, p, &m, &page);
			binds[p].resourceOffset = (VkDeviceSize)p * 4096;
			binds[p].size = 4096;
			binds[p].memory = m < 0 ? VK_NULL_HANDLE : sMem[m].memory;
			binds[p].memoryOffset = m < 0 ? 0 : (VkDeviceSize)page * 4096;
		}
		VkSparseBufferMemoryBindInfo bbi = {b, SPARSE_PAGES, binds};
		VkBindSparseInfo bsi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
		bsi.bufferBindCount = 1;
		bsi.pBufferBinds = &bbi;
		CHECK(vkResetFences(dev, 1, &fence));
		bigtime_t t0 = system_time();
		VkResult r = vkQueueBindSparse(queue, 1, &bsi, fence);
		if (r != VK_SUCCESS) {
			fail(test, "round %d: vkQueueBindSparse: %d", round, r);
			free(binds);
			break;
		}
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
		bigtime_t bindUs = system_time() - t0;
		free(binds);

		/* read through it: holes must read as zero */
		begin();
		vkCmdFillBuffer(cmd, sReadback.buffer, 0, SPARSE_SIZE, 0x11111111);
		barrier();
		copy(b, 0, sReadback.buffer, 0, SPARSE_SIZE);
		submit_wait();
		int bad = 0;
		for (uint32_t w = 0; w < SPARSE_SIZE / 4; w++) {
			int m;
			int32_t page;
			sparse_source(round, w / 1024, &m, &page);
			uint32_t want = m < 0 ? 0 : pattern(m, page * 1024 + w % 1024);
			if (sReadback.map[w] != want) {
				if (bad < 3)
					fail(test, "round %d: read word %#x (page %u) is %#x, expected %#x",
						round, w, w / 1024, sReadback.map[w], want);
				bad++;
			}
		}
		if (bad > 3)
			fail(test, "round %d: ... %d bad words read", round, bad);

		/* write through it: only the bound pages change */
		begin();
		vkCmdFillBuffer(cmd, b, 0, SPARSE_SIZE, 0x77777777);
		submit_wait();
		/* which 4 KiB pages of each memory object are bound somewhere */
		static uint8_t boundPage[2][MEM_SIZE / 4096];
		memset(boundPage, 0, sizeof(boundPage));
		for (uint32_t p = 0; p < SPARSE_PAGES; p++) {
			int m;
			int32_t page;
			sparse_source(round, p, &m, &page);
			if (m >= 0)
				boundPage[m][page] = 1;
		}
		for (int mm = 0; mm < 2; mm++) {
			read_memory(mm);
			int badM = 0;
			for (uint32_t i = 0; i < WORDS; i++) {
				uint32_t want = boundPage[mm][i / 1024] ? 0x77777777 : pattern(mm, i);
				if (sReadback.map[i] != want) {
					if (badM < 3)
						fail(test, "round %d: memory %d word %#x is %#x, expected %#x",
							round, mm, i, sReadback.map[i], want);
					badM++;
				}
			}
			if (badM > 3)
				fail(test, "round %d: ... %d bad words in memory %d", round, badM, mm);
			bad += badM;
		}
		if (bad == 0)
			printf("  ok sparse buffer round %d (%u binds in %.1f ms)\n", round,
				SPARSE_PAGES, bindUs / 1000.0);
		reset_memory();
	}
	vkDestroyBuffer(dev, b, NULL);
}

/* One 4 KiB bind whose GPU address and memory offset are the same distance
 * into a 64 KiB page (0x1000 and 0x11000): RM's offset checks accept it.
 * Everything else in the sparse buffer is a hole and must read as zero, and
 * a fill through the page must change that page of memory only. */
static void sub_sparse_congruent(void)
{
	const char *test = "sparse 4 KiB bind at congruent offsets";
	const VkDeviceSize size = 0x40000, vaOff = 0x1000, memOff = 0x11000;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer b;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
	VkSparseMemoryBind bind = {vaOff, 0x1000, sMem[0].memory, memOff, 0};
	VkSparseBufferMemoryBindInfo bbi = {b, 1, &bind};
	VkBindSparseInfo bsi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
	bsi.bufferBindCount = 1;
	bsi.pBufferBinds = &bbi;
	CHECK(vkResetFences(dev, 1, &fence));
	VkResult r = vkQueueBindSparse(queue, 1, &bsi, fence);
	if (r != VK_SUCCESS) {
		fail(test, "vkQueueBindSparse: %d", r);
		vkDestroyBuffer(dev, b, NULL);
		return;
	}
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
	begin();
	vkCmdFillBuffer(cmd, sReadback.buffer, 0, size, 0x11111111);
	barrier();
	copy(b, 0, sReadback.buffer, 0, size);
	submit_wait();
	int bad = 0;
	for (uint32_t w = 0; w < size / 4; w++) {
		uint32_t want = (w * 4 >= vaOff && w * 4 < vaOff + 0x1000) ?
			pattern(0, (memOff - vaOff) / 4 + w) : 0;
		if (sReadback.map[w] != want) {
			if (bad < 4)
				fail(test, "read word %#x (byte %#x) is %#x, expected %#x", w, w * 4,
					sReadback.map[w], want);
			bad++;
		}
	}
	if (bad > 4)
		fail(test, "... %d bad words read", bad);
	begin();
	vkCmdFillBuffer(cmd, b, 0, size, 0x77777777);
	submit_wait();
	bad += check_memory(test, 0, memOff / 4, (memOff + 0x1000) / 4, 0x77777777);
	if (bad == 0)
		printf("  ok %s\n", test);
	vkDestroyBuffer(dev, b, NULL);
	reset_memory();
}

static void run_sub(void)
{
	make_buf(&sStaging, MEM_SIZE, sHost, 0);
	make_buf(&sReadback, MEM_SIZE, sHost, 0);
	for (int m = 0; m < 2; m++)
		make_buf(&sMem[m], MEM_SIZE, sLocal, 0);
	reset_memory();
	/* sanity: the plain buffers (no range of their own) */
	check_memory("plain", 0, 0, 0, 0);
	check_memory("plain", 1, 0, 0, 0);

	const VkDeviceSize offsets[] = {0, 0x1000, 0xf000, 0x10000, 0x11000, 0x101000,
		0x200000, 0x201000, 0x3ff000};
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
		sub_replay_buffer(offsets[i], offsets[i] == 0x3ff000 ? 0x2000 : 0x10000);
	/* a 64 KiB-aligned bind that straddles a huge page boundary */
	sub_replay_buffer(0x1f0000, 0x20000);
	/* a 2 MiB bind at a 64 KiB offset (big pages, not huge) */
	sub_replay_buffer(0x10000, 0x200000);
	sub_replay_buffer(0x200000, 0x200000);

	const VkDeviceSize imageOffsets[] = {0x1000, 0x2000, 0x10000, 0x11000, 0x205000};
	for (unsigned i = 0; i < sizeof(imageOffsets) / sizeof(imageOffsets[0]); i++)
		sub_image(imageOffsets[i]);

	sub_sparse_congruent();
	sub_sparse();

	for (int m = 0; m < 2; m++)
		free_buf(&sMem[m]);
	free_buf(&sStaging);
	free_buf(&sReadback);
	printf("sub: %d failures\n", sFailures);
}

/* ---- tlb: random access over large memory ------------------------------ */

static VkShaderModule make_module(const uint32_t *code, size_t size)
{
	VkShaderModuleCreateInfo sci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	sci.codeSize = size;
	sci.pCode = code;
	VkShaderModule m;
	CHECK(vkCreateShaderModule(dev, &sci, NULL, &m));
	return m;
}

static VkPipeline make_pipeline(VkShaderModule m, VkPipelineLayout layout)
{
	VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = m;
	cpi.stage.pName = "main";
	cpi.layout = layout;
	VkPipeline p;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p));
	return p;
}

#define THREADS (256u * 1024)
#define ITERS 256u

static VkQueryPool sQueries;

/* what timed() binds and pushes before each dispatch */
static VkPipeline sPipe;
static VkPipelineLayout sLayout;
static VkDescriptorSet sSet;
static const void *sPush;
static uint32_t sPushSize;

static double timed(int reps)
{
	double best = 1e30;
	for (int r = 0; r < reps; r++) {
		begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sPipe);
		if (sSet != VK_NULL_HANDLE)
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sLayout, 0, 1,
				&sSet, 0, NULL);
		vkCmdPushConstants(cmd, sLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sPushSize, sPush);
		vkCmdResetQueryPool(cmd, sQueries, 0, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sQueries, 0);
		vkCmdDispatch(cmd, THREADS / 256, 1, 1);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sQueries, 1);
		submit_wait();
		uint64_t ts[2];
		CHECK(vkGetQueryPoolResults(dev, sQueries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		double us = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0;
		if (us < best)
			best = us;
	}
	return best;
}

static void run_tlb(void)
{
	VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qpi.queryCount = 2;
	CHECK(vkCreateQueryPool(dev, &qpi, NULL, &sQueries));

	struct buf result;
	make_buf(&result, THREADS * 4, sLocal, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		| VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	VkBufferDeviceAddressInfo dai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
	dai.buffer = result.buffer;
	VkDeviceAddress resultAddr = vkGetBufferDeviceAddress(dev, &dai);

	/* buffer loads */
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &sLayout));
	VkShaderModule mod = make_module(spv_randbuf, sizeof(spv_randbuf));
	sPipe = make_pipeline(mod, sLayout);
	sSet = VK_NULL_HANDLE;

	printf("random 4-byte loads, %u threads x %u loads (Gloads/s, best of 5)\n",
		THREADS, ITERS);
	int warmed = 0;
	for (uint64_t size = 16ull << 20; size <= (4ull << 30); size *= 4) {
		struct buf b;
		VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = size;
		bi.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (vkCreateBuffer(dev, &bi, NULL, &b.buffer) != VK_SUCCESS) {
			printf("  %5llu MiB: buffer refused\n", (unsigned long long)(size >> 20));
			continue;
		}
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, b.buffer, &req);
		VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &fi;
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = sLocal;
		bigtime_t t0 = system_time();
		if (vkAllocateMemory(dev, &ai, NULL, &b.memory) != VK_SUCCESS) {
			printf("  %5llu MiB: allocation refused\n", (unsigned long long)(size >> 20));
			vkDestroyBuffer(dev, b.buffer, NULL);
			continue;
		}
		bigtime_t allocUs = system_time() - t0;
		CHECK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
		begin();
		vkCmdFillBuffer(cmd, b.buffer, 0, size, 1);
		submit_wait();
		dai.buffer = b.buffer;
		struct {
			uint64_t base, result;
			uint32_t mask, iters;
		} pc = {vkGetBufferDeviceAddress(dev, &dai), resultAddr,
			(uint32_t)(size / 4 - 1), ITERS};
		sPush = &pc;
		sPushSize = sizeof(pc);
		if (!warmed) {
			bigtime_t until = system_time() + 2000000;
			while (system_time() < until)
				timed(1);
			warmed = 1;
		}
		double us = timed(5);
		printf("  %5llu MiB: %7.2f Gloads/s (%8.1f us; allocated in %lld us)\n",
			(unsigned long long)(size >> 20), (double)THREADS * ITERS / us / 1000.0, us,
			(long long)allocUs);
		free_buf(&b);
	}
	vkDestroyPipeline(dev, sPipe, NULL);
	vkDestroyShaderModule(dev, mod, NULL);
	vkDestroyPipelineLayout(dev, sLayout, NULL);

	/* texel fetches */
	VkDescriptorSetLayoutBinding dslb[2] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo dsli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dsli.bindingCount = 2;
	dsli.pBindings = dslb;
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dsli, NULL, &dsl));
	VkPushConstantRange pcr2 = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &dsl;
	pli.pPushConstantRanges = &pcr2;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &sLayout));
	mod = make_module(spv_randtex, sizeof(spv_randtex));
	sPipe = make_pipeline(mod, sLayout);
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 8;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = ps;
	dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VkDescriptorPool pool;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
	VkSamplerCreateInfo sai = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sai.magFilter = sai.minFilter = VK_FILTER_NEAREST;
	sai.addressModeU = sai.addressModeV = sai.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSampler sampler;
	CHECK(vkCreateSampler(dev, &sai, NULL, &sampler));

	printf("random texel fetches (RGBA8), %u threads x %u fetches (Gfetches/s, best of 5)\n",
		THREADS, ITERS);
	for (uint32_t dim = 4096; dim <= 16384; dim *= 2) {
		VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = (VkExtent3D){dim, dim, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkImage img;
		if (vkCreateImage(dev, &ici, NULL, &img) != VK_SUCCESS) {
			printf("  %5u^2: image refused\n", dim);
			continue;
		}
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, img, &req);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		VkDeviceMemory mem;
		if (vkAllocateMemory(dev, &ai, NULL, &mem) != VK_SUCCESS) {
			printf("  %5u^2: allocation refused\n", dim);
			vkDestroyImage(dev, img, NULL);
			continue;
		}
		CHECK(vkBindImageMemory(dev, img, mem, 0));
		begin();
		VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		ib.image = img;
		ib.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
		VkClearColorValue color = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
		vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
			&ib.subresourceRange);
		ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
		submit_wait();

		VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = img;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = ici.format;
		vci.subresourceRange = ib.subresourceRange;
		VkImageView view;
		CHECK(vkCreateImageView(dev, &vci, NULL, &view));
		VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dsai.descriptorPool = pool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts = &dsl;
		CHECK(vkAllocateDescriptorSets(dev, &dsai, &sSet));
		VkDescriptorImageInfo dii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo dbi = {result.buffer, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
			{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
		w[0].dstSet = sSet;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[0].pImageInfo = &dii;
		w[1].dstSet = sSet;
		w[1].dstBinding = 1;
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[1].pBufferInfo = &dbi;
		vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
		uint32_t pc[2] = {dim - 1, ITERS};
		sPush = pc;
		sPushSize = sizeof(pc);
		timed(2);
		double us = timed(5);
		printf("  %5u^2 (%4llu MiB): %7.2f Gfetches/s (%8.1f us)\n", dim,
			(unsigned long long)(req.size >> 20), (double)THREADS * ITERS / us / 1000.0, us);
		vkDestroyImageView(dev, view, NULL);
		vkDestroyImage(dev, img, NULL);
		vkFreeMemory(dev, mem, NULL);
		/* the pool frees the set */
		sSet = VK_NULL_HANDLE;
		CHECK(vkQueueWaitIdle(queue));
	}
	vkDestroySampler(dev, sampler, NULL);
	vkDestroyDescriptorPool(dev, pool, NULL);
	vkDestroyPipeline(dev, sPipe, NULL);
	vkDestroyShaderModule(dev, mod, NULL);
	vkDestroyPipelineLayout(dev, sLayout, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	free_buf(&result);
	vkDestroyQueryPool(dev, sQueries, NULL);
}

/* ---- zero: does a new allocation come back cleared? -------------------- */

static void run_zero(void)
{
	struct buf rb;
	make_buf(&rb, 16 << 20, sHost, 0);
	for (uint32_t type = 0; type < memProps.memoryTypeCount; type++) {
		for (int round = 0; round < 3; round++) {
			struct buf b;
			make_buf(&b, 16 << 20, type, 0);
			begin();
			copy(b.buffer, 0, rb.buffer, 0, 16 << 20);
			barrier();
			vkCmdFillBuffer(cmd, b.buffer, 0, 16 << 20, 0xabababab);
			submit_wait();
			uint32_t nonzero = 0;
			for (uint32_t i = 0; i < (16u << 20) / 4; i++)
				nonzero += rb.map[i] != 0;
			printf("  type %u round %d: %u of %u words not zero when allocated\n", type,
				round, nonzero, (16u << 20) / 4);
			free_buf(&b);
		}
	}
	/* small host-visible blocks, written by the processor before they are
	 * freed: what a driver that keeps freed memory for reuse hands back */
	for (uint32_t type = 0; type < memProps.memoryTypeCount; type++) {
		if (!(memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		const uint32_t sizes[] = {64 << 10, 1 << 20, 4 << 20};
		for (unsigned s = 0; s < 3; s++) {
			uint32_t nonzeroGpu = 0, nonzeroCpu = 0;
			for (int round = 0; round < 4; round++) {
				struct buf b;
				make_buf(&b, sizes[s], type, 0);
				begin();
				copy(b.buffer, 0, rb.buffer, 0, sizes[s]);
				submit_wait();
				for (uint32_t i = 0; i < sizes[s] / 4; i++) {
					nonzeroGpu += rb.map[i] != 0;
					nonzeroCpu += b.map[i] != 0;
				}
				memset(b.map, 0xab, sizes[s]);
				free_buf(&b);
			}
			printf("  type %u, %u KiB written and freed 4 times: %u words not zero by GPU, "
				"%u by CPU\n", type, sizes[s] >> 10, nonzeroGpu, nonzeroCpu);
			if (nonzeroGpu + nonzeroCpu != 0)
				sFailures++;
		}
	}
	free_buf(&rb);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "sub";
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
	app.apiVersion = VK_API_VERSION_1_2;
	app.pApplicationName = "vkbigpage";
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
#define L(name) name = (PFN_##name)gipa(inst, #name)
	L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
	L(vkGetPhysicalDeviceMemoryProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
	L(vkGetPhysicalDeviceFeatures2);
	L(vkCreateDevice); L(vkGetDeviceQueue); L(vkCreateCommandPool); L(vkAllocateCommandBuffers);
	L(vkBeginCommandBuffer); L(vkEndCommandBuffer); L(vkCmdFillBuffer); L(vkCmdCopyBuffer);
	L(vkQueueSubmit); L(vkCreateFence); L(vkWaitForFences); L(vkResetFences); L(vkCreateBuffer);
	L(vkGetBufferMemoryRequirements); L(vkAllocateMemory); L(vkFreeMemory);
	L(vkBindBufferMemory); L(vkMapMemory); L(vkUnmapMemory); L(vkDestroyBuffer);
	L(vkResetCommandBuffer); L(vkQueueWaitIdle); L(vkCmdPipelineBarrier); L(vkQueueBindSparse);
	L(vkCreateImage); L(vkDestroyImage); L(vkGetImageMemoryRequirements); L(vkBindImageMemory);
	L(vkCmdClearColorImage); L(vkGetBufferDeviceAddress); L(vkCreateShaderModule);
	L(vkDestroyShaderModule); L(vkCreatePipelineLayout); L(vkDestroyPipelineLayout);
	L(vkCreateComputePipelines); L(vkDestroyPipeline); L(vkCmdBindPipeline);
	L(vkCmdPushConstants); L(vkCmdDispatch); L(vkCreateDescriptorSetLayout);
	L(vkDestroyDescriptorSetLayout); L(vkCreateDescriptorPool); L(vkDestroyDescriptorPool);
	L(vkAllocateDescriptorSets); L(vkUpdateDescriptorSets); L(vkCmdBindDescriptorSets);
	L(vkCreateImageView); L(vkDestroyImageView); L(vkCreateSampler); L(vkDestroySampler);
	L(vkCmdWriteTimestamp); L(vkCreateQueryPool); L(vkGetQueryPoolResults);
	L(vkCmdResetQueryPool); L(vkDestroyQueryPool);

	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = pds[0];
	vkGetPhysicalDeviceProperties(pd, &props);
	vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
	printf("device: %s, NVK_NVRM_PAGES=%s\n", props.deviceName,
		getenv("NVK_NVRM_PAGES") ? getenv("NVK_NVRM_PAGES") : "(unset)");

	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f12;
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	printf("sparseBinding %d sparseResidencyBuffer %d bufferDeviceAddressCaptureReplay %d\n",
		f2.features.sparseBinding, f2.features.sparseResidencyBuffer,
		f12.bufferDeviceAddressCaptureReplay);
	VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	e12.bufferDeviceAddress = VK_TRUE;
	e12.bufferDeviceAddressCaptureReplay = f12.bufferDeviceAddressCaptureReplay;
	VkPhysicalDeviceFeatures2 e2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	e2.pNext = &e12;
	e2.features.sparseBinding = f2.features.sparseBinding;
	e2.features.sparseResidencyBuffer = f2.features.sparseResidencyBuffer;
	e2.features.shaderInt64 = VK_TRUE;

	uint32_t qn = 16;
	VkQueueFamilyProperties qf[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t family = 0;
	for (uint32_t i = 0; i < qn; i++)
		if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			&& (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
			family = i;
			break;
		}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &e2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
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

	VkBufferCreateInfo probe = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	probe.size = 4096;
	probe.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer pb;
	CHECK(vkCreateBuffer(dev, &probe, NULL, &pb));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, pb, &req);
	vkDestroyBuffer(dev, pb, NULL);
	sLocal = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	sHost = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (getenv("VKBIGPAGE_LOCAL_TYPE") != NULL)
		sLocal = atoi(getenv("VKBIGPAGE_LOCAL_TYPE"));
	printf("device-local type %d, host type %d\n", sLocal, sHost);

	if (strcmp(mode, "sub") == 0)
		run_sub();
	else if (strcmp(mode, "tlb") == 0)
		run_tlb();
	else if (strcmp(mode, "zero") == 0)
		run_zero();
	else {
		fprintf(stderr, "usage: vkbigpage sub|tlb|zero\n");
		return 2;
	}
	CHECK(vkQueueWaitIdle(queue));
	return sFailures != 0;
}
