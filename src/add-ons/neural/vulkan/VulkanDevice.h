/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H


#include <Locker.h>
#include <SupportDefs.h>

#include "VulkanLibrary.h"


//! The compute shaders, see shaders/.
enum vulkan_shader {
	VULKAN_SHADER_CONV = 0,
	VULKAN_SHADER_CONV_POINTWISE,
	VULKAN_SHADER_CONV8,
	VULKAN_SHADER_CONV8_POINTWISE,
	VULKAN_SHADER_DEPTHWISE,
	VULKAN_SHADER_DEPTHWISE_MULTIPLIER,
	VULKAN_SHADER_POOL_AVERAGE,
	VULKAN_SHADER_POOL_MAXIMUM,
	VULKAN_SHADER_BINARY,
	VULKAN_SHADER_UNARY,
	VULKAN_SHADER_SOFTMAX,
	VULKAN_SHADER_MEAN,
	VULKAN_SHADER_PAD,
	VULKAN_SHADER_RESIZE,
	VULKAN_SHADER_CONCAT,
	VULKAN_SHADER_REPACK,

	VULKAN_SHADER_COUNT
};

const char* vulkan_shader_name(uint32 shader);


//! Invocations of a workgroup; the shaders' LOCAL_SIZE.
#define VULKAN_LOCAL_SIZE			64
//! Storage buffers a shader is given; the bindings of the one layout.
#define VULKAN_BINDING_COUNT		4
//! Bytes of push constants of the one layout: what every driver has.
#define VULKAN_PUSH_CONSTANT_SIZE	128


/*!	What a device can take, known without opening it: programs are planned
	against these. */
struct VulkanLimits {
	//! bytes one tensor or one set of weights may have
	uint64				max_range;
	//! workgroups a dispatch may have along x and y
	uint32				max_groups[2];
	//! what offsets of tensors in a buffer are multiples of
	uint32				alignment;

	void				SetTo(const VulkanPhysicalDevice& device);
};


struct VulkanBuffer {
	VkBuffer			buffer;
	VkDeviceMemory		memory;
	uint64				size;
	//! for memory the CPU reaches
	void*				address;
};


/*!	An open device: one for each graphics processor used, shared by all
	the programs of the team. It owns what they have in common: the queue
	(one thread submits at a time), the compiled shaders, and the count
	of the memory in use.
*/
class VulkanDevice {
public:
								VulkanDevice(const VulkanLibrary& library,
									int32 index);
								~VulkanDevice();

			status_t			Init();

			VkDevice			Handle() const { return fDevice; }
			const VulkanDeviceFunctions& Functions() const
									{ return fFunctions; }
			const VulkanPhysicalDevice& Physical() const
									{ return fPhysical; }
			const VulkanLimits&	Limits() const { return fLimits; }
			uint32				QueueFamily() const
									{ return fPhysical.queue_family; }

			VkDescriptorSetLayout SetLayout() const { return fSetLayout; }
			VkPipelineLayout	PipelineLayout() const
									{ return fPipelineLayout; }
	//! Compiles the shader the first time it is asked for.
			status_t			GetPipeline(uint32 shader,
									VkPipeline* _pipeline);

	/*!	A buffer with memory of its own: the device's, or (\a hostVisible)
		memory that stays mapped at \c address. */
			status_t			CreateBuffer(uint64 size,
									VkBufferUsageFlags usage,
									bool hostVisible, VulkanBuffer& buffer);
			void				DeleteBuffer(VulkanBuffer& buffer);

	//! Bytes of the device's memory that programs may still take.
			uint64				AvailableMemory();
	//! Bytes of it the programs of this team have.
			uint64				UsedMemory()
									{ return atomic_get64(&fAllocated); }

			status_t			Submit(VkCommandBuffer commands,
									VkFence fence);
	/*!	Waits for submitted work. B_NEURAL_DEVICE_LOST when it does not
		end, and the device is not used any more then. */
			status_t			Wait(VkFence fence);

			bool				IsLost() const
									{ return atomic_get(
										(int32*)&fLost) != 0; }
			void				SetLost() { atomic_set(&fLost, 1); }

	//! VK_ERROR_... as a status_t
			status_t			Error(VkResult result);

private:
			int32				_FindMemoryType(uint32 allowed,
									bool hostVisible) const;

			const VulkanLibrary& fLibrary;
			VulkanPhysicalDevice fPhysical;
			VulkanLimits		fLimits;

			VkDevice			fDevice;
			VulkanDeviceFunctions fFunctions;
			VkQueue				fQueue;
			BLocker				fQueueLock;
			bool				fMemoryBudget;

			VkDescriptorSetLayout fSetLayout;
			VkPipelineLayout	fPipelineLayout;
			VkPipelineCache		fPipelineCache;
			BLocker				fPipelineLock;
			VkPipeline			fPipelines[VULKAN_SHADER_COUNT];

			int64				fAllocated;
				// bytes of device memory, atomic
			int32				fLost;
};


#endif	// VULKAN_DEVICE_H
