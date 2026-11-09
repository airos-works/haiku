/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef VULKAN_API_H
#define VULKAN_API_H


/*!	The Vulkan functions the back end uses. Nothing is linked against a
	Vulkan library: the loader is opened at run time (see VulkanLibrary)
	and every function is asked from it by name, so the add-on loads on
	machines without one.
*/


#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"


//! Functions of an instance, all of Vulkan 1.0.
#define VULKAN_INSTANCE_FUNCTIONS(_) \
	_(DestroyInstance) \
	_(EnumeratePhysicalDevices) \
	_(GetPhysicalDeviceProperties) \
	_(GetPhysicalDeviceQueueFamilyProperties) \
	_(GetPhysicalDeviceMemoryProperties) \
	_(EnumerateDeviceExtensionProperties) \
	_(CreateDevice) \
	_(GetDeviceProcAddr)

//! Functions of an instance that came with Vulkan 1.1; may be NULL.
#define VULKAN_INSTANCE_FUNCTIONS_1_1(_) \
	_(GetPhysicalDeviceProperties2) \
	_(GetPhysicalDeviceMemoryProperties2)

#define VULKAN_DEVICE_FUNCTIONS(_) \
	_(DestroyDevice) \
	_(GetDeviceQueue) \
	_(DeviceWaitIdle) \
	_(CreateBuffer) \
	_(DestroyBuffer) \
	_(GetBufferMemoryRequirements) \
	_(AllocateMemory) \
	_(FreeMemory) \
	_(BindBufferMemory) \
	_(MapMemory) \
	_(UnmapMemory) \
	_(CreateShaderModule) \
	_(DestroyShaderModule) \
	_(CreatePipelineCache) \
	_(DestroyPipelineCache) \
	_(CreateDescriptorSetLayout) \
	_(DestroyDescriptorSetLayout) \
	_(CreatePipelineLayout) \
	_(DestroyPipelineLayout) \
	_(CreateComputePipelines) \
	_(DestroyPipeline) \
	_(CreateDescriptorPool) \
	_(DestroyDescriptorPool) \
	_(AllocateDescriptorSets) \
	_(UpdateDescriptorSets) \
	_(CreateCommandPool) \
	_(DestroyCommandPool) \
	_(AllocateCommandBuffers) \
	_(BeginCommandBuffer) \
	_(EndCommandBuffer) \
	_(CmdBindPipeline) \
	_(CmdBindDescriptorSets) \
	_(CmdPushConstants) \
	_(CmdDispatch) \
	_(CmdCopyBuffer) \
	_(CmdPipelineBarrier) \
	_(CreateFence) \
	_(DestroyFence) \
	_(ResetFences) \
	_(WaitForFences) \
	_(QueueSubmit)


#define VULKAN_DECLARE_FUNCTION(name) PFN_vk##name name;

struct VulkanInstanceFunctions {
	VULKAN_INSTANCE_FUNCTIONS(VULKAN_DECLARE_FUNCTION)
	VULKAN_INSTANCE_FUNCTIONS_1_1(VULKAN_DECLARE_FUNCTION)
};

struct VulkanDeviceFunctions {
	VULKAN_DEVICE_FUNCTIONS(VULKAN_DECLARE_FUNCTION)
};

#undef VULKAN_DECLARE_FUNCTION


#endif	// VULKAN_API_H
