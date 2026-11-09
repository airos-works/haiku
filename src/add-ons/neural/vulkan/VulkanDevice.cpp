/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VulkanDevice.h"

#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <neural/NeuralDefs.h>

#include "shaders/Shaders.inc.h"


/*!	How long submitted work may take before the device is given up. A
	program is a network run once: seconds at worst. */
static const uint64 kWaitTimeout = 60ull * 1000 * 1000 * 1000;


#define SHADER(name) { #name, kShader_##name, sizeof(kShader_##name) }

static const struct {
	const char*		name;
	const uint32*	code;
	size_t			size;
} kShaders[VULKAN_SHADER_COUNT] = {
	SHADER(conv),
	SHADER(conv_pointwise),
	SHADER(conv_quad),
	SHADER(depthwise),
	SHADER(depthwise_multiplier),
	SHADER(depthwise_grid),
	SHADER(pool_average),
	SHADER(pool_maximum),
	SHADER(binary),
	SHADER(unary),
	SHADER(softmax),
	SHADER(mean),
	SHADER(pad),
	SHADER(resize),
	SHADER(concat),
	SHADER(repack)
};

#undef SHADER


const char*
vulkan_shader_name(uint32 shader)
{
	return shader < VULKAN_SHADER_COUNT ? kShaders[shader].name : "?";
}


void
VulkanLimits::SetTo(const VulkanPhysicalDevice& device)
{
	const VkPhysicalDeviceLimits& limits = device.properties.limits;

	max_range = limits.maxStorageBufferRange;
	if (max_range > device.max_allocation_size)
		max_range = device.max_allocation_size;

	// The shaders count invocations in 32 bits, x and y together.
	for (int32 i = 0; i < 3; i++) {
		max_groups[i] = limits.maxComputeWorkGroupCount[i];
		if (max_groups[i] > 32768)
			max_groups[i] = 32768;
	}

	// vec4 at least, and whole cache lines
	alignment = 64;
	while (alignment < limits.minStorageBufferOffsetAlignment)
		alignment *= 2;
}


// #pragma mark - VulkanDevice


VulkanDevice::VulkanDevice(const VulkanLibrary& library, int32 index)
	:
	fLibrary(library),
	fPhysical(library.DeviceAt(index)),
	fDevice(VK_NULL_HANDLE),
	fQueue(VK_NULL_HANDLE),
	fQueueLock("vulkan neural queue"),
	fMemoryBudget(false),
	fSetLayout(VK_NULL_HANDLE),
	fPipelineLayout(VK_NULL_HANDLE),
	fPipelineCache(VK_NULL_HANDLE),
	fPipelineLock("vulkan neural pipelines"),
	fAllocated(0),
	fLost(0)
{
	memset(&fFunctions, 0, sizeof(fFunctions));
	memset(fPipelines, 0, sizeof(fPipelines));
	fLimits.SetTo(fPhysical);
}


VulkanDevice::~VulkanDevice()
{
	if (fDevice == VK_NULL_HANDLE)
		return;

	// A device that stopped answering may not come back from this either:
	// what it holds is left to the end of the team.
	if (IsLost())
		return;

	fFunctions.DeviceWaitIdle(fDevice);
	for (int32 i = 0; i < VULKAN_SHADER_COUNT; i++) {
		if (fPipelines[i] != VK_NULL_HANDLE)
			fFunctions.DestroyPipeline(fDevice, fPipelines[i], NULL);
	}
	if (fPipelineCache != VK_NULL_HANDLE)
		fFunctions.DestroyPipelineCache(fDevice, fPipelineCache, NULL);
	if (fPipelineLayout != VK_NULL_HANDLE)
		fFunctions.DestroyPipelineLayout(fDevice, fPipelineLayout, NULL);
	if (fSetLayout != VK_NULL_HANDLE) {
		fFunctions.DestroyDescriptorSetLayout(fDevice, fSetLayout, NULL);
	}
	fFunctions.DestroyDevice(fDevice, NULL);
}


status_t
VulkanDevice::Error(VkResult result)
{
	switch (result) {
		case VK_SUCCESS:
			return B_OK;
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			return B_NO_MEMORY;
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			// not B_NO_MEMORY: the CPU can still run the network
			return B_DEVICE_FULL;
		case VK_ERROR_DEVICE_LOST:
			SetLost();
			return B_NEURAL_DEVICE_LOST;
		case VK_TIMEOUT:
			return B_TIMED_OUT;
		default:
			return B_ERROR;
	}
}


status_t
VulkanDevice::Init()
{
	const VulkanInstanceFunctions& instance = fLibrary.Functions();

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queue;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = fPhysical.queue_family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;

	const char* extensions[1];
	uint32 extensionCount = 0;
	if (fPhysical.has_memory_budget
		&& instance.GetPhysicalDeviceMemoryProperties2 != NULL) {
		extensions[extensionCount++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
		fMemoryBudget = true;
	}

	VkDeviceCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;
	info.enabledExtensionCount = extensionCount;
	info.ppEnabledExtensionNames = extensions;

	VkResult result = instance.CreateDevice(fPhysical.handle, &info, NULL,
		&fDevice);
	if (result != VK_SUCCESS) {
		fDevice = VK_NULL_HANDLE;
		return Error(result);
	}

#define LOAD(name) \
	fFunctions.name = (PFN_vk##name)instance.GetDeviceProcAddr(fDevice, \
		"vk" #name); \
	if (fFunctions.name == NULL) \
		return B_MISSING_SYMBOL;
	VULKAN_DEVICE_FUNCTIONS(LOAD)
#undef LOAD

	fFunctions.GetDeviceQueue(fDevice, fPhysical.queue_family, 0, &fQueue);

	// one layout for all the shaders: four buffers, a block of constants
	VkDescriptorSetLayoutBinding bindings[VULKAN_BINDING_COUNT];
	memset(bindings, 0, sizeof(bindings));
	for (uint32 i = 0; i < VULKAN_BINDING_COUNT; i++) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo setInfo;
	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = VULKAN_BINDING_COUNT;
	setInfo.pBindings = bindings;
	result = fFunctions.CreateDescriptorSetLayout(fDevice, &setInfo, NULL,
		&fSetLayout);
	if (result != VK_SUCCESS)
		return Error(result);

	VkPushConstantRange range;
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.offset = 0;
	range.size = VULKAN_PUSH_CONSTANT_SIZE;

	VkPipelineLayoutCreateInfo layoutInfo;
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &fSetLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &range;
	result = fFunctions.CreatePipelineLayout(fDevice, &layoutInfo, NULL,
		&fPipelineLayout);
	if (result != VK_SUCCESS)
		return Error(result);

	VkPipelineCacheCreateInfo cacheInfo;
	memset(&cacheInfo, 0, sizeof(cacheInfo));
	cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (fFunctions.CreatePipelineCache(fDevice, &cacheInfo, NULL,
			&fPipelineCache) != VK_SUCCESS) {
		// it only saves time
		fPipelineCache = VK_NULL_HANDLE;
	}

	return B_OK;
}


status_t
VulkanDevice::GetPipeline(uint32 shader, VkPipeline* _pipeline)
{
	if (shader >= VULKAN_SHADER_COUNT)
		return B_BAD_VALUE;

	BAutolock locker(fPipelineLock);
	if (fPipelines[shader] != VK_NULL_HANDLE) {
		*_pipeline = fPipelines[shader];
		return B_OK;
	}

	VkShaderModuleCreateInfo moduleInfo;
	memset(&moduleInfo, 0, sizeof(moduleInfo));
	moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	moduleInfo.codeSize = kShaders[shader].size;
	moduleInfo.pCode = kShaders[shader].code;

	VkShaderModule module;
	VkResult result = fFunctions.CreateShaderModule(fDevice, &moduleInfo,
		NULL, &module);
	if (result != VK_SUCCESS)
		return Error(result);

	VkComputePipelineCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = fPipelineLayout;

	result = fFunctions.CreateComputePipelines(fDevice, fPipelineCache, 1,
		&info, NULL, &fPipelines[shader]);
	fFunctions.DestroyShaderModule(fDevice, module, NULL);
	if (result != VK_SUCCESS) {
		fPipelines[shader] = VK_NULL_HANDLE;
		return Error(result);
	}

	*_pipeline = fPipelines[shader];
	return B_OK;
}


/*!	The memory type for a buffer: the device's own memory, preferably of
	a kind the CPU does not see (on a card that is the fast one), or
	memory the CPU reads and writes without flushing, preferably cached:
	results are read back from it. */
int32
VulkanDevice::_FindMemoryType(uint32 allowed, bool hostVisible) const
{
	const VkPhysicalDeviceMemoryProperties& memory = fPhysical.memory;
	int32 best = -1;
	int32 bestScore = -1;

	for (uint32 i = 0; i < memory.memoryTypeCount; i++) {
		if ((allowed & (1u << i)) == 0)
			continue;

		VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
		int32 score;
		if (hostVisible) {
			const VkMemoryPropertyFlags kNeeded
				= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
					| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			if ((flags & kNeeded) != kNeeded)
				continue;
			score = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? 1 : 0;
		} else {
			if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
				continue;
			score = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ? 1 : 0;
		}

		if (score > bestScore) {
			best = i;
			bestScore = score;
		}
	}
	return best;
}


status_t
VulkanDevice::CreateBuffer(uint64 size, VkBufferUsageFlags usage,
	bool hostVisible, VulkanBuffer& buffer)
{
	memset(&buffer, 0, sizeof(buffer));
	if (size == 0)
		return B_BAD_VALUE;
	if (size > fPhysical.max_allocation_size)
		return B_DEVICE_FULL;

	VkBufferCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkResult result = fFunctions.CreateBuffer(fDevice, &info, NULL,
		&buffer.buffer);
	if (result != VK_SUCCESS) {
		buffer.buffer = VK_NULL_HANDLE;
		return Error(result);
	}

	VkMemoryRequirements requirements;
	fFunctions.GetBufferMemoryRequirements(fDevice, buffer.buffer,
		&requirements);

	int32 type = _FindMemoryType(requirements.memoryTypeBits, hostVisible);
	if (type < 0) {
		DeleteBuffer(buffer);
		return B_NOT_SUPPORTED;
	}

	VkMemoryAllocateInfo allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;

	result = fFunctions.AllocateMemory(fDevice, &allocation, NULL,
		&buffer.memory);
	if (result != VK_SUCCESS) {
		buffer.memory = VK_NULL_HANDLE;
		DeleteBuffer(buffer);
		return Error(result);
	}
	buffer.size = requirements.size;
	if (!hostVisible)
		atomic_add64(&fAllocated, buffer.size);

	result = fFunctions.BindBufferMemory(fDevice, buffer.buffer,
		buffer.memory, 0);
	if (result == VK_SUCCESS && hostVisible) {
		result = fFunctions.MapMemory(fDevice, buffer.memory, 0,
			VK_WHOLE_SIZE, 0, &buffer.address);
	}
	if (result != VK_SUCCESS) {
		buffer.address = NULL;
		DeleteBuffer(buffer);
		return Error(result);
	}

	return B_OK;
}


void
VulkanDevice::DeleteBuffer(VulkanBuffer& buffer)
{
	if (buffer.buffer != VK_NULL_HANDLE)
		fFunctions.DestroyBuffer(fDevice, buffer.buffer, NULL);
	if (buffer.memory != VK_NULL_HANDLE) {
		// unmaps it as well
		fFunctions.FreeMemory(fDevice, buffer.memory, NULL);
		if (buffer.address == NULL)
			atomic_add64(&fAllocated, -(int64)buffer.size);
	}
	memset(&buffer, 0, sizeof(buffer));
}


uint64
VulkanDevice::AvailableMemory()
{
	// what the driver says is left for this team, where it says
	if (fMemoryBudget) {
		VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
		memset(&budget, 0, sizeof(budget));
		budget.sType
			= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

		VkPhysicalDeviceMemoryProperties2 properties;
		memset(&properties, 0, sizeof(properties));
		properties.sType
			= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		properties.pNext = &budget;
		fLibrary.Functions().GetPhysicalDeviceMemoryProperties2(
			fPhysical.handle, &properties);

		uint64 total = budget.heapBudget[fPhysical.local_heap];
		uint64 used = budget.heapUsage[fPhysical.local_heap];
		if (total > 0)
			return total > used ? total - used : 0;
	}

	// Otherwise three quarters of the heap are taken to be ours: the rest
	// is for whatever else the machine draws with.
	uint64 total = fPhysical.local_heap_size / 4 * 3;
	uint64 used = (uint64)atomic_get64(&fAllocated);
	return total > used ? total - used : 0;
}


status_t
VulkanDevice::Submit(VkCommandBuffer commands, VkFence fence)
{
	if (IsLost())
		return B_NEURAL_DEVICE_LOST;

	VkSubmitInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	info.commandBufferCount = 1;
	info.pCommandBuffers = &commands;

	BAutolock locker(fQueueLock);
	return Error(fFunctions.QueueSubmit(fQueue, 1, &info, fence));
}


status_t
VulkanDevice::Wait(VkFence fence)
{
	VkResult result = fFunctions.WaitForFences(fDevice, 1, &fence, VK_TRUE,
		kWaitTimeout);
	if (result == VK_SUCCESS)
		return B_OK;

	// Work that does not end is still the device's: nothing of it may be
	// touched again.
	SetLost();
	return B_NEURAL_DEVICE_LOST;
}
