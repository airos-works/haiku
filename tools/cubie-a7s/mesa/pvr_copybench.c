/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// The PowerVR transfer path on its own: how long single copies and fills
// take on the GPU, from vkQueueSubmit() to the fence. Each case is one
// command buffer with one command, run once to warm up and then --runs
// times; the line gives the fastest and the mean time and the rate.
//   - vkCmdCopyImageToBuffer of a 1121x538 and a 512x512 B8G8R8A8 image
//     (optimal tiling) into a buffer, with buffer row lengths of the width,
//     the width rounded up to 16 and to 64 pixels, and 1920, into memory of
//     each host-visible type (0: coherent, write-combined for the CPU; 1:
//     cached), and into the screen's frame buffer (VkImportScanoutMemoryHAIKU,
//     row length its own) unless --no-scanout;
//   - vkCmdCopyBuffer of 2.4 MB, between ordinary buffers and into the frame
//     buffer;
//   - vkCmdFillBuffer of 4 MB, of the whole frame buffer, and 256 fills of a
//     1 KiB row each into the frame buffer (pvr_present's square);
//   - vkCmdClearColorImage of the 1121x538 image.
// Writing into the frame buffer shows on the screen until app_server draws
// there again.
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_copybench [--runs N]
//       [--no-scanout]


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vk_haiku_scanout.h"


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

static VkInstance sInstance;
static VkDevice sDevice;
static VkQueue sQueue;
static VkCommandPool sPool;
static VkFence sFence;
static VkPhysicalDeviceMemoryProperties sMemory;
static unsigned sRuns = 5;

#define FUNCTIONS(X) \
	X(vkGetDeviceQueue) X(vkAllocateMemory) X(vkFreeMemory) \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkBindBufferMemory) \
	X(vkGetBufferMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) \
	X(vkBindImageMemory) X(vkGetImageMemoryRequirements) \
	X(vkCreateCommandPool) X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
	X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
	X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
	X(vkCmdClearColorImage) X(vkCmdPipelineBarrier) X(vkCreateFence) \
	X(vkDestroyFence) X(vkResetFences) X(vkQueueSubmit) \
	X(vkWaitForFences) X(vkDestroyDevice)

#define DECLARE(name) static PFN_##name name;
FUNCTIONS(DECLARE)

#define CHECK(call) \
	do { \
		VkResult result_ = (call); \
		if (result_ != VK_SUCCESS) { \
			printf("FAIL: " #call ": %d\n", (int)result_); \
			exit(1); \
		} \
	} while (0)


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


struct target {
	VkBuffer		buffer;
	VkDeviceMemory	memory;
	uint32_t		row_length;	// the frame buffer's, in pixels
	uint32_t		width, height;
};


static int
make_buffer(VkDeviceSize size, uint32_t type, struct target* target)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	CHECK(vkCreateBuffer(sDevice, &info, NULL, &target->buffer));
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(sDevice, target->buffer, &requirements);
	if (type >= sMemory.memoryTypeCount
		|| (requirements.memoryTypeBits & (1u << type)) == 0) {
		vkDestroyBuffer(sDevice, target->buffer, NULL);
		return 0;
	}
	VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	CHECK(vkAllocateMemory(sDevice, &allocate, NULL, &target->memory));
	CHECK(vkBindBufferMemory(sDevice, target->buffer, target->memory, 0));
	return 1;
}


static int
make_scanout(struct target* target)
{
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < sMemory.memoryTypeCount && type == UINT32_MAX;
			i++) {
		if (sMemory.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			type = i;
	}
	VkImportScanoutMemoryHAIKU scanout = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SCANOUT_MEMORY_HAIKU,
	};
	VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &scanout,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(sDevice, &allocate, NULL, &target->memory)
			!= VK_SUCCESS)
		return 0;
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = scanout.size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	CHECK(vkCreateBuffer(sDevice, &info, NULL, &target->buffer));
	CHECK(vkBindBufferMemory(sDevice, target->buffer, target->memory, 0));
	target->row_length = scanout.rowPitch / 4;
	target->width = scanout.width;
	target->height = scanout.height;
	printf("frame buffer: %ux%u, %u bytes per row\n", scanout.width,
		scanout.height, scanout.rowPitch);
	return 1;
}


static VkImage
make_image(uint32_t width, uint32_t height, VkDeviceMemory* memory)
{
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT
			| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage image;
	CHECK(vkCreateImage(sDevice, &info, NULL, &image));
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(sDevice, image, &requirements);
	uint32_t type = 0;
	while (!(requirements.memoryTypeBits & (1u << type)))
		type++;
	VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	CHECK(vkAllocateMemory(sDevice, &allocate, NULL, memory));
	CHECK(vkBindImageMemory(sDevice, image, *memory, 0));
	return image;
}


static void
image_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from,
	VkImageLayout to)
{
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
			| VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}


enum command {
	COPY_IMAGE,
	COPY_BUFFER,
	FILL,
	FILL_ROWS,
	CLEAR_IMAGE
};

struct work {
	enum command	command;
	VkImage			image;
	uint32_t		width, height;		// copied or filled
	uint32_t		row_length;			// pixels
	VkBuffer		source;
	VkBuffer		destination;
	VkDeviceSize	bytes;
};


static void
record(VkCommandBuffer commands, const struct work* work)
{
	switch (work->command) {
		case COPY_IMAGE:
		{
			VkBufferImageCopy region = {
				.bufferOffset = 0,
				.bufferRowLength = work->row_length,
				.bufferImageHeight = work->height,
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.imageExtent = { work->width, work->height, 1 },
			};
			vkCmdCopyImageToBuffer(commands, work->image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work->destination, 1,
				&region);
			break;
		}
		case COPY_BUFFER:
		{
			VkBufferCopy region = { 0, 0, work->bytes };
			vkCmdCopyBuffer(commands, work->source, work->destination, 1,
				&region);
			break;
		}
		case FILL:
			vkCmdFillBuffer(commands, work->destination, 0, work->bytes,
				0xff304050);
			break;
		case FILL_ROWS:
			for (uint32_t row = 0; row < work->height; row++) {
				vkCmdFillBuffer(commands, work->destination,
					(VkDeviceSize)(64 + row) * work->row_length * 4 + 64 * 4,
					(VkDeviceSize)work->width * 4, 0xff00c0ff);
			}
			break;
		case CLEAR_IMAGE:
		{
			VkClearColorValue color = { .float32 = { 0.2f, 0.4f, 0.6f, 1 } };
			VkImageSubresourceRange range = {
				VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			image_barrier(commands, work->image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			vkCmdClearColorImage(commands, work->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
			image_barrier(commands, work->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			break;
		}
	}
}


// one command buffer per run, timed from submit to the fence
static void
run(const char* label, const struct work* work)
{
	double best = 1e30, total = 0;
	for (unsigned i = 0; i <= sRuns; i++) {
		VkCommandBufferAllocateInfo allocate = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = sPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBuffer commands;
		CHECK(vkAllocateCommandBuffers(sDevice, &allocate, &commands));
		VkCommandBufferBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		CHECK(vkBeginCommandBuffer(commands, &begin));
		record(commands, work);
		CHECK(vkEndCommandBuffer(commands));
		VkSubmitInfo submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &commands,
		};
		CHECK(vkResetFences(sDevice, 1, &sFence));
		double start = now_ms();
		CHECK(vkQueueSubmit(sQueue, 1, &submit, sFence));
		VkResult result = vkWaitForFences(sDevice, 1, &sFence, VK_TRUE,
			60000ull * 1000000);
		double elapsed = now_ms() - start;
		vkFreeCommandBuffers(sDevice, sPool, 1, &commands);
		if (result != VK_SUCCESS) {
			printf("%-46s FAILED: %d\n", label, (int)result);
			return;
		}
		if (i == 0)
			continue;	// warm-up
		total += elapsed;
		if (elapsed < best)
			best = elapsed;
	}
	double bytes = work->command == COPY_BUFFER || work->command == FILL
		? (double)work->bytes : (double)work->width * work->height * 4;
	printf("%-46s %8.2f ms best %8.2f ms mean %8.1f MB/s", label, best,
		total / sRuns, bytes / (best / 1000.0) / 1e6);
	if (work->command == FILL_ROWS) {
		// one transfer job (one SUBMIT_JOBS) per fill: the cost of a job
		printf(", %.3f ms per fill", best / work->height);
	}
	printf("\n");
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	int useScanout = 1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
			sRuns = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--no-scanout") == 0)
			useScanout = 0;
		else {
			printf("usage: %s [--runs N] [--no-scanout]\n", argv[0]);
			return 2;
		}
	}
	if (sRuns == 0)
		sRuns = 1;

	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)
		vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_copybench",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	if (createInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	CHECK(createInstance(&instanceInfo, NULL, &sInstance));
	PFN_vkEnumeratePhysicalDevices enumerate
		= (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(
			sInstance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceMemoryProperties getMemory
		= (PFN_vkGetPhysicalDeviceMemoryProperties)vk_icdGetInstanceProcAddr(
			sInstance, "vkGetPhysicalDeviceMemoryProperties");
	PFN_vkCreateDevice createDevice
		= (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(sInstance,
			"vkCreateDevice");
	PFN_vkGetDeviceProcAddr getDeviceProcAddr
		= (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(sInstance,
			"vkGetDeviceProcAddr");
	PFN_vkDestroyInstance destroyInstance
		= (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(sInstance,
			"vkDestroyInstance");

	uint32_t count = 1;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkResult result = enumerate(sInstance, &count, &physicalDevice);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
		printf("FAIL: no physical device (%d)\n", (int)result);
		return 1;
	}
	getMemory(physicalDevice, &sMemory);
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	CHECK(createDevice(physicalDevice, &deviceInfo, NULL, &sDevice));
#define RESOLVE(name) \
	name = (PFN_##name)getDeviceProcAddr(sDevice, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); return 1; }
	FUNCTIONS(RESOLVE)

	vkGetDeviceQueue(sDevice, 0, 0, &sQueue);
	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = 0,
	};
	CHECK(vkCreateCommandPool(sDevice, &poolInfo, NULL, &sPool));
	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &sFence));

	// the images, cleared once and left as transfer sources
	VkDeviceMemory imageMemory[2];
	const uint32_t sizes[2][2] = { { 1121, 538 }, { 512, 512 } };
	VkImage images[2];
	for (int i = 0; i < 2; i++) {
		images[i] = make_image(sizes[i][0], sizes[i][1], &imageMemory[i]);
		struct work clear = { CLEAR_IMAGE, images[i], sizes[i][0],
			sizes[i][1], 0, 0, 0, 0 };
		VkCommandBufferAllocateInfo allocate = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = sPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBuffer commands;
		CHECK(vkAllocateCommandBuffers(sDevice, &allocate, &commands));
		VkCommandBufferBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		};
		CHECK(vkBeginCommandBuffer(commands, &begin));
		image_barrier(commands, images[i], VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		record(commands, &clear);
		CHECK(vkEndCommandBuffer(commands));
		VkSubmitInfo submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &commands,
		};
		CHECK(vkResetFences(sDevice, 1, &sFence));
		CHECK(vkQueueSubmit(sQueue, 1, &submit, sFence));
		CHECK(vkWaitForFences(sDevice, 1, &sFence, VK_TRUE, UINT64_MAX));
		vkFreeCommandBuffers(sDevice, sPool, 1, &commands);
	}

	// destinations: 1920 pixel rows of the larger image's height, per type
	const VkDeviceSize bufferSize = (VkDeviceSize)1920 * 4 * 540;
	struct target ordinary[2] = { 0 };
	int haveType[2];
	for (uint32_t type = 0; type < 2; type++)
		haveType[type] = make_buffer(bufferSize, type, &ordinary[type]);
	struct target scanout = { 0 };
	int haveScanout = useScanout && make_scanout(&scanout);
	if (useScanout && !haveScanout)
		printf("no frame buffer to import: those cases are skipped\n");

	char label[96];
	for (int i = 0; i < 2; i++) {
		const uint32_t width = sizes[i][0], height = sizes[i][1];
		const uint32_t rowLengths[4] = { width, (width + 15) & ~15u,
			(width + 63) & ~63u, 1920 };
		for (uint32_t type = 0; type < 2; type++) {
			if (!haveType[type])
				continue;
			for (int r = 0; r < 4; r++) {
				if (r > 0 && rowLengths[r] == rowLengths[r - 1])
					continue;
				struct work work = { COPY_IMAGE, images[i], width, height,
					rowLengths[r], 0, ordinary[type].buffer, 0 };
				snprintf(label, sizeof(label),
					"image %ux%u -> type %u, rows of %u px", width, height,
					type, rowLengths[r]);
				run(label, &work);
			}
		}
		if (haveScanout) {
			struct work work = { COPY_IMAGE, images[i], width, height,
				scanout.row_length, 0, scanout.buffer, 0 };
			snprintf(label, sizeof(label),
				"image %ux%u -> frame buffer, rows of %u px", width, height,
				scanout.row_length);
			run(label, &work);
		}
	}

	const VkDeviceSize copyBytes = 2400ull * 1024;
	if (haveType[0] && haveType[1]) {
		struct work work = { COPY_BUFFER, 0, 0, 0, 0, ordinary[1].buffer,
			ordinary[0].buffer, copyBytes };
		run("buffer copy 2.4 MB, type 1 -> type 0", &work);
	}
	if (haveType[0] && haveScanout) {
		struct work work = { COPY_BUFFER, 0, 0, 0, 0, ordinary[0].buffer,
			scanout.buffer, copyBytes };
		run("buffer copy 2.4 MB -> frame buffer", &work);
	}
	if (haveType[0]) {
		struct work work = { FILL, 0, 0, 0, 0, 0, ordinary[0].buffer,
			bufferSize & ~3ull };
		snprintf(label, sizeof(label), "fill %.1f MB, type 0",
			(double)bufferSize / 1048576);
		run(label, &work);
	}
	if (haveScanout) {
		struct work work = { FILL, 0, 0, 0, 0, 0, scanout.buffer,
			(VkDeviceSize)scanout.row_length * 4 * scanout.height };
		run("fill the whole frame buffer", &work);
		struct work rows = { FILL_ROWS, 0, 256, 256, scanout.row_length, 0,
			scanout.buffer, 0 };
		run("256 row fills of 1 KiB into the frame buffer", &rows);
	}
	if (haveType[0]) {
		struct work rows = { FILL_ROWS, 0, 256, 256, 1920, 0,
			ordinary[0].buffer, 0 };
		run("256 row fills of 1 KiB, type 0", &rows);
	}
	{
		struct work work = { CLEAR_IMAGE, images[0], sizes[0][0],
			sizes[0][1], 0, 0, 0, 0 };
		run("clear of the 1121x538 image", &work);
	}

	for (uint32_t type = 0; type < 2; type++) {
		if (haveType[type]) {
			vkDestroyBuffer(sDevice, ordinary[type].buffer, NULL);
			vkFreeMemory(sDevice, ordinary[type].memory, NULL);
		}
	}
	if (haveScanout) {
		vkDestroyBuffer(sDevice, scanout.buffer, NULL);
		vkFreeMemory(sDevice, scanout.memory, NULL);
	}
	for (int i = 0; i < 2; i++) {
		vkDestroyImage(sDevice, images[i], NULL);
		vkFreeMemory(sDevice, imageMemory[i], NULL);
	}
	vkDestroyFence(sDevice, sFence, NULL);
	vkDestroyCommandPool(sDevice, sPool, NULL);
	vkDestroyDevice(sDevice, NULL);
	destroyInstance(sInstance, NULL);
	printf("PASS\n");
	return 0;
}
