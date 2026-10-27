/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Sync objects without GPU work, through Mesa's PowerVR Vulkan driver.
// A vkQueueSubmit with no command buffers that signals a fence and a
// timeline semaphore value takes the driver's null-job path: a signalled
// temporary syncobj is transferred into the fence's (point 0) and into the
// semaphore's timeline point (DRM SYNCOBJ_CREATE, SYNCOBJ_TRANSFER,
// SYNCOBJ_DESTROY). Then it waits on both with a timeout, checks the
// counter, does a second submit that waits on point 1 and signals point 2,
// signals point 3 from the host, and checks that waits which cannot finish
// time out (VK_TIMEOUT, i.e. ETIME from the kernel).
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vkfence [timeout-ms]
// (default 1000 ms for the waits that must succeed)


#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

static VkInstance sInstance;
static VkDevice sDevice;
static PFN_vkGetDeviceProcAddr sGetDeviceProcAddr;
static int sFailures;

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
			printf("FAIL: " #call ": %s\n", result_name(result_)); \
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


static const char*
result_name(VkResult result)
{
	switch (result) {
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
		default: return "(other)";
	}
}


// one step: what it did, what it got, what it should have got, how long
static void
expect(const char* what, VkResult result, VkResult wanted, double ms)
{
	int ok = result == wanted;
	printf("  %-4s %-52s %-12s (want %s) %8.3f ms\n", ok ? "ok" : "FAIL", what,
		result_name(result), result_name(wanted), ms);
	if (!ok)
		sFailures++;
}


static void
expect_value(const char* what, VkResult result, uint64_t value,
	uint64_t wanted)
{
	int ok = result == VK_SUCCESS && value == wanted;
	printf("  %-4s %-52s %-12s value %llu (want %llu)\n", ok ? "ok" : "FAIL",
		what, result_name(result), (unsigned long long)value,
		(unsigned long long)wanted);
	if (!ok)
		sFailures++;
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	uint64_t timeoutNs = (argc > 1 ? strtoull(argv[1], NULL, 0) : 1000)
		* 1000000ull;
	const uint64_t shortNs = 10 * 1000000ull;

	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	if (vkCreateInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vkfence",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceFeatures2);
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

	VkPhysicalDeviceVulkan12Features features12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features12,
	};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	if (!features12.timelineSemaphore) {
		printf("FAIL: no timelineSemaphore feature\n");
		return 1;
	}

	VkPhysicalDeviceVulkan12Features enable12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = VK_TRUE,
	};
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
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
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkGetFenceStatus);
	DEVICE_FN(vkResetFences);
	DEVICE_FN(vkWaitForFences);
	DEVICE_FN(vkDestroyFence);
	DEVICE_FN(vkCreateSemaphore);
	DEVICE_FN(vkGetSemaphoreCounterValue);
	DEVICE_FN(vkWaitSemaphores);
	DEVICE_FN(vkSignalSemaphore);
	DEVICE_FN(vkDestroySemaphore);
	DEVICE_FN(vkQueueSubmit);
	DEVICE_FN(vkQueueWaitIdle);
	DEVICE_FN(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(sDevice, 0, 0, &queue);

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));

	VkSemaphoreTypeCreateInfo timelineInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		.initialValue = 0,
	};
	VkSemaphoreCreateInfo semaphoreInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timelineInfo,
	};
	VkSemaphore timeline;
	CHECK(vkCreateSemaphore(sDevice, &semaphoreInfo, NULL, &timeline));

	uint64_t value = ~0ull;
	double start;

	printf("before any submit\n");
	start = now_ms();
	result = vkGetFenceStatus(sDevice, fence);
	expect("vkGetFenceStatus", result, VK_NOT_READY, now_ms() - start);
	result = vkGetSemaphoreCounterValue(sDevice, timeline, &value);
	expect_value("vkGetSemaphoreCounterValue", result, value, 0);
	start = now_ms();
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE, shortNs);
	expect("vkWaitForFences, 10 ms, nothing submitted", result, VK_TIMEOUT,
		now_ms() - start);

	// 1: no command buffers; signal the fence and timeline point 1
	printf("submit 1: no command buffers, signals fence + timeline 1\n");
	uint64_t signalValue = 1;
	VkTimelineSemaphoreSubmitInfo timelineSubmit = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &signalValue,
	};
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timelineSubmit,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &timeline,
	};
	start = now_ms();
	result = vkQueueSubmit(queue, 1, &submit, fence);
	expect("vkQueueSubmit", result, VK_SUCCESS, now_ms() - start);

	start = now_ms();
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE, timeoutNs);
	expect("vkWaitForFences", result, VK_SUCCESS, now_ms() - start);

	uint64_t waitValue = 1;
	VkSemaphoreWaitInfo waitInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &timeline,
		.pValues = &waitValue,
	};
	start = now_ms();
	result = vkWaitSemaphores(sDevice, &waitInfo, timeoutNs);
	expect("vkWaitSemaphores, value 1", result, VK_SUCCESS, now_ms() - start);
	result = vkGetSemaphoreCounterValue(sDevice, timeline, &value);
	expect_value("vkGetSemaphoreCounterValue", result, value, 1);
	start = now_ms();
	result = vkGetFenceStatus(sDevice, fence);
	expect("vkGetFenceStatus", result, VK_SUCCESS, now_ms() - start);

	// 2: wait on point 1, signal point 2 (the waits go through a binary
	// syncobj fed by SYNCOBJ_TRANSFER from the timeline point)
	printf("submit 2: waits timeline 1, signals fence + timeline 2\n");
	start = now_ms();
	result = vkResetFences(sDevice, 1, &fence);
	expect("vkResetFences", result, VK_SUCCESS, now_ms() - start);
	uint64_t wait1 = 1;
	uint64_t signal2 = 2;
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo timelineSubmit2 = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.waitSemaphoreValueCount = 1,
		.pWaitSemaphoreValues = &wait1,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &signal2,
	};
	VkSubmitInfo submit2 = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timelineSubmit2,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &timeline,
		.pWaitDstStageMask = &waitStage,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &timeline,
	};
	start = now_ms();
	result = vkQueueSubmit(queue, 1, &submit2, fence);
	expect("vkQueueSubmit", result, VK_SUCCESS, now_ms() - start);
	start = now_ms();
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE, timeoutNs);
	expect("vkWaitForFences", result, VK_SUCCESS, now_ms() - start);
	waitValue = 2;
	start = now_ms();
	result = vkWaitSemaphores(sDevice, &waitInfo, timeoutNs);
	expect("vkWaitSemaphores, value 2", result, VK_SUCCESS, now_ms() - start);
	result = vkGetSemaphoreCounterValue(sDevice, timeline, &value);
	expect_value("vkGetSemaphoreCounterValue", result, value, 2);

	// 3: the host signals point 3; point 10 never comes
	printf("host: signals timeline 3; waits that cannot finish\n");
	VkSemaphoreSignalInfo signalInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
		.semaphore = timeline,
		.value = 3,
	};
	start = now_ms();
	result = vkSignalSemaphore(sDevice, &signalInfo);
	expect("vkSignalSemaphore, value 3", result, VK_SUCCESS, now_ms() - start);
	result = vkGetSemaphoreCounterValue(sDevice, timeline, &value);
	expect_value("vkGetSemaphoreCounterValue", result, value, 3);
	waitValue = 10;
	start = now_ms();
	result = vkWaitSemaphores(sDevice, &waitInfo, shortNs);
	expect("vkWaitSemaphores, value 10, 10 ms", result, VK_TIMEOUT,
		now_ms() - start);
	start = now_ms();
	result = vkResetFences(sDevice, 1, &fence);
	expect("vkResetFences", result, VK_SUCCESS, now_ms() - start);
	start = now_ms();
	result = vkGetFenceStatus(sDevice, fence);
	expect("vkGetFenceStatus after reset", result, VK_NOT_READY,
		now_ms() - start);
	start = now_ms();
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE, shortNs);
	expect("vkWaitForFences, 10 ms, reset fence", result, VK_TIMEOUT,
		now_ms() - start);

	start = now_ms();
	result = vkQueueWaitIdle(queue);
	expect("vkQueueWaitIdle", result, VK_SUCCESS, now_ms() - start);

	vkDestroySemaphore(sDevice, timeline, NULL);
	vkDestroyFence(sDevice, fence, NULL);
	vkDestroyDevice(sDevice, NULL);
	vkDestroyInstance(sInstance, NULL);

	printf("%s (%d failure%s)\n", sFailures == 0 ? "PASS" : "FAIL", sFailures,
		sFailures == 1 ? "" : "s");
	return sFailures == 0 ? 0 : 1;
}
