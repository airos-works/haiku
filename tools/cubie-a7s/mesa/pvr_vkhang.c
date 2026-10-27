/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// A deliberately hung GPU job, for the kernel's job timeout and GPU reset.
// A compute shader spins on a storage buffer the host leaves at 0, so it
// never ends on its own. The test submits it with a fence, waits with a
// timeout, and reports what ended it:
//   - VK_ERROR_DEVICE_LOST, or VK_SUCCESS once the kernel/firmware killed
//     the job: the recovery worked;
//   - VK_TIMEOUT: the kernel never recovered, a FAIL.
// Then it tries to render again on the same device, and on a fresh device,
// to see whether the reset recovered the context or the device is lost.
//
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vkhang [options]
//     --timeout MS     fence wait for the hang (default 180000)
//     --no-recovery    stop after reading the progress counter (steps 1-3)
//     --hang-ms N      instead of hanging, run a bounded job of about N ms
//                      (calibrated, approximate) and check it is NOT killed
//
// Output is line-buffered and each step is timestamped: the process may be
// lost with the device. There is no Vulkan loader on arm64 Haiku, so the
// driver is linked directly and everything starts from
// vk_icdGetInstanceProcAddr.


#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

#define HANG_BUFFER_SIZE	4096
#define FILL_BUFFER_SIZE	(1024 * 1024)
#define FILL_WORD_COUNT		(FILL_BUFFER_SIZE / 4)
#define FILL_LOCAL_SIZE		64
#define FILL_PATTERN		0xdeadbeefu

// data[0] stop sentinel (host leaves 0), data[1] progress, data[2] cap
#define HANG_PROGRESS		1
#define HANG_CAP		2


// The hang shader. SPIR-V 1.0, assembled with spirv-as (--target-env
// spv1.0) and validated with spirv-val (--target-env vulkan1.0) from
// toolchains/mesa-native-deps.
//
//   #version 450
//   layout(local_size_x = 1) in;
//   layout(std430, set = 0, binding = 0) buffer Buf { uint data[]; };
//   void main() {
//      uint i = 0u;
//      while (true) {
//         if (data[0] == 0x600DF00Du) break;  // host leaves data[0] = 0
//         i = i + 1u;
//         data[1] = i;                         // progress
//         if (data[2] != 0u && i >= data[2]) break;  // data[2]=cap (0=forever)
//      }
//   }
static const uint32_t kHangShader[] = {
	0x07230203, 0x00010000, 0x00070000, 0x00000023, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
	0x00000001, 0x6e69616d, 0x00000000, 0x00060010, 0x00000001, 0x00000011,
	0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000002, 0x00000006,
	0x00000004, 0x00050048, 0x00000003, 0x00000000, 0x00000023, 0x00000000,
	0x00030047, 0x00000003, 0x00000003, 0x00040047, 0x00000004, 0x00000022,
	0x00000000, 0x00040047, 0x00000004, 0x00000021, 0x00000000, 0x00020013,
	0x00000005, 0x00030021, 0x00000006, 0x00000005, 0x00040015, 0x00000007,
	0x00000020, 0x00000000, 0x00020014, 0x00000008, 0x00040020, 0x00000009,
	0x00000007, 0x00000007, 0x0003001d, 0x00000002, 0x00000007, 0x0003001e,
	0x00000003, 0x00000002, 0x00040020, 0x0000000a, 0x00000002, 0x00000003,
	0x0004003b, 0x0000000a, 0x00000004, 0x00000002, 0x0004002b, 0x00000007,
	0x0000000b, 0x00000000, 0x0004002b, 0x00000007, 0x0000000c, 0x00000001,
	0x0004002b, 0x00000007, 0x0000000d, 0x00000002, 0x0004002b, 0x00000007,
	0x0000000e, 0x600df00d, 0x00040020, 0x0000000f, 0x00000002, 0x00000007,
	0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006, 0x000200f8,
	0x00000010, 0x0004003b, 0x00000009, 0x00000011, 0x00000007, 0x0003003e,
	0x00000011, 0x0000000b, 0x000200f9, 0x00000012, 0x000200f8, 0x00000012,
	0x000400f6, 0x00000013, 0x00000014, 0x00000000, 0x000200f9, 0x00000015,
	0x000200f8, 0x00000015, 0x00060041, 0x0000000f, 0x00000016, 0x00000004,
	0x0000000b, 0x0000000b, 0x0004003d, 0x00000007, 0x00000017, 0x00000016,
	0x000500aa, 0x00000008, 0x00000018, 0x00000017, 0x0000000e, 0x000300f7,
	0x00000019, 0x00000000, 0x000400fa, 0x00000018, 0x00000013, 0x00000019,
	0x000200f8, 0x00000019, 0x0004003d, 0x00000007, 0x0000001a, 0x00000011,
	0x00050080, 0x00000007, 0x0000001b, 0x0000001a, 0x0000000c, 0x0003003e,
	0x00000011, 0x0000001b, 0x00060041, 0x0000000f, 0x0000001c, 0x00000004,
	0x0000000b, 0x0000000c, 0x0003003e, 0x0000001c, 0x0000001b, 0x00060041,
	0x0000000f, 0x0000001d, 0x00000004, 0x0000000b, 0x0000000d, 0x0004003d,
	0x00000007, 0x0000001e, 0x0000001d, 0x000500ab, 0x00000008, 0x0000001f,
	0x0000001e, 0x0000000b, 0x000500ae, 0x00000008, 0x00000020, 0x0000001b,
	0x0000001e, 0x000500a7, 0x00000008, 0x00000021, 0x0000001f, 0x00000020,
	0x000300f7, 0x00000022, 0x00000000, 0x000400fa, 0x00000021, 0x00000013,
	0x00000022, 0x000200f8, 0x00000022, 0x000200f9, 0x00000014, 0x000200f8,
	0x00000014, 0x000200f9, 0x00000012, 0x000200f8, 0x00000013, 0x000100fd,
	0x00010038,
};

// The recovery shader: pvr_vkfill's, data[i] = i * 3 + 1 over the buffer.
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


static VkInstance sInstance;
static PFN_vkGetDeviceProcAddr sGetDeviceProcAddr;
static double sStart;

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


static void
step(const char* fmt, ...)
{
	va_list args;
	printf("[t=%8.3fs] ", (now_ms() - sStart) / 1000.0);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	putchar('\n');
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


// device-level entry points, re-resolved for each new device
struct device_fns {
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkCreateDescriptorPool CreateDescriptorPool;
	PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkCmdBindPipeline CmdBindPipeline;
	PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
	PFN_vkCmdDispatch CmdDispatch;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCreateFence CreateFence;
	PFN_vkResetFences ResetFences;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkDestroyFence DestroyFence;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkDestroyPipeline DestroyPipeline;
	PFN_vkDestroyShaderModule DestroyShaderModule;
	PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
	PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
	PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;
	PFN_vkDestroyDevice DestroyDevice;
};


static int
load_device_fns(VkDevice device, struct device_fns* fns)
{
	memset(fns, 0, sizeof(*fns));
#define LOAD(name) \
	fns->name = (PFN_vk##name)sGetDeviceProcAddr(device, "vk" #name); \
	if (fns->name == NULL) { printf("FAIL: no vk" #name "\n"); return 0; }
	LOAD(GetDeviceQueue);
	LOAD(CreateBuffer);
	LOAD(GetBufferMemoryRequirements);
	LOAD(AllocateMemory);
	LOAD(BindBufferMemory);
	LOAD(MapMemory);
	LOAD(CreateDescriptorSetLayout);
	LOAD(CreatePipelineLayout);
	LOAD(CreateDescriptorPool);
	LOAD(AllocateDescriptorSets);
	LOAD(UpdateDescriptorSets);
	LOAD(CreateShaderModule);
	LOAD(CreateComputePipelines);
	LOAD(CreateCommandPool);
	LOAD(AllocateCommandBuffers);
	LOAD(BeginCommandBuffer);
	LOAD(CmdBindPipeline);
	LOAD(CmdBindDescriptorSets);
	LOAD(CmdDispatch);
	LOAD(CmdPipelineBarrier);
	LOAD(EndCommandBuffer);
	LOAD(CreateFence);
	LOAD(ResetFences);
	LOAD(QueueSubmit);
	LOAD(WaitForFences);
	LOAD(DestroyFence);
	LOAD(DestroyCommandPool);
	LOAD(DestroyPipeline);
	LOAD(DestroyShaderModule);
	LOAD(DestroyDescriptorPool);
	LOAD(DestroyPipelineLayout);
	LOAD(DestroyDescriptorSetLayout);
	LOAD(FreeMemory);
	LOAD(DestroyBuffer);
	LOAD(DeviceWaitIdle);
	LOAD(DestroyDevice);
#undef LOAD
	return 1;
}


// a compute job bound to one device: a storage buffer, the pipeline, and a
// command buffer that dispatches "groups" workgroups
struct compute {
	VkDevice device;
	struct device_fns* fns;
	VkDeviceSize size;
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t* words;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout pipelineLayout;
	VkDescriptorPool descriptorPool;
	VkDescriptorSet descriptorSet;
	VkShaderModule module;
	VkPipeline pipeline;
	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;
	VkFence fence;
};


static uint32_t
find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits,
	VkMemoryPropertyFlags wanted)
{
	INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		if ((typeBits & (1u << i)) != 0
			&& (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	}
	return UINT32_MAX;
}


static VkResult
compute_setup(struct compute* c, struct device_fns* fns, VkDevice device,
	VkPhysicalDevice physicalDevice, uint32_t family,
	const uint32_t* spirv, size_t spirvSize, VkDeviceSize size,
	uint32_t groups)
{
	memset(c, 0, sizeof(*c));
	c->device = device;
	c->fns = fns;
	c->size = size;

	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkResult result = fns->CreateBuffer(device, &bufferInfo, NULL, &c->buffer);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	fns->GetBufferMemoryRequirements(device, c->buffer, &requirements);
	uint32_t type = find_memory_type(physicalDevice,
		requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX) {
		printf("FAIL: no HOST_VISIBLE | HOST_COHERENT memory type\n");
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	result = fns->AllocateMemory(device, &allocateInfo, NULL, &c->memory);
	if (result != VK_SUCCESS)
		return result;
	result = fns->BindBufferMemory(device, c->buffer, c->memory, 0);
	if (result != VK_SUCCESS)
		return result;
	result = fns->MapMemory(device, c->memory, 0, VK_WHOLE_SIZE, 0,
		(void**)&c->words);
	if (result != VK_SUCCESS)
		return result;

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
	result = fns->CreateDescriptorSetLayout(device, &setLayoutInfo, NULL,
		&c->setLayout);
	if (result != VK_SUCCESS)
		return result;
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &c->setLayout,
	};
	result = fns->CreatePipelineLayout(device, &pipelineLayoutInfo, NULL,
		&c->pipelineLayout);
	if (result != VK_SUCCESS)
		return result;

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
	result = fns->CreateDescriptorPool(device, &poolInfo, NULL,
		&c->descriptorPool);
	if (result != VK_SUCCESS)
		return result;
	VkDescriptorSetAllocateInfo setInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = c->descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &c->setLayout,
	};
	result = fns->AllocateDescriptorSets(device, &setInfo, &c->descriptorSet);
	if (result != VK_SUCCESS)
		return result;
	VkDescriptorBufferInfo descriptorBuffer = {
		.buffer = c->buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = c->descriptorSet,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &descriptorBuffer,
	};
	fns->UpdateDescriptorSets(device, 1, &write, 0, NULL);

	VkShaderModuleCreateInfo moduleInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spirvSize,
		.pCode = spirv,
	};
	result = fns->CreateShaderModule(device, &moduleInfo, NULL, &c->module);
	if (result != VK_SUCCESS)
		return result;
	VkComputePipelineCreateInfo pipelineInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = c->module,
			.pName = "main",
		},
		.layout = c->pipelineLayout,
	};
	result = fns->CreateComputePipelines(device, VK_NULL_HANDLE, 1,
		&pipelineInfo, NULL, &c->pipeline);
	if (result != VK_SUCCESS)
		return result;

	VkCommandPoolCreateInfo commandPoolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family,
	};
	result = fns->CreateCommandPool(device, &commandPoolInfo, NULL,
		&c->commandPool);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferAllocateInfo commandBufferInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = c->commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	result = fns->AllocateCommandBuffers(device, &commandBufferInfo,
		&c->commandBuffer);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	result = fns->BeginCommandBuffer(c->commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
		return result;
	fns->CmdBindPipeline(c->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
		c->pipeline);
	fns->CmdBindDescriptorSets(c->commandBuffer,
		VK_PIPELINE_BIND_POINT_COMPUTE, c->pipelineLayout, 0, 1,
		&c->descriptorSet, 0, NULL);
	fns->CmdDispatch(c->commandBuffer, groups, 1, 1);
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	fns->CmdPipelineBarrier(c->commandBuffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
		1, &barrier, 0, NULL, 0, NULL);
	result = fns->EndCommandBuffer(c->commandBuffer);
	if (result != VK_SUCCESS)
		return result;

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	return fns->CreateFence(device, &fenceInfo, NULL, &c->fence);
}


// submit the recorded command buffer and wait on its fence; the submit
// result and the wait result are both reported (either can be
// VK_ERROR_DEVICE_LOST)
static VkResult
compute_submit(struct compute* c, VkQueue queue, uint64_t timeoutNs,
	VkResult* submitResult, double* elapsedMs)
{
	struct device_fns* fns = c->fns;
	fns->ResetFences(c->device, 1, &c->fence);
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &c->commandBuffer,
	};
	double start = now_ms();
	VkResult submit = fns->QueueSubmit(queue, 1, &submitInfo, c->fence);
	if (submitResult != NULL)
		*submitResult = submit;
	if (submit != VK_SUCCESS) {
		if (elapsedMs != NULL)
			*elapsedMs = now_ms() - start;
		return submit;
	}
	VkResult wait = fns->WaitForFences(c->device, 1, &c->fence, VK_TRUE,
		timeoutNs);
	if (elapsedMs != NULL)
		*elapsedMs = now_ms() - start;
	return wait;
}


static void
compute_teardown(struct compute* c)
{
	struct device_fns* fns = c->fns;
	if (c->device == VK_NULL_HANDLE)
		return;
	if (c->fence) fns->DestroyFence(c->device, c->fence, NULL);
	if (c->commandPool)
		fns->DestroyCommandPool(c->device, c->commandPool, NULL);
	if (c->pipeline) fns->DestroyPipeline(c->device, c->pipeline, NULL);
	if (c->module) fns->DestroyShaderModule(c->device, c->module, NULL);
	if (c->descriptorPool)
		fns->DestroyDescriptorPool(c->device, c->descriptorPool, NULL);
	if (c->pipelineLayout)
		fns->DestroyPipelineLayout(c->device, c->pipelineLayout, NULL);
	if (c->setLayout)
		fns->DestroyDescriptorSetLayout(c->device, c->setLayout, NULL);
	if (c->memory) fns->FreeMemory(c->device, c->memory, NULL);
	if (c->buffer) fns->DestroyBuffer(c->device, c->buffer, NULL);
	memset(c, 0, sizeof(*c));
}


// instance-level handles used throughout
static VkPhysicalDevice sPhysicalDevice;
static uint32_t sComputeFamily;


static int
enumerate(void)
{
	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties);
	INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
	uint32_t count = 1;
	VkResult result = vkEnumeratePhysicalDevices(sInstance, &count,
		&sPhysicalDevice);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
		step("no physical device (%s); see pvr_vkprobe", result_name(result));
		return 0;
	}
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(sPhysicalDevice, &properties);
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(sPhysicalDevice, &familyCount,
		NULL);
	VkQueueFamilyProperties families[8];
	if (familyCount > 8)
		familyCount = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(sPhysicalDevice, &familyCount,
		families);
	sComputeFamily = UINT32_MAX;
	for (uint32_t i = 0; i < familyCount; i++) {
		if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			sComputeFamily = i;
			break;
		}
	}
	if (sComputeFamily == UINT32_MAX) {
		step("no compute queue family");
		return 0;
	}
	step("device: %s, compute queue family %u", properties.deviceName,
		sComputeFamily);
	return 1;
}


static VkResult
make_device(VkDevice* device, VkQueue* queue, struct device_fns* fns)
{
	INSTANCE_FN(vkCreateDevice);
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = sComputeFamily,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	VkResult result = vkCreateDevice(sPhysicalDevice, &deviceInfo, NULL,
		device);
	if (result != VK_SUCCESS)
		return result;
	if (!load_device_fns(*device, fns))
		return VK_ERROR_INITIALIZATION_FAILED;
	fns->GetDeviceQueue(*device, sComputeFamily, 0, queue);
	return VK_SUCCESS;
}


// Run the fill dispatch on a device and verify every word. Returns the
// submit/wait VkResult; sets *ok only when the result is VK_SUCCESS and the
// data is correct.
static VkResult
fill_and_verify(VkDevice device, VkQueue queue, struct device_fns* fns,
	int* ok, uint64_t timeoutNs)
{
	*ok = 0;
	struct compute fill;
	VkResult result = compute_setup(&fill, fns, device, sPhysicalDevice,
		sComputeFamily, kFillShader, sizeof(kFillShader), FILL_BUFFER_SIZE,
		FILL_WORD_COUNT / FILL_LOCAL_SIZE);
	if (result != VK_SUCCESS) {
		step("  fill setup: %s", result_name(result));
		compute_teardown(&fill);
		return result;
	}
	for (uint32_t i = 0; i < FILL_WORD_COUNT; i++)
		fill.words[i] = FILL_PATTERN;
	VkResult submit = VK_SUCCESS;
	double elapsed = 0.0;
	result = compute_submit(&fill, queue, timeoutNs, &submit, &elapsed);
	step("  fill submit %s, fence %s after %.1f ms", result_name(submit),
		result_name(result), elapsed);
	if (result == VK_SUCCESS && submit == VK_SUCCESS) {
		uint32_t wrong = 0;
		for (uint32_t i = 0; i < FILL_WORD_COUNT; i++) {
			if (fill.words[i] != i * 3 + 1)
				wrong++;
		}
		step("  fill check: %u of %u words wrong", wrong,
			(unsigned)FILL_WORD_COUNT);
		*ok = wrong == 0;
	}
	compute_teardown(&fill);
	return result;
}


// --hang-ms: a bounded loop of about N ms, to check that a long but legal
// job is NOT killed. The iteration count is calibrated against this GPU by
// doubling until a run is long enough to measure, so the N ms is
// approximate. Returns an exit code.
static int
run_hang_ms(VkDevice device, VkQueue queue, struct device_fns* fns,
	int64_t targetMs)
{
	step("step: calibrating a bounded loop (approximate)");
	struct compute job;
	VkResult result = compute_setup(&job, fns, device, sPhysicalDevice,
		sComputeFamily, kHangShader, sizeof(kHangShader), HANG_BUFFER_SIZE, 1);
	if (result != VK_SUCCESS) {
		step("setup: %s", result_name(result));
		return 1;
	}

	// double the cap until a run takes at least 30 ms or the cap is huge
	uint32_t cap = 1u << 20;
	double elapsed = 0.0;
	double perMs = 0.0;
	while (true) {
		job.words[0] = 0;
		job.words[HANG_PROGRESS] = 0;
		job.words[HANG_CAP] = cap;
		VkResult submit = VK_SUCCESS;
		result = compute_submit(&job, queue, 30000ull * 1000000ull, &submit,
			&elapsed);
		if (submit != VK_SUCCESS || result != VK_SUCCESS) {
			step("calibration submit %s, fence %s: cannot calibrate",
				result_name(submit), result_name(result));
			compute_teardown(&job);
			return 1;
		}
		step("  cap %u ran %.1f ms (progress %u)", cap, elapsed,
			job.words[HANG_PROGRESS]);
		if (elapsed >= 30.0 || cap >= (1u << 30)) {
			perMs = cap / (elapsed > 0.0 ? elapsed : 1.0);
			break;
		}
		cap <<= 2;
	}
	step("  about %.0f iterations/ms", perMs);

	double wantMs = (double)targetMs;
	uint64_t wantIters = (uint64_t)(perMs * wantMs);
	if (wantIters == 0)
		wantIters = 1;
	if (wantIters > 0xffffffffull)
		wantIters = 0xffffffffull;
	uint32_t targetCap = (uint32_t)wantIters;
	// a legal job must not be killed: wait well past the target
	uint64_t timeoutNs = ((uint64_t)targetMs * 4 + 5000) * 1000000ull;
	step("step: bounded job of about %lld ms (cap %u), must NOT be killed",
		(long long)targetMs, targetCap);
	job.words[0] = 0;
	job.words[HANG_PROGRESS] = 0;
	job.words[HANG_CAP] = targetCap;
	VkResult submit = VK_SUCCESS;
	result = compute_submit(&job, queue, timeoutNs, &submit, &elapsed);
	step("vkQueueSubmit %s", result_name(submit));
	step("vkWaitForFences %s after %.1f ms (target ~%lld ms), progress %u",
		result_name(result), elapsed, (long long)targetMs,
		job.words[HANG_PROGRESS]);
	int ok = submit == VK_SUCCESS && result == VK_SUCCESS;
	if (!ok)
		step("FAIL: a legal ~%lld ms job did not finish cleanly",
			(long long)targetMs);
	compute_teardown(&job);
	fns->DestroyDevice(device, NULL);
	step("%s", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	sStart = now_ms();

	uint64_t timeoutMs = 180000;
	int noRecovery = 0;
	int64_t hangMs = -1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			timeoutMs = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--no-recovery") == 0)
			noRecovery = 1;
		else if (strcmp(argv[i], "--hang-ms") == 0 && i + 1 < argc)
			hangMs = strtoll(argv[++i], NULL, 0);
		else {
			printf("usage: %s [--timeout MS] [--no-recovery] [--hang-ms N]\n",
				argv[0]);
			return 2;
		}
	}

	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	if (vkCreateInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vkhang",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));
	sGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(
		sInstance, "vkGetDeviceProcAddr");
	if (sGetDeviceProcAddr == NULL) {
		printf("FAIL: no vkGetDeviceProcAddr\n");
		return 1;
	}
	if (!enumerate())
		return 1;

	struct device_fns fns;
	VkDevice device;
	VkQueue queue;
	CHECK(make_device(&device, &queue, &fns));

	if (hangMs >= 0)
		return run_hang_ms(device, queue, &fns, hangMs);

	// 1-2: the hang
	step("step 1-2: a compute job that never ends, fence wait up to %llu ms",
		(unsigned long long)timeoutMs);
	struct compute hang;
	VkResult result = compute_setup(&hang, &fns, device, sPhysicalDevice,
		sComputeFamily, kHangShader, sizeof(kHangShader), HANG_BUFFER_SIZE, 1);
	if (result != VK_SUCCESS) {
		step("hang setup: %s", result_name(result));
		return 1;
	}
	hang.words[0] = 0;
	hang.words[HANG_PROGRESS] = 0;
	hang.words[HANG_CAP] = 0;			// forever

	VkResult submit = VK_SUCCESS;
	double elapsed = 0.0;
	result = compute_submit(&hang, queue, timeoutMs * 1000000ull, &submit,
		&elapsed);
	step("vkQueueSubmit %s", result_name(submit));
	step("vkWaitForFences %s after %.1f ms", result_name(result), elapsed);

	int hangEnded = result != VK_TIMEOUT;
	if (result == VK_TIMEOUT) {
		step("FAIL: the kernel did not end the hung job in %llu ms",
			(unsigned long long)timeoutMs);
	} else if (result == VK_SUCCESS) {
		step("the job was ended and the fence signalled (kernel/firmware "
			"reset)");
	} else {
		step("the job was ended with %s", result_name(result));
	}

	// 3: progress counter (coherent memory; readable even after a reset)
	step("step 3: progress counter data[1] = %u", hang.words[HANG_PROGRESS]);

	if (noRecovery) {
		compute_teardown(&hang);
		step("--no-recovery: stopping after step 3");
		step("%s", hangEnded ? "PASS" : "FAIL");
		fns.DestroyDevice(device, NULL);
		return hangEnded ? 0 : 1;
	}

	// 4: recovery on the same device
	step("step 4: fill dispatch again on the SAME device");
	int sameOk = 0;
	VkResult sameResult = fill_and_verify(device, queue, &fns, &sameOk,
		5000ull * 1000000ull);
	step("step 4 result: %s%s", result_name(sameResult),
		sameOk ? ", data correct" : "");

	compute_teardown(&hang);
	fns.DestroyDevice(device, NULL);

	// 5: recovery on a new device
	step("step 5: destroy the device, create a new one, fill dispatch");
	VkDevice device2;
	VkQueue queue2;
	struct device_fns fns2;
	int newOk = 0;
	VkResult newResult;
	int newConsistent;
	if (!enumerate()) {
		step("step 5: the device no longer enumerates (declared lost)");
		newResult = VK_ERROR_DEVICE_LOST;
		newConsistent = 1;
	} else {
		newResult = make_device(&device2, &queue2, &fns2);
		if (newResult != VK_SUCCESS) {
			step("step 5: vkCreateDevice %s (declared lost)",
				result_name(newResult));
			newConsistent = 1;
		} else {
			newResult = fill_and_verify(device2, queue2, &fns2, &newOk,
				5000ull * 1000000ull);
			step("step 5 result: %s%s", result_name(newResult),
				newOk ? ", data correct" : "");
			// A new device must either work or be cleanly lost.
			newConsistent = newOk || newResult == VK_ERROR_DEVICE_LOST;
			fns2.DestroyDevice(device2, NULL);
		}
	}

	int pass = hangEnded && newConsistent;
	step("summary: hang %s, same-device fill %s, new-device %s",
		hangEnded ? "ended" : "TIMED OUT",
		sameOk ? "recovered"
			: sameResult == VK_ERROR_DEVICE_LOST ? "device lost" : "failed",
		newOk ? "recovered"
			: newResult == VK_ERROR_DEVICE_LOST ? "device lost" : "failed");
	step("%s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
