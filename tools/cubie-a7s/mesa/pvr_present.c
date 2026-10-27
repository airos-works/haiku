/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// The front half of Summit's direct present on its own: the screen's frame
// buffer, imported through VkImportScanoutMemoryHAIKU (vk_haiku_scanout.h:
// the sunxi_display frame buffer, cloned and imported with
// PVR_HAIKU_NR_IMPORT_HOST), filled in a rectangle by the GPU, row by row
// with vkCmdFillBuffer. The rectangle is then read back through the import's
// CPU map (the clone) and checked, and so are a few pixels beside it, which
// must not turn the fill colour. The colour stays on the screen until
// app_server draws there again.
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_present [--rect X,Y,W,H]
//       [--color 0xAARRGGBB] [--expect-none] [--shim]
// (defaults: 64,64,256,256 and 0xff00c0ff, an orange)
// --expect-none: pass when there is no frame buffer to import (the host
// smoke test, or a kernel driver without the import): the fallback;
// --shim: the GPU runs nothing (build.sh shim, with the driver's stand-in
// frame buffer, PVR_SHIM_SCANOUT=WxH), so the pixels are not checked.


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
static PFN_vkGetDeviceProcAddr sGetDeviceProcAddr;

#define INSTANCE_FN(name) \
	PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(sInstance, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); exit(1); }
#define DEVICE_FN(name) \
	PFN_##name name = (PFN_##name)sGetDeviceProcAddr(sDevice, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); exit(1); }
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


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	unsigned x = 64, y = 64, w = 256, h = 256;
	uint32_t color = 0xff00c0ff;
	int expectNone = 0, shim = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--rect") == 0 && i + 1 < argc
			&& sscanf(argv[i + 1], "%u,%u,%u,%u", &x, &y, &w, &h) == 4)
			i++;
		else if (strcmp(argv[i], "--color") == 0 && i + 1 < argc)
			color = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--expect-none") == 0)
			expectNone = 1;
		else if (strcmp(argv[i], "--shim") == 0)
			shim = 1;
		else {
			printf("usage: %s [--rect X,Y,W,H] [--color 0xAARRGGBB] "
				"[--expect-none] [--shim]\n", argv[0]);
			return 2;
		}
	}

	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_present",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	if (vkCreateInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));
	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
	INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
	INSTANCE_FN(vkCreateDevice);
	INSTANCE_FN(vkGetDeviceProcAddr);
	INSTANCE_FN(vkDestroyInstance);
	sGetDeviceProcAddr = vkGetDeviceProcAddr;

	uint32_t count = 1;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkResult result = vkEnumeratePhysicalDevices(sInstance, &count,
		&physicalDevice);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
		printf("FAIL: no physical device (%d)\n", (int)result);
		return 1;
	}
	VkQueueFamilyProperties families[8];
	count = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count,
		families);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < count; i++) {
		if (families[i].queueFlags
			& (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			family = i;
			break;
		}
	}
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	CHECK(vkCreateDevice(physicalDevice, &deviceInfo, NULL, &sDevice));

	DEVICE_FN(vkGetDeviceQueue);
	DEVICE_FN(vkAllocateMemory);
	DEVICE_FN(vkFreeMemory);
	DEVICE_FN(vkMapMemory);
	DEVICE_FN(vkUnmapMemory);
	DEVICE_FN(vkCreateBuffer);
	DEVICE_FN(vkDestroyBuffer);
	DEVICE_FN(vkBindBufferMemory);
	DEVICE_FN(vkCreateCommandPool);
	DEVICE_FN(vkDestroyCommandPool);
	DEVICE_FN(vkAllocateCommandBuffers);
	DEVICE_FN(vkBeginCommandBuffer);
	DEVICE_FN(vkEndCommandBuffer);
	DEVICE_FN(vkCmdFillBuffer);
	DEVICE_FN(vkCmdPipelineBarrier);
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkDestroyFence);
	DEVICE_FN(vkQueueSubmit);
	DEVICE_FN(vkWaitForFences);
	DEVICE_FN(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(sDevice, family, 0, &queue);

	// the frame buffer, into the first device-local memory type
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		if (memory.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			type = i;
			break;
		}
	}
	VkImportScanoutMemoryHAIKU scanout = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SCANOUT_MEMORY_HAIKU,
	};
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &scanout,
		.allocationSize = 0,
		.memoryTypeIndex = type,
	};
	VkDeviceMemory frameBuffer = VK_NULL_HANDLE;
	double start = now_ms();
	result = vkAllocateMemory(sDevice, &allocateInfo, NULL, &frameBuffer);
	if (result != VK_SUCCESS) {
		printf("no frame buffer to import: %d\n", (int)result);
		vkDestroyDevice(sDevice, NULL);
		vkDestroyInstance(sInstance, NULL);
		printf("%s\n", expectNone ? "PASS" : "FAIL");
		return expectNone ? 0 : 1;
	}
	printf("frame buffer: %ux%u, %u bytes per row, %llu bytes, imported in "
		"%.2f ms\n", scanout.width, scanout.height, scanout.rowPitch,
		(unsigned long long)scanout.size, now_ms() - start);
	if (expectNone) {
		printf("FAIL: a frame buffer was imported\n");
		return 1;
	}
	if (x >= scanout.width || y >= scanout.height) {
		printf("FAIL: the rectangle is off the screen\n");
		return 1;
	}
	if (x + w > scanout.width)
		w = scanout.width - x;
	if (y + h > scanout.height)
		h = scanout.height - y;

	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = scanout.size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkBuffer buffer;
	CHECK(vkCreateBuffer(sDevice, &bufferInfo, NULL, &buffer));
	CHECK(vkBindBufferMemory(sDevice, buffer, frameBuffer, 0));

	// pixels around the rectangle, which must stay as they are
	uint8_t* pixels;
	CHECK(vkMapMemory(sDevice, frameBuffer, 0, VK_WHOLE_SIZE, 0,
		(void**)&pixels));
	const uint32_t pitch = scanout.rowPitch;
	struct { unsigned x, y; uint32_t value; } around[4] = {
		{ x > 0 ? x - 1 : x + w, y },
		{ x + w < scanout.width ? x + w : x, y + h - 1 },
		{ x, y > 0 ? y - 1 : y + h },
		{ x + w - 1, y + h < scanout.height ? y + h : y },
	};
	for (int i = 0; i < 4; i++) {
		memcpy(&around[i].value, pixels + (size_t)around[i].y * pitch
			+ (size_t)around[i].x * 4, 4);
	}

	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(sDevice, &poolInfo, NULL, &pool));
	VkCommandBufferAllocateInfo commandInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands;
	CHECK(vkAllocateCommandBuffers(sDevice, &commandInfo, &commands));
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	CHECK(vkBeginCommandBuffer(commands, &beginInfo));
	for (unsigned row = 0; row < h; row++) {
		vkCmdFillBuffer(commands, buffer,
			(VkDeviceSize)(y + row) * pitch + (VkDeviceSize)x * 4,
			(VkDeviceSize)w * 4, color);
	}
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	CHECK(vkEndCommandBuffer(commands));

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	start = now_ms();
	CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE,
		5000ull * 1000000);
	printf("fill of %ux%u at (%u, %u) with 0x%08x: %s after %.2f ms\n", w, h,
		x, y, color, result == VK_SUCCESS ? "done" : "FAILED",
		now_ms() - start);
	int ok = result == VK_SUCCESS;

	// the frame buffer is write-combined: no cache maintenance to do
	unsigned wrong = 0;
	for (unsigned row = 0; row < h && ok && !shim; row++) {
		const uint8_t* line = pixels + (size_t)(y + row) * pitch
			+ (size_t)x * 4;
		for (unsigned column = 0; column < w; column++) {
			uint32_t value;
			memcpy(&value, line + column * 4, 4);
			if (value != color) {
				if (wrong < 8) {
					printf("  pixel (%u, %u): 0x%08x\n", x + column, y + row,
						value);
				}
				wrong++;
			}
		}
	}
	if (shim)
		printf("the shim runs nothing: pixels not checked\n");
	else
		printf("%u of %u pixels wrong\n", wrong, w * h);
	// the GPU must not write there, but app_server (a cursor) may have
	for (int i = 0; i < 4 && !shim; i++) {
		uint32_t value;
		memcpy(&value, pixels + (size_t)around[i].y * pitch
			+ (size_t)around[i].x * 4, 4);
		if (value != around[i].value) {
			printf("  warning: pixel (%u, %u) beside the rectangle changed: "
				"0x%08x -> 0x%08x%s\n", around[i].x, around[i].y,
				around[i].value, value,
				value == color ? " (the fill colour: the GPU wrote it)" : "");
			if (value == color)
				ok = 0;
		}
	}
	ok = ok && wrong == 0;

	vkUnmapMemory(sDevice, frameBuffer);
	vkDestroyFence(sDevice, fence, NULL);
	vkDestroyCommandPool(sDevice, pool, NULL);
	vkDestroyBuffer(sDevice, buffer, NULL);
	vkFreeMemory(sDevice, frameBuffer, NULL);
	vkDestroyDevice(sDevice, NULL);
	vkDestroyInstance(sInstance, NULL);
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
