/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// What Mesa's PowerVR Vulkan driver reports on air/OS: instance, physical
// device properties, limits, features, memory, queue families and device
// extensions; then a device with one queue is created and destroyed.
// There is no Vulkan loader on arm64 Haiku: the driver library is linked
// directly and everything starts from vk_icdGetInstanceProcAddr.
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vkprobe
// (HAIKU_PVR_DEVICE names another device node, PVR_DEBUG=info makes the
// driver dump what it knows about the core.)


#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

static VkInstance sInstance;

#define INSTANCE_FN(name) \
	PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(sInstance, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); exit(1); }


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
		case VK_INCOMPLETE: return "VK_INCOMPLETE";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
		default: return "(other)";
	}
}

#define CHECK(call) \
	do { \
		VkResult result_ = (call); \
		if (result_ != VK_SUCCESS) { \
			printf("FAIL: " #call ": %s (%d)\n", result_name(result_), \
				(int)result_); \
			exit(1); \
		} \
	} while (0)


static void
print_version(const char* label, uint32_t version)
{
	printf("  %-28s %u.%u.%u\n", label, VK_API_VERSION_MAJOR(version),
		VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}


static void
print_uuid(const char* label, const uint8_t* uuid, size_t size)
{
	printf("  %-28s ", label);
	for (size_t i = 0; i < size; i++)
		printf("%02x%s", uuid[i],
			i == 3 || i == 5 || i == 7 || i == 9 ? "-" : "");
	printf("\n");
}


static const char*
device_type_name(VkPhysicalDeviceType type)
{
	switch (type) {
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
		case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
		default: return "other";
	}
}


struct feature_name {
	const char* name;
	size_t offset;
};

#define F10(field) { #field, offsetof(VkPhysicalDeviceFeatures, field) }
static const struct feature_name kFeatures10[] = {
	F10(robustBufferAccess), F10(fullDrawIndexUint32), F10(imageCubeArray),
	F10(independentBlend), F10(geometryShader), F10(tessellationShader),
	F10(sampleRateShading), F10(dualSrcBlend), F10(logicOp),
	F10(multiDrawIndirect), F10(drawIndirectFirstInstance), F10(depthClamp),
	F10(depthBiasClamp), F10(fillModeNonSolid), F10(depthBounds),
	F10(wideLines), F10(largePoints), F10(alphaToOne), F10(multiViewport),
	F10(samplerAnisotropy), F10(textureCompressionETC2),
	F10(textureCompressionASTC_LDR), F10(textureCompressionBC),
	F10(occlusionQueryPrecise), F10(pipelineStatisticsQuery),
	F10(vertexPipelineStoresAndAtomics), F10(fragmentStoresAndAtomics),
	F10(shaderTessellationAndGeometryPointSize), F10(shaderImageGatherExtended),
	F10(shaderStorageImageExtendedFormats), F10(shaderStorageImageMultisample),
	F10(shaderStorageImageReadWithoutFormat),
	F10(shaderStorageImageWriteWithoutFormat),
	F10(shaderUniformBufferArrayDynamicIndexing),
	F10(shaderSampledImageArrayDynamicIndexing),
	F10(shaderStorageBufferArrayDynamicIndexing),
	F10(shaderStorageImageArrayDynamicIndexing), F10(shaderClipDistance),
	F10(shaderCullDistance), F10(shaderFloat64), F10(shaderInt64),
	F10(shaderInt16), F10(shaderResourceResidency), F10(shaderResourceMinLod),
	F10(sparseBinding), F10(sparseResidencyBuffer),
	F10(sparseResidencyImage2D), F10(sparseResidencyImage3D),
	F10(sparseResidency2Samples), F10(sparseResidency4Samples),
	F10(sparseResidency8Samples), F10(sparseResidency16Samples),
	F10(sparseResidencyAliased), F10(variableMultisampleRate),
	F10(inheritedQueries),
};

#define F11(field) { #field, offsetof(VkPhysicalDeviceVulkan11Features, field) }
static const struct feature_name kFeatures11[] = {
	F11(storageBuffer16BitAccess), F11(uniformAndStorageBuffer16BitAccess),
	F11(storagePushConstant16), F11(storageInputOutput16), F11(multiview),
	F11(multiviewGeometryShader), F11(multiviewTessellationShader),
	F11(variablePointersStorageBuffer), F11(variablePointers),
	F11(protectedMemory), F11(samplerYcbcrConversion),
	F11(shaderDrawParameters),
};

#define F12(field) { #field, offsetof(VkPhysicalDeviceVulkan12Features, field) }
static const struct feature_name kFeatures12[] = {
	F12(samplerMirrorClampToEdge), F12(drawIndirectCount),
	F12(storageBuffer8BitAccess), F12(uniformAndStorageBuffer8BitAccess),
	F12(storagePushConstant8), F12(shaderBufferInt64Atomics),
	F12(shaderSharedInt64Atomics), F12(shaderFloat16), F12(shaderInt8),
	F12(descriptorIndexing), F12(shaderInputAttachmentArrayDynamicIndexing),
	F12(shaderUniformTexelBufferArrayDynamicIndexing),
	F12(shaderStorageTexelBufferArrayDynamicIndexing),
	F12(shaderUniformBufferArrayNonUniformIndexing),
	F12(shaderSampledImageArrayNonUniformIndexing),
	F12(shaderStorageBufferArrayNonUniformIndexing),
	F12(shaderStorageImageArrayNonUniformIndexing),
	F12(shaderInputAttachmentArrayNonUniformIndexing),
	F12(shaderUniformTexelBufferArrayNonUniformIndexing),
	F12(shaderStorageTexelBufferArrayNonUniformIndexing),
	F12(descriptorBindingUniformBufferUpdateAfterBind),
	F12(descriptorBindingSampledImageUpdateAfterBind),
	F12(descriptorBindingStorageImageUpdateAfterBind),
	F12(descriptorBindingStorageBufferUpdateAfterBind),
	F12(descriptorBindingUniformTexelBufferUpdateAfterBind),
	F12(descriptorBindingStorageTexelBufferUpdateAfterBind),
	F12(descriptorBindingUpdateUnusedWhilePending),
	F12(descriptorBindingPartiallyBound),
	F12(descriptorBindingVariableDescriptorCount),
	F12(runtimeDescriptorArray), F12(samplerFilterMinmax),
	F12(scalarBlockLayout), F12(imagelessFramebuffer),
	F12(uniformBufferStandardLayout), F12(shaderSubgroupExtendedTypes),
	F12(separateDepthStencilLayouts), F12(hostQueryReset),
	F12(timelineSemaphore), F12(bufferDeviceAddress),
	F12(bufferDeviceAddressCaptureReplay),
	F12(bufferDeviceAddressMultiDevice), F12(vulkanMemoryModel),
	F12(vulkanMemoryModelDeviceScope),
	F12(vulkanMemoryModelAvailabilityVisibilityChains),
	F12(shaderOutputViewportIndex), F12(shaderOutputLayer),
	F12(subgroupBroadcastDynamicId),
};


static void
print_features(const char* label, const void* features,
	const struct feature_name* names, size_t count)
{
	size_t supported = 0;
	printf("  %s:\n   ", label);
	int column = 3;
	for (size_t i = 0; i < count; i++) {
		VkBool32 value = *(const VkBool32*)((const char*)features
			+ names[i].offset);
		if (!value)
			continue;
		supported++;
		int length = (int)strlen(names[i].name) + 1;
		if (column + length > 78) {
			printf("\n   ");
			column = 3;
		}
		printf(" %s", names[i].name);
		column += length;
	}
	printf("\n    (%zu of %zu)\n   not supported:", supported, count);
	column = 18;
	for (size_t i = 0; i < count; i++) {
		VkBool32 value = *(const VkBool32*)((const char*)features
			+ names[i].offset);
		if (value)
			continue;
		int length = (int)strlen(names[i].name) + 1;
		if (column + length > 78) {
			printf("\n   ");
			column = 3;
		}
		printf(" %s", names[i].name);
		column += length;
	}
	printf("\n");
}


static void
print_memory_flags(VkMemoryPropertyFlags flags)
{
	if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) printf(" DEVICE_LOCAL");
	if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) printf(" HOST_VISIBLE");
	if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) printf(" HOST_COHERENT");
	if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) printf(" HOST_CACHED");
	if (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
		printf(" LAZILY_ALLOCATED");
	if (flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) printf(" PROTECTED");
}


static void
print_limits(const VkPhysicalDeviceLimits* l,
	const VkPhysicalDeviceVulkan11Properties* p11)
{
	printf("Limits\n");
#define L(name, format) printf("  %-40s " format "\n", #name, l->name)
	L(maxImageDimension1D, "%u");
	L(maxImageDimension2D, "%u");
	L(maxImageDimension3D, "%u");
	L(maxImageArrayLayers, "%u");
	L(maxTexelBufferElements, "%u");
	L(maxUniformBufferRange, "%u");
	L(maxStorageBufferRange, "%u");
	L(maxPushConstantsSize, "%u");
	L(maxMemoryAllocationCount, "%u");
	L(maxSamplerAllocationCount, "%u");
	printf("  %-40s %llu\n", "bufferImageGranularity",
		(unsigned long long)l->bufferImageGranularity);
	L(maxBoundDescriptorSets, "%u");
	L(maxPerStageResources, "%u");
	L(maxDescriptorSetStorageBuffers, "%u");
	L(maxDescriptorSetUniformBuffers, "%u");
	L(maxDescriptorSetSampledImages, "%u");
	L(maxVertexInputAttributes, "%u");
	L(maxFragmentOutputAttachments, "%u");
	L(maxComputeSharedMemorySize, "%u");
	printf("  %-40s %u x %u x %u\n", "maxComputeWorkGroupCount",
		l->maxComputeWorkGroupCount[0], l->maxComputeWorkGroupCount[1],
		l->maxComputeWorkGroupCount[2]);
	L(maxComputeWorkGroupInvocations, "%u");
	printf("  %-40s %u x %u x %u\n", "maxComputeWorkGroupSize",
		l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
		l->maxComputeWorkGroupSize[2]);
	L(maxSamplerAnisotropy, "%.1f");
	L(maxViewports, "%u");
	printf("  %-40s %u x %u\n", "maxViewportDimensions",
		l->maxViewportDimensions[0], l->maxViewportDimensions[1]);
	printf("  %-40s %llu\n", "minMemoryMapAlignment",
		(unsigned long long)l->minMemoryMapAlignment);
	printf("  %-40s %llu\n", "minUniformBufferOffsetAlignment",
		(unsigned long long)l->minUniformBufferOffsetAlignment);
	printf("  %-40s %llu\n", "minStorageBufferOffsetAlignment",
		(unsigned long long)l->minStorageBufferOffsetAlignment);
	L(maxFramebufferWidth, "%u");
	L(maxFramebufferHeight, "%u");
	L(framebufferColorSampleCounts, "0x%x");
	L(maxColorAttachments, "%u");
	L(timestampComputeAndGraphics, "%u");
	L(timestampPeriod, "%.3f");
	printf("  %-40s %llu\n", "optimalBufferCopyRowPitchAlignment",
		(unsigned long long)l->optimalBufferCopyRowPitchAlignment);
	printf("  %-40s %llu\n", "nonCoherentAtomSize",
		(unsigned long long)l->nonCoherentAtomSize);
#undef L
	printf("  %-40s %u (stages 0x%x, operations 0x%x)\n", "subgroupSize",
		p11->subgroupSize, p11->subgroupSupportedStages,
		p11->subgroupSupportedOperations);
	printf("  %-40s %llu\n", "maxMemoryAllocationSize",
		(unsigned long long)p11->maxMemoryAllocationSize);
}


int
main(void)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("pvr_vkprobe: PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=%s "
		"HAIKU_PVR_DEVICE=%s\n",
		getenv("PVR_I_WANT_A_BROKEN_VULKAN_DRIVER") != NULL
			? getenv("PVR_I_WANT_A_BROKEN_VULKAN_DRIVER") : "(unset)",
		getenv("HAIKU_PVR_DEVICE") != NULL
			? getenv("HAIKU_PVR_DEVICE") : "(unset: /dev/graphics/powervr/0)");

	PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion
		= (PFN_vkEnumerateInstanceVersion)vk_icdGetInstanceProcAddr(NULL,
			"vkEnumerateInstanceVersion");
	PFN_vkEnumerateInstanceExtensionProperties
		vkEnumerateInstanceExtensionProperties
		= (PFN_vkEnumerateInstanceExtensionProperties)
			vk_icdGetInstanceProcAddr(NULL,
				"vkEnumerateInstanceExtensionProperties");
	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	if (vkEnumerateInstanceVersion == NULL || vkCreateInstance == NULL
		|| vkEnumerateInstanceExtensionProperties == NULL) {
		printf("FAIL: no global entry points\n");
		return 1;
	}

	uint32_t instanceVersion = 0;
	CHECK(vkEnumerateInstanceVersion(&instanceVersion));
	printf("Instance\n");
	print_version("instance version", instanceVersion);

	uint32_t count = 0;
	CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
	VkExtensionProperties* instanceExtensions
		= calloc(count, sizeof(VkExtensionProperties));
	CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count,
		instanceExtensions));
	printf("  instance extensions (%u):", count);
	for (uint32_t i = 0; i < count; i++)
		printf("%s%s", i % 2 == 0 ? "\n    " : "  ",
			instanceExtensions[i].extensionName);
	printf("\n");
	free(instanceExtensions);

	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vkprobe",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	double start = now_ms();
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));
	printf("  vkCreateInstance: %.2f ms\n", now_ms() - start);

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties2);
	INSTANCE_FN(vkGetPhysicalDeviceFeatures2);
	INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
	INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
	INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
	INSTANCE_FN(vkCreateDevice);
	INSTANCE_FN(vkGetDeviceProcAddr);
	INSTANCE_FN(vkDestroyInstance);

	start = now_ms();
	uint32_t deviceCount = 0;
	CHECK(vkEnumeratePhysicalDevices(sInstance, &deviceCount, NULL));
	printf("  vkEnumeratePhysicalDevices: %u device(s), %.2f ms\n",
		deviceCount, now_ms() - start);
	if (deviceCount == 0) {
		printf("FAIL: no physical device. Is the powervr driver loaded "
			"(/dev/graphics/powervr/0), and is "
			"PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 set? MESA_DEBUG=1 or "
			"MESA_LOG_LEVEL=debug says why.\n");
		vkDestroyInstance(sInstance, NULL);
		return 1;
	}
	VkPhysicalDevice* devices = calloc(deviceCount, sizeof(VkPhysicalDevice));
	CHECK(vkEnumeratePhysicalDevices(sInstance, &deviceCount, devices));
	VkPhysicalDevice physicalDevice = devices[0];
	free(devices);

	// properties
	VkPhysicalDeviceDrmPropertiesEXT drm = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceVulkan12Properties p12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
		.pNext = &drm,
	};
	VkPhysicalDeviceVulkan11Properties p11 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
		.pNext = &p12,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &p11,
	};
	vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
	const VkPhysicalDeviceProperties* p = &properties.properties;

	printf("Physical device 0\n");
	printf("  %-28s %s\n", "name", p->deviceName);
	printf("  %-28s %s\n", "type", device_type_name(p->deviceType));
	print_version("API version", p->apiVersion);
	printf("  %-28s 0x%08x\n", "driver version", p->driverVersion);
	printf("  %-28s 0x%04x\n", "vendor ID", p->vendorID);
	printf("  %-28s 0x%08x\n", "device ID", p->deviceID);
	printf("  %-28s %d\n", "driver ID", (int)p12.driverID);
	printf("  %-28s %s\n", "driver name", p12.driverName);
	printf("  %-28s %s\n", "driver info", p12.driverInfo);
	printf("  %-28s %u.%u.%u.%u\n", "conformance version",
		p12.conformanceVersion.major, p12.conformanceVersion.minor,
		p12.conformanceVersion.subminor, p12.conformanceVersion.patch);
	print_uuid("device UUID", p11.deviceUUID, VK_UUID_SIZE);
	print_uuid("driver UUID", p11.driverUUID, VK_UUID_SIZE);
	print_uuid("pipeline cache UUID", p->pipelineCacheUUID, VK_UUID_SIZE);
	printf("  %-28s primary %s %lld:%lld, render %s %lld:%lld\n", "DRM nodes",
		drm.hasPrimary ? "yes" : "no", (long long)drm.primaryMajor,
		(long long)drm.primaryMinor, drm.hasRender ? "yes" : "no",
		(long long)drm.renderMajor, (long long)drm.renderMinor);
	printf("  %-28s 0x%x\n", "denorm/rounding independence",
		p12.denormBehaviorIndependence | (p12.roundingModeIndependence << 4));
	printf("  %-28s RTE16 %u RTE32 %u RTZ16 %u RTZ32 %u\n", "float controls",
		p12.shaderRoundingModeRTEFloat16, p12.shaderRoundingModeRTEFloat32,
		p12.shaderRoundingModeRTZFloat16, p12.shaderRoundingModeRTZFloat32);
	printf("  %-28s %llu\n", "maxTimelineSemaphoreValueDifference",
		(unsigned long long)p12.maxTimelineSemaphoreValueDifference);

	print_limits(&p->limits, &p11);

	// features
	VkPhysicalDeviceVulkan12Features f12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	};
	VkPhysicalDeviceVulkan11Features f11 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
		.pNext = &f12,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &f11,
	};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	printf("Features\n");
	print_features("Vulkan 1.0", &features.features, kFeatures10,
		sizeof(kFeatures10) / sizeof(kFeatures10[0]));
	print_features("Vulkan 1.1", &f11, kFeatures11,
		sizeof(kFeatures11) / sizeof(kFeatures11[0]));
	print_features("Vulkan 1.2", &f12, kFeatures12,
		sizeof(kFeatures12) / sizeof(kFeatures12[0]));

	// memory
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
	printf("Memory\n");
	for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
		printf("  heap %u: %llu MiB%s\n", i,
			(unsigned long long)(memory.memoryHeaps[i].size >> 20),
			memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
				? " DEVICE_LOCAL" : "");
	}
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		printf("  type %u: heap %u,", i, memory.memoryTypes[i].heapIndex);
		print_memory_flags(memory.memoryTypes[i].propertyFlags);
		printf("\n");
	}

	// queue families
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
		NULL);
	VkQueueFamilyProperties* families
		= calloc(familyCount, sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
		families);
	printf("Queue families\n");
	for (uint32_t i = 0; i < familyCount; i++) {
		printf("  family %u: %u queue(s),%s%s%s%s timestamp bits %u, "
			"granularity %ux%ux%u\n", i, families[i].queueCount,
			families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ? " GRAPHICS" : "",
			families[i].queueFlags & VK_QUEUE_COMPUTE_BIT ? " COMPUTE" : "",
			families[i].queueFlags & VK_QUEUE_TRANSFER_BIT ? " TRANSFER" : "",
			families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT
				? " SPARSE" : "",
			families[i].timestampValidBits,
			families[i].minImageTransferGranularity.width,
			families[i].minImageTransferGranularity.height,
			families[i].minImageTransferGranularity.depth);
	}
	free(families);

	// device extensions
	count = 0;
	CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count,
		NULL));
	VkExtensionProperties* extensions
		= calloc(count, sizeof(VkExtensionProperties));
	CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count,
		extensions));
	printf("Device extensions (%u)", count);
	for (uint32_t i = 0; i < count; i++) {
		printf("%s%-44s v%u", i % 2 == 0 ? "\n  " : "  ",
			extensions[i].extensionName, extensions[i].specVersion);
	}
	printf("\n");
	free(extensions);

	// a device with one queue
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
	VkDevice device;
	start = now_ms();
	CHECK(vkCreateDevice(physicalDevice, &deviceInfo, NULL, &device));
	printf("Device\n  vkCreateDevice: %.2f ms\n", now_ms() - start);

	PFN_vkGetDeviceQueue vkGetDeviceQueue
		= (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
	PFN_vkDeviceWaitIdle vkDeviceWaitIdle
		= (PFN_vkDeviceWaitIdle)vkGetDeviceProcAddr(device, "vkDeviceWaitIdle");
	PFN_vkDestroyDevice vkDestroyDevice
		= (PFN_vkDestroyDevice)vkGetDeviceProcAddr(device, "vkDestroyDevice");
	if (vkGetDeviceQueue == NULL || vkDeviceWaitIdle == NULL
		|| vkDestroyDevice == NULL) {
		printf("FAIL: no device entry points\n");
		return 1;
	}
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, 0, 0, &queue);
	printf("  queue %p\n", (void*)queue);
	// an empty submission and a wait on it (null job + syncobjs); reported,
	// not fatal: pvr_vkfence tests that path
	start = now_ms();
	VkResult idle = vkDeviceWaitIdle(device);
	printf("  vkDeviceWaitIdle: %s, %.2f ms\n", result_name(idle),
		now_ms() - start);

	start = now_ms();
	vkDestroyDevice(device, NULL);
	printf("  vkDestroyDevice: %.2f ms\n", now_ms() - start);
	vkDestroyInstance(sInstance, NULL);

	printf("PASS\n");
	return 0;
}
