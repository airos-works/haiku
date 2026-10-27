/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// pvr_vkfill's compute dispatch in a loop, straight on the Vulkan driver (no
// zink, no GL, no readback, no app_server): submit, wait, repeat. Every
// interval it prints one line: dispatches/s in that window, average ms per
// dispatch spent in vkQueueSubmit() and in the wait, the process's and the
// kernel's areas (as pvr_glbench), and a check of the buffer by one extra
// dispatch. If GL frames slow down over time and this does not, the
// slowdown is above the kernel and the Vulkan driver.
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vkbench [--seconds N]
//       [--interval S] [--dispatches N] [--timeline] [--rerecord] [--mark]
// --seconds: run time (default 300; 0 = until --dispatches or Ctrl+C);
// --interval: seconds per line (default 10); --dispatches: stop after N;
// --timeline: zink's way to wait, a timeline semaphore signalled by each
// submit (value n) and vkWaitSemaphores(), instead of a fence and
// vkWaitForFences() + vkResetFences(); --rerecord: reset the command pool
// and record the command buffer again for every dispatch, as zink does
// every batch, instead of recording it once; --mark: print
// "== dispatch N" before each one.


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "pvr_bench_areas.h"


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

#define BUFFER_SIZE		(1024 * 1024)
#define WORD_COUNT		(BUFFER_SIZE / 4)
#define LOCAL_SIZE		64
#define FILL_PATTERN	0xdeadbeefu
#define WAIT_TIMEOUT	(5000 * 1000000ull)

static VkInstance sInstance;
static VkDevice sDevice;
static PFN_vkGetDeviceProcAddr sGetDeviceProcAddr;
static volatile sig_atomic_t sStop;
static struct area_snapshot sOwnStart, sOwnNow, sKernelStart, sKernelNow;

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


// pvr_vkfill's shader (see there): data[i] = i * 3 + 1, 64 per workgroup
static const uint32_t kFillShader[] = {
	0x07230203, 0x00010000, 0x00070000, 0x00000019, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
	0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00060010, 0x00000001,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x00000002,
	0x0000000b, 0x0000001c, 0x00040047, 0x00000003, 0x00000006, 0x00000004,
	0x00050048, 0x00000004, 0x00000000, 0x00000023, 0x00000000, 0x00030047,
	0x00000004, 0x00000003, 0x00040047, 0x00000005, 0x00000022, 0x00000000,
	0x00040047, 0x00000005, 0x00000021, 0x00000000, 0x00020013, 0x00000006,
	0x00030021, 0x00000007, 0x00000006, 0x00040015, 0x00000008, 0x00000020,
	0x00000000, 0x00040015, 0x00000009, 0x00000020, 0x00000001, 0x00040017,
	0x0000000a, 0x00000008, 0x00000003, 0x00040020, 0x0000000b, 0x00000001,
	0x0000000a, 0x0004003b, 0x0000000b, 0x00000002, 0x00000001, 0x0003001d,
	0x00000003, 0x00000008, 0x0003001e, 0x00000004, 0x00000003, 0x00040020,
	0x0000000c, 0x00000002, 0x00000004, 0x0004003b, 0x0000000c, 0x00000005,
	0x00000002, 0x0004002b, 0x00000009, 0x0000000d, 0x00000000, 0x0004002b,
	0x00000008, 0x0000000e, 0x00000000, 0x0004002b, 0x00000008, 0x0000000f,
	0x00000001, 0x0004002b, 0x00000008, 0x00000010, 0x00000003, 0x00040020,
	0x00000011, 0x00000001, 0x00000008, 0x00040020, 0x00000012, 0x00000002,
	0x00000008, 0x00050036, 0x00000006, 0x00000001, 0x00000000, 0x00000007,
	0x000200f8, 0x00000013, 0x00050041, 0x00000011, 0x00000014, 0x00000002,
	0x0000000e, 0x0004003d, 0x00000008, 0x00000015, 0x00000014, 0x00050084,
	0x00000008, 0x00000016, 0x00000015, 0x00000010, 0x00050080, 0x00000008,
	0x00000017, 0x00000016, 0x0000000f, 0x00060041, 0x00000012, 0x00000018,
	0x00000005, 0x0000000d, 0x00000015, 0x0003003e, 0x00000018, 0x00000017,
	0x000100fd, 0x00010038,
};


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


static void
stop_handler(int number)
{
	sStop = 1;
}


struct window {
	unsigned	dispatches;
	double		submitMs;
	double		waitMs;
	double		start;
};


static void
report(const struct window* window, double now, double runStart,
	const char* check)
{
	double seconds = (now - window->start) / 1000.0;
	unsigned dispatches = window->dispatches != 0 ? window->dispatches : 1;
	area_snapshot_own(&sOwnNow);
	area_snapshot_kernel(&sKernelNow);
	printf("[%6.1f s] %6u dispatches %7.1f/s; ms/dispatch submit %.3f, "
		"wait %.3f", (now - runStart) / 1000.0, window->dispatches,
		window->dispatches / seconds, window->submitMs / dispatches,
		window->waitMs / dispatches);
	area_print(stdout, "areas", &sOwnNow);
	area_print(stdout, "kernel", &sKernelNow);
	printf("; check %s\n", check);
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	double seconds = 300, interval = 10;
	unsigned maxDispatches = 0;
	int useTimeline = 0, rerecord = 0, mark = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atof(argv[++i]);
		else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
			interval = atof(argv[++i]);
		else if (strcmp(argv[i], "--dispatches") == 0 && i + 1 < argc)
			maxDispatches = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--timeline") == 0)
			useTimeline = 1;
		else if (strcmp(argv[i], "--rerecord") == 0)
			rerecord = 1;
		else if (strcmp(argv[i], "--mark") == 0)
			mark = 1;
		else {
			printf("usage: %s [--seconds N] [--interval S] [--dispatches N] "
				"[--timeline] [--rerecord] [--mark]\n", argv[0]);
			return 2;
		}
	}
	if (interval <= 0)
		interval = 10;
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);

	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	if (vkCreateInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vkbench",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties);
	INSTANCE_FN(vkGetPhysicalDeviceFeatures2);
	INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
	INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
	INSTANCE_FN(vkCreateDevice);
	INSTANCE_FN(vkGetDeviceProcAddr);
	INSTANCE_FN(vkDestroyInstance);
	sGetDeviceProcAddr = vkGetDeviceProcAddr;

	uint32_t deviceCount = 1;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkResult result = vkEnumeratePhysicalDevices(sInstance, &deviceCount,
		&physicalDevice);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || deviceCount == 0) {
		printf("FAIL: no physical device (%d); see pvr_vkprobe\n", (int)result);
		return 1;
	}
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	printf("device: %s\n", properties.deviceName);

	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
		NULL);
	VkQueueFamilyProperties families[8];
	if (familyCount > 8)
		familyCount = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
		families);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < familyCount; i++) {
		if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			family = i;
			break;
		}
	}
	if (family == UINT32_MAX) {
		printf("FAIL: no compute queue family\n");
		return 1;
	}

	VkPhysicalDeviceVulkan12Features features12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features12,
	};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	if (useTimeline && !features12.timelineSemaphore) {
		printf("FAIL: no timelineSemaphore feature\n");
		return 1;
	}
	VkPhysicalDeviceVulkan12Features enable12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = useTimeline ? VK_TRUE : VK_FALSE,
	};
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &enable12,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	CHECK(vkCreateDevice(physicalDevice, &deviceInfo, NULL, &sDevice));

	DEVICE_FN(vkGetDeviceQueue);
	DEVICE_FN(vkCreateBuffer);
	DEVICE_FN(vkGetBufferMemoryRequirements);
	DEVICE_FN(vkAllocateMemory);
	DEVICE_FN(vkBindBufferMemory);
	DEVICE_FN(vkMapMemory);
	DEVICE_FN(vkUnmapMemory);
	DEVICE_FN(vkCreateDescriptorSetLayout);
	DEVICE_FN(vkCreatePipelineLayout);
	DEVICE_FN(vkCreateDescriptorPool);
	DEVICE_FN(vkAllocateDescriptorSets);
	DEVICE_FN(vkUpdateDescriptorSets);
	DEVICE_FN(vkCreateShaderModule);
	DEVICE_FN(vkCreateComputePipelines);
	DEVICE_FN(vkCreateCommandPool);
	DEVICE_FN(vkResetCommandPool);
	DEVICE_FN(vkAllocateCommandBuffers);
	DEVICE_FN(vkBeginCommandBuffer);
	DEVICE_FN(vkCmdBindPipeline);
	DEVICE_FN(vkCmdBindDescriptorSets);
	DEVICE_FN(vkCmdDispatch);
	DEVICE_FN(vkCmdPipelineBarrier);
	DEVICE_FN(vkEndCommandBuffer);
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkResetFences);
	DEVICE_FN(vkCreateSemaphore);
	DEVICE_FN(vkDestroySemaphore);
	DEVICE_FN(vkWaitSemaphores);
	DEVICE_FN(vkQueueSubmit);
	DEVICE_FN(vkWaitForFences);
	DEVICE_FN(vkDestroyFence);
	DEVICE_FN(vkDestroyCommandPool);
	DEVICE_FN(vkDestroyPipeline);
	DEVICE_FN(vkDestroyShaderModule);
	DEVICE_FN(vkDestroyDescriptorPool);
	DEVICE_FN(vkDestroyPipelineLayout);
	DEVICE_FN(vkDestroyDescriptorSetLayout);
	DEVICE_FN(vkFreeMemory);
	DEVICE_FN(vkDestroyBuffer);
	DEVICE_FN(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(sDevice, family, 0, &queue);

	// the buffer, in host visible, coherent memory
	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = BUFFER_SIZE,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkBuffer buffer;
	CHECK(vkCreateBuffer(sDevice, &bufferInfo, NULL, &buffer));
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(sDevice, buffer, &requirements);
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t memoryType = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0
			&& (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
			memoryType = i;
			break;
		}
	}
	if (memoryType == UINT32_MAX) {
		printf("FAIL: no HOST_VISIBLE | HOST_COHERENT memory type\n");
		return 1;
	}
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = memoryType,
	};
	VkDeviceMemory deviceMemory;
	CHECK(vkAllocateMemory(sDevice, &allocateInfo, NULL, &deviceMemory));
	CHECK(vkBindBufferMemory(sDevice, buffer, deviceMemory, 0));
	uint32_t* words;
	CHECK(vkMapMemory(sDevice, deviceMemory, 0, VK_WHOLE_SIZE, 0,
		(void**)&words));

	// the pipeline
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo setLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkDescriptorSetLayout setLayout;
	CHECK(vkCreateDescriptorSetLayout(sDevice, &setLayoutInfo, NULL,
		&setLayout));
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &setLayout,
	};
	VkPipelineLayout pipelineLayout;
	CHECK(vkCreatePipelineLayout(sDevice, &pipelineLayoutInfo, NULL,
		&pipelineLayout));
	VkDescriptorPoolSize poolSize = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
	};
	VkDescriptorPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &poolSize,
	};
	VkDescriptorPool descriptorPool;
	CHECK(vkCreateDescriptorPool(sDevice, &poolInfo, NULL, &descriptorPool));
	VkDescriptorSetAllocateInfo setInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &setLayout,
	};
	VkDescriptorSet descriptorSet;
	CHECK(vkAllocateDescriptorSets(sDevice, &setInfo, &descriptorSet));
	VkDescriptorBufferInfo descriptorBuffer = {
		.buffer = buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &descriptorBuffer,
	};
	vkUpdateDescriptorSets(sDevice, 1, &write, 0, NULL);
	VkShaderModuleCreateInfo moduleInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(kFillShader),
		.pCode = kFillShader,
	};
	VkShaderModule module;
	CHECK(vkCreateShaderModule(sDevice, &moduleInfo, NULL, &module));
	VkComputePipelineCreateInfo pipelineInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		},
		.layout = pipelineLayout,
	};
	VkPipeline pipeline;
	CHECK(vkCreateComputePipelines(sDevice, VK_NULL_HANDLE, 1, &pipelineInfo,
		NULL, &pipeline));

	VkCommandPoolCreateInfo commandPoolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family,
	};
	VkCommandPool commandPool;
	CHECK(vkCreateCommandPool(sDevice, &commandPoolInfo, NULL, &commandPool));
	VkCommandBufferAllocateInfo commandBufferInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commandBuffer;
	CHECK(vkAllocateCommandBuffers(sDevice, &commandBufferInfo,
		&commandBuffer));
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = rerecord ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0,
	};
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore timeline = VK_NULL_HANDLE;
	if (useTimeline) {
		VkSemaphoreTypeCreateInfo typeInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue = 0,
		};
		VkSemaphoreCreateInfo semaphoreInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &typeInfo,
		};
		CHECK(vkCreateSemaphore(sDevice, &semaphoreInfo, NULL, &timeline));
	} else
		CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));
	printf("%u words per dispatch; waits on a %s; command buffer %s\n",
		(unsigned)WORD_COUNT, useTimeline ? "timeline semaphore" : "fence",
		rerecord ? "recorded for every dispatch" : "recorded once");

	uint64_t value = 0;
	unsigned total = 0, failedChecks = 0;
	int ok = 1;
	double runStart = now_ms();
	struct window window = { 0, 0, 0, runStart };
	int recorded = 0;
	area_snapshot_own(&sOwnStart);
	area_snapshot_kernel(&sKernelStart);
	for (unsigned dispatch = 0;; dispatch++) {
		double start = now_ms();
		int last = sStop || (maxDispatches != 0 && dispatch >= maxDispatches)
			|| (seconds > 0 && start - runStart >= seconds * 1000);
		// the window ends: one more dispatch, checked and not timed
		int check = last || start - window.start >= interval * 1000;
		if (last && window.dispatches == 0 && dispatch != 0)
			break;
		if (mark)
			printf("== dispatch %u%s\n", dispatch, check ? " (check)" : "");
		if (check) {
			for (uint32_t i = 0; i < WORD_COUNT; i++)
				words[i] = FILL_PATTERN;
		}

		if (rerecord || !recorded) {
			if (rerecord)
				CHECK(vkResetCommandPool(sDevice, commandPool, 0));
			CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
				pipeline);
			vkCmdBindDescriptorSets(commandBuffer,
				VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
				&descriptorSet, 0, NULL);
			vkCmdDispatch(commandBuffer, WORD_COUNT / LOCAL_SIZE, 1, 1);
			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0,
				NULL);
			CHECK(vkEndCommandBuffer(commandBuffer));
			recorded = 1;
		}

		value++;
		VkTimelineSemaphoreSubmitInfo timelineSubmit = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.signalSemaphoreValueCount = 1,
			.pSignalSemaphoreValues = &value,
		};
		VkSubmitInfo submitInfo = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = useTimeline ? &timelineSubmit : NULL,
			.commandBufferCount = 1,
			.pCommandBuffers = &commandBuffer,
			.signalSemaphoreCount = useTimeline ? 1 : 0,
			.pSignalSemaphores = &timeline,
		};
		double submitStart = now_ms();
		result = vkQueueSubmit(queue, 1, &submitInfo, fence);
		if (result != VK_SUCCESS) {
			printf("FAIL: vkQueueSubmit: %d (dispatch %u)\n", (int)result,
				dispatch);
			ok = 0;
			break;
		}
		double submitted = now_ms();
		if (useTimeline) {
			VkSemaphoreWaitInfo waitInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores = &timeline,
				.pValues = &value,
			};
			result = vkWaitSemaphores(sDevice, &waitInfo, WAIT_TIMEOUT);
		} else {
			result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE,
				WAIT_TIMEOUT);
			if (result == VK_SUCCESS)
				result = vkResetFences(sDevice, 1, &fence);
		}
		double done = now_ms();
		if (result != VK_SUCCESS) {
			printf("FAIL: %s: %d (dispatch %u)\n", useTimeline
				? "vkWaitSemaphores" : "vkWaitForFences", (int)result,
				dispatch);
			ok = 0;
			break;
		}
		total++;

		if (!check) {
			window.dispatches++;
			window.submitMs += submitted - submitStart;
			window.waitMs += done - submitted;
			continue;
		}
		uint32_t wrong = 0;
		for (uint32_t i = 0; i < WORD_COUNT; i++) {
			if (words[i] != i * 3 + 1)
				wrong++;
		}
		char checkText[64];
		if (wrong == 0)
			snprintf(checkText, sizeof(checkText), "ok");
		else {
			snprintf(checkText, sizeof(checkText), "%u of %u words wrong",
				wrong, (unsigned)WORD_COUNT);
			failedChecks++;
		}
		report(&window, done, runStart, checkText);
		window = (struct window){ 0, 0, 0, now_ms() };
		if (last)
			break;
	}
	double end = now_ms();
	printf("%u dispatches in %.1f s, %.1f/s\n", total,
		(end - runStart) / 1000.0, total / ((end - runStart) / 1000.0));
	area_snapshot_own(&sOwnNow);
	area_snapshot_kernel(&sKernelNow);
	area_print_changes(stdout, "process areas since the start", &sOwnStart,
		&sOwnNow);
	area_print_changes(stdout, "kernel powervr areas since the start",
		&sKernelStart, &sKernelNow);

	if (timeline != VK_NULL_HANDLE)
		vkDestroySemaphore(sDevice, timeline, NULL);
	if (fence != VK_NULL_HANDLE)
		vkDestroyFence(sDevice, fence, NULL);
	vkDestroyCommandPool(sDevice, commandPool, NULL);
	vkDestroyPipeline(sDevice, pipeline, NULL);
	vkDestroyShaderModule(sDevice, module, NULL);
	vkDestroyDescriptorPool(sDevice, descriptorPool, NULL);
	vkDestroyPipelineLayout(sDevice, pipelineLayout, NULL);
	vkDestroyDescriptorSetLayout(sDevice, setLayout, NULL);
	vkUnmapMemory(sDevice, deviceMemory);
	vkFreeMemory(sDevice, deviceMemory, NULL);
	vkDestroyBuffer(sDevice, buffer, NULL);
	vkDestroyDevice(sDevice, NULL);
	vkDestroyInstance(sInstance, NULL);

	if (failedChecks != 0)
		ok = 0;
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
