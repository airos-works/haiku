/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef VULKAN_PROGRAM_H
#define VULKAN_PROGRAM_H


#include <Locker.h>

#include "VulkanPlan.h"


/*!	A plan made real on a device: the memory with the weights in it, and
	the commands, recorded once, that run all the steps. A run copies the
	inputs to memory the device shares with the CPU, submits the commands
	(which begin by copying from there and end by copying the results
	back), waits and copies the outputs out.
*/
class VulkanProgram : public BNeuralProgram {
public:
								VulkanProgram(VulkanDevice* device);
	virtual						~VulkanProgram();

			status_t			Init(const VulkanPlan& plan);

	virtual	status_t			Run(void* const* tensors);
	virtual	bigtime_t			LastDeviceTime() const;

private:
	//! A tensor that goes in or out, and where in the shared memory.
	struct Transfer {
		int32				tensor;
		uint32				positions;
		uint32				channels;
		uint32				groups;
		uint64				offset;
	};

	//! What a command buffer of a profiled program does.
	struct Phase {
		uint32				shader;
		int32				op;
		neural_operator_code code;
		bigtime_t			time;
	};

			status_t			_CreateBuffers(const VulkanPlan& plan);
			status_t			_UploadConstants(const VulkanPlan& plan);
			status_t			_CreateDescriptors(const VulkanPlan& plan);
			status_t			_Record(const VulkanPlan& plan);
			void				_RecordCopies(VkCommandBuffer commands,
									const VulkanPlan& plan, bool in);
			status_t			_RecordStep(VkCommandBuffer commands,
									const VulkanPlan& plan, int32 index,
									VkPipeline& bound);
			void				_Barrier(VkCommandBuffer commands,
									VkPipelineStageFlags from,
									VkAccessFlags fromAccess,
									VkPipelineStageFlags to,
									VkAccessFlags toAccess);
			void				_Region(const VulkanPlan& plan,
									const PlanBinding& binding,
									VkDescriptorBufferInfo& info) const;
			void				_PrintStatistics() const;

			VulkanDevice*		fDevice;
			BLocker				fLock;

			VulkanBuffer		fTensors;
			VulkanBuffer		fConstants;
			VulkanBuffer		fShared;

			VkDescriptorPool	fDescriptorPool;
			VkDescriptorSet*	fSets;
			VkCommandPool		fCommandPool;
			VkCommandBuffer*	fCommands;
			int32				fCommandCount;
			VkFence				fFence;

			Transfer*			fInputs;
			int32				fInputCount;
			Transfer*			fOutputs;
			int32				fOutputCount;

			// NEURAL_VULKAN_STATS and NEURAL_VULKAN_PROFILE
			bool				fStatistics;
			Phase*				fPhases;
			int64				fRuns;
			bigtime_t			fCopyInTime;
			bigtime_t			fSubmitTime;
			bigtime_t			fWaitTime;
			bigtime_t			fCopyOutTime;
			bigtime_t			fLastDeviceTime;
};


#endif	// VULKAN_PROGRAM_H
