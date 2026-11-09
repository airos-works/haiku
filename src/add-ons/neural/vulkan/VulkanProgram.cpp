/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VulkanProgram.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>


static bool
has_setting(const char* name)
{
	const char* value = getenv(name);
	return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}


/*!	From the kit's layout, channels one after the other, to the device's,
	channels in fours with zeros to fill up; the same thing when the
	channels are a multiple of four. */
static void
copy_to_device(float* target, const float* source, uint32 positions,
	uint32 channels, uint32 groups)
{
	if (channels % 4 == 0) {
		memcpy(target, source, (size_t)positions * channels * sizeof(float));
		return;
	}

	// pictures of red, green and blue: what most networks are given
	if (channels == 3) {
		for (uint32 i = 0; i < positions; i++) {
			target[0] = source[0];
			target[1] = source[1];
			target[2] = source[2];
			target[3] = 0;
			target += 4;
			source += 3;
		}
		return;
	}

	uint32 padded = groups * 4;
	for (uint32 i = 0; i < positions; i++) {
		uint32 j = 0;
		for (; j < channels; j++)
			target[j] = source[j];
		for (; j < padded; j++)
			target[j] = 0;
		target += padded;
		source += channels;
	}
}


static void
copy_from_device(float* target, const float* source, uint32 positions,
	uint32 channels, uint32 groups)
{
	if (channels % 4 == 0 || positions == 1) {
		memcpy(target, source, (size_t)positions * channels * sizeof(float));
		return;
	}

	uint32 padded = groups * 4;
	for (uint32 i = 0; i < positions; i++) {
		for (uint32 j = 0; j < channels; j++)
			target[j] = source[j];
		target += channels;
		source += padded;
	}
}


VulkanProgram::VulkanProgram(VulkanDevice* device)
	:
	fDevice(device),
	fLock("vulkan neural program"),
	fDescriptorPool(VK_NULL_HANDLE),
	fSets(NULL),
	fCommandPool(VK_NULL_HANDLE),
	fCommands(NULL),
	fCommandCount(0),
	fFence(VK_NULL_HANDLE),
	fChoices(NULL),
	fInputs(NULL),
	fInputCount(0),
	fOutputs(NULL),
	fOutputCount(0),
	fStatistics(false),
	fPhases(NULL),
	fRuns(0),
	fCopyInTime(0),
	fSubmitTime(0),
	fWaitTime(0),
	fCopyOutTime(0),
	fLastDeviceTime(0)
{
	memset(&fTensors, 0, sizeof(fTensors));
	memset(&fConstants, 0, sizeof(fConstants));
	memset(&fShared, 0, sizeof(fShared));
}


VulkanProgram::~VulkanProgram()
{
	if (fStatistics && fRuns > 0)
		_PrintStatistics();

	// Work that never ended may still be using all of this: it is left
	// alone, with the device.
	if (!fDevice->IsLost()) {
		const VulkanDeviceFunctions& vk = fDevice->Functions();
		VkDevice device = fDevice->Handle();

		if (fFence != VK_NULL_HANDLE)
			vk.DestroyFence(device, fFence, NULL);
		if (fCommandPool != VK_NULL_HANDLE)
			vk.DestroyCommandPool(device, fCommandPool, NULL);
		if (fDescriptorPool != VK_NULL_HANDLE)
			vk.DestroyDescriptorPool(device, fDescriptorPool, NULL);
		fDevice->DeleteBuffer(fShared);
		fDevice->DeleteBuffer(fConstants);
		fDevice->DeleteBuffer(fTensors);
	}

	free(fSets);
	free(fCommands);
	free(fChoices);
	free(fInputs);
	free(fOutputs);
	free(fPhases);
}


status_t
VulkanProgram::Init(const VulkanPlan& plan)
{
	if (fLock.InitCheck() != B_OK)
		return fLock.InitCheck();
	if (fDevice->IsLost())
		return B_NEURAL_DEVICE_LOST;

	fStatistics = has_setting("NEURAL_VULKAN_STATS")
		|| has_setting("NEURAL_VULKAN_PROFILE");

	// what Run() needs to know of the plan
	fInputs = (Transfer*)calloc(plan.CountInputs() + 1, sizeof(Transfer));
	fOutputs = (Transfer*)calloc(plan.CountOutputs() + 1, sizeof(Transfer));
	if (fInputs == NULL || fOutputs == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < plan.CountInputs() + plan.CountOutputs(); i++) {
		bool isInput = i < plan.CountInputs();
		const PlanTransfer& from = isInput ? plan.InputAt(i)
			: plan.OutputAt(i - plan.CountInputs());
		const PlanValue& value = plan.ValueAt(from.value);

		Transfer& transfer = isInput ? fInputs[fInputCount++]
			: fOutputs[fOutputCount++];
		transfer.tensor = from.tensor;
		transfer.positions = value.positions;
		transfer.channels = value.channels;
		transfer.groups = value.groups;
		transfer.offset = from.offset;
	}

	fChoices = (uint8*)calloc(plan.CountSteps(), 1);
	if (fChoices == NULL)
		return B_NO_MEMORY;

	status_t status = _CreateBuffers(plan);
	if (status == B_OK)
		status = _CreateDescriptors(plan);
	if (status == B_OK)
		status = _CreateCommands();
	if (status == B_OK)
		status = _UploadConstants(plan);
	if (status == B_OK)
		status = _Tune(plan);
	if (status == B_OK)
		status = _Record(plan);
	return status;
}


status_t
VulkanProgram::_CreateBuffers(const VulkanPlan& plan)
{
	// Memory the device does not have is not asked for: drivers differ
	// in what they do then, and the CPU can run the network.
	uint64 needed = plan.TensorMemory() + plan.ConstantMemory();
	if (needed > fDevice->AvailableMemory())
		return B_DEVICE_FULL;

	status_t status = fDevice->CreateBuffer(plan.TensorMemory(),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, fTensors);
	if (status == B_OK && plan.ConstantMemory() > 0) {
		status = fDevice->CreateBuffer(plan.ConstantMemory(),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
				| VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, fConstants);
	}
	if (status == B_OK) {
		status = fDevice->CreateBuffer(plan.TransferMemory(),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT
				| VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fShared);
	}
	return status;
}


/*!	Puts the weights into the device's memory, by way of memory the CPU
	can write that is given back at once. */
status_t
VulkanProgram::_UploadConstants(const VulkanPlan& plan)
{
	if (plan.ConstantMemory() == 0)
		return B_OK;

	const VulkanDeviceFunctions& vk = fDevice->Functions();
	VkDevice device = fDevice->Handle();

	VulkanBuffer staging;
	status_t status = fDevice->CreateBuffer(plan.ConstantMemory(),
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging);
	if (status != B_OK)
		return status;

	for (int32 i = 0; i < plan.CountConstants(); i++) {
		const PlanConstant& constant = plan.ConstantAt(i);
		constant.Fill(plan.Graph(),
			(float*)((uint8*)staging.address + constant.offset));
	}

	VkCommandBufferAllocateInfo allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = fCommandPool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;

	VkCommandBuffer commands;
	VkResult result = vk.AllocateCommandBuffers(device, &allocation,
		&commands);
	if (result == VK_SUCCESS) {
		VkCommandBufferBeginInfo begin;
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		result = vk.BeginCommandBuffer(commands, &begin);
	}
	if (result == VK_SUCCESS) {
		VkBufferCopy region;
		region.srcOffset = 0;
		region.dstOffset = 0;
		region.size = plan.ConstantMemory();
		vk.CmdCopyBuffer(commands, staging.buffer, fConstants.buffer, 1,
			&region);
		_Barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		result = vk.EndCommandBuffer(commands);
	}

	status = fDevice->Error(result);
	if (status == B_OK)
		status = fDevice->Submit(commands, fFence);
	if (status == B_OK) {
		status = fDevice->Wait(fFence);
		if (status == B_OK)
			vk.ResetFences(device, 1, &fFence);
	}

	// not while the device may still be reading it
	if (!fDevice->IsLost())
		fDevice->DeleteBuffer(staging);
	return status;
}


//! The piece of a buffer a step is given for one of its bindings.
void
VulkanProgram::_Region(const VulkanPlan& plan, const PlanBinding& binding,
	VkDescriptorBufferInfo& info) const
{
	int32 constant = -1;
	if (binding.kind == PLAN_BINDING_VALUE) {
		const PlanValue& value = plan.ValueAt(binding.index);
		const PlanValue& root = plan.ValueAt(value.root);
		if (root.constant < 0) {
			info.buffer = fTensors.buffer;
			info.offset = root.offset;
			info.range = value.Bytes();
			return;
		}
		constant = root.constant;
	} else
		constant = binding.index;

	info.buffer = fConstants.buffer;
	info.offset = plan.ConstantAt(constant).offset;
	info.range = plan.ConstantAt(constant).size;
}


status_t
VulkanProgram::_CreateDescriptors(const VulkanPlan& plan)
{
	const VulkanDeviceFunctions& vk = fDevice->Functions();
	VkDevice device = fDevice->Handle();
	int32 stepCount = plan.CountSteps();

	VkDescriptorPoolSize size;
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = stepCount * VULKAN_BINDING_COUNT;

	VkDescriptorPoolCreateInfo poolInfo;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = stepCount;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &size;
	VkResult result = vk.CreateDescriptorPool(device, &poolInfo, NULL,
		&fDescriptorPool);
	if (result != VK_SUCCESS) {
		fDescriptorPool = VK_NULL_HANDLE;
		return fDevice->Error(result);
	}

	fSets = (VkDescriptorSet*)calloc(stepCount, sizeof(VkDescriptorSet));
	VkDescriptorSetLayout* layouts = (VkDescriptorSetLayout*)calloc(
		stepCount, sizeof(VkDescriptorSetLayout));
	if (fSets == NULL || layouts == NULL) {
		free(layouts);
		return B_NO_MEMORY;
	}
	for (int32 i = 0; i < stepCount; i++)
		layouts[i] = fDevice->SetLayout();

	VkDescriptorSetAllocateInfo allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocation.descriptorPool = fDescriptorPool;
	allocation.descriptorSetCount = stepCount;
	allocation.pSetLayouts = layouts;
	result = vk.AllocateDescriptorSets(device, &allocation, fSets);
	free(layouts);
	if (result != VK_SUCCESS)
		return fDevice->Error(result);

	for (int32 i = 0; i < stepCount; i++) {
		const PlanStep& step = plan.StepAt(i);
		VkDescriptorBufferInfo buffers[VULKAN_BINDING_COUNT];
		VkWriteDescriptorSet writes[VULKAN_BINDING_COUNT];
		memset(writes, 0, sizeof(writes));

		// A binding the shader does not have is given what the first
		// one has: every descriptor of a set is a valid one that way.
		for (uint32 j = 0; j < VULKAN_BINDING_COUNT; j++) {
			_Region(plan, step.bindings[j].kind != PLAN_BINDING_NONE
				? step.bindings[j] : step.bindings[0], buffers[j]);

			writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[j].dstSet = fSets[i];
			writes[j].dstBinding = j;
			writes[j].descriptorCount = 1;
			writes[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[j].pBufferInfo = &buffers[j];
		}
		vk.UpdateDescriptorSets(device, VULKAN_BINDING_COUNT, writes, 0,
			NULL);
	}

	return B_OK;
}


void
VulkanProgram::_Barrier(VkCommandBuffer commands, VkPipelineStageFlags from,
	VkAccessFlags fromAccess, VkPipelineStageFlags to, VkAccessFlags toAccess)
{
	VkMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = fromAccess;
	barrier.dstAccessMask = toAccess;
	fDevice->Functions().CmdPipelineBarrier(commands, from, to, 0, 1,
		&barrier, 0, NULL, 0, NULL);
}


//! The copies between the shared memory and the tensors.
void
VulkanProgram::_RecordCopies(VkCommandBuffer commands,
	const VulkanPlan& plan, bool in)
{
	const VulkanDeviceFunctions& vk = fDevice->Functions();
	int32 count = in ? plan.CountInputs() : plan.CountOutputs();
	if (count == 0)
		return;

	if (!in) {
		_Barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
	}

	for (int32 i = 0; i < count; i++) {
		const PlanTransfer& transfer = in ? plan.InputAt(i)
			: plan.OutputAt(i);
		const PlanValue& value = plan.ValueAt(transfer.value);
		const PlanValue& root = plan.ValueAt(value.root);

		VkBufferCopy region;
		region.srcOffset = in ? transfer.offset : root.offset;
		region.dstOffset = in ? root.offset : transfer.offset;
		region.size = value.Bytes();
		vk.CmdCopyBuffer(commands, in ? fShared.buffer : fTensors.buffer,
			in ? fTensors.buffer : fShared.buffer, 1, &region);
	}

	if (in) {
		_Barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	} else {
		_Barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			VK_ACCESS_HOST_READ_BIT);
	}
}


status_t
VulkanProgram::_RecordStep(VkCommandBuffer commands, const VulkanPlan& plan,
	int32 index, VkPipeline& bound)
{
	const VulkanDeviceFunctions& vk = fDevice->Functions();
	const PlanStep& step = plan.StepAt(index);
	const PlanVariant& variant = step.variants[fChoices[index]];

	VkPipeline pipeline;
	status_t status = fDevice->GetPipeline(variant.shader, &pipeline);
	if (status != B_OK)
		return status;

	PlanWord words[VULKAN_PUSH_CONSTANT_SIZE / 4];
	memcpy(words, step.words, sizeof(words));
	words[0].u = variant.total;
	words[1].u = variant.row_size;

	if (pipeline != bound) {
		vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline);
		bound = pipeline;
	}
	vk.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
		fDevice->PipelineLayout(), 0, 1, &fSets[index], 0, NULL);
	vk.CmdPushConstants(commands, fDevice->PipelineLayout(),
		VK_SHADER_STAGE_COMPUTE_BIT, 0, step.word_count * 4, words);
	vk.CmdDispatch(commands, variant.groups[0], variant.groups[1],
		variant.groups[2]);

	// the next step reads what this one wrote
	_Barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	return B_OK;
}


status_t
VulkanProgram::_CreateCommands()
{
	const VulkanDeviceFunctions& vk = fDevice->Functions();
	VkDevice device = fDevice->Handle();

	// _Tune() records over and over into one buffer
	VkCommandPoolCreateInfo poolInfo;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = fDevice->QueueFamily();
	VkResult result = vk.CreateCommandPool(device, &poolInfo, NULL,
		&fCommandPool);
	if (result != VK_SUCCESS) {
		fCommandPool = VK_NULL_HANDLE;
		return fDevice->Error(result);
	}

	VkFenceCreateInfo fenceInfo;
	memset(&fenceInfo, 0, sizeof(fenceInfo));
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vk.CreateFence(device, &fenceInfo, NULL, &fFence);
	if (result != VK_SUCCESS) {
		fFence = VK_NULL_HANDLE;
		return fDevice->Error(result);
	}

	return B_OK;
}


/*!	Picks the variant of each step that has a choice: each is run on the
	device a few times, on whatever is in the tensors, and the one that
	took the least time stays. NEURAL_VULKAN_VARIANT names one to use
	instead, for comparing them. */
status_t
VulkanProgram::_Tune(const VulkanPlan& plan)
{
	const int32 kRuns = 3;
		// the first one does not count: it may have to wake things up

	const char* forced = getenv("NEURAL_VULKAN_VARIANT");
	if (forced != NULL) {
		for (int32 i = 0; i < plan.CountSteps(); i++) {
			uint32 count = plan.StepAt(i).variant_count;
			uint32 wanted = atoi(forced);
			fChoices[i] = wanted < count ? wanted : 0;
		}
		return B_OK;
	}

	const VulkanDeviceFunctions& vk = fDevice->Functions();
	VkDevice device = fDevice->Handle();

	VkCommandBufferAllocateInfo allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = fCommandPool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;

	VkCommandBuffer commands = VK_NULL_HANDLE;
	VkCommandBufferBeginInfo begin;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	for (int32 i = 0; i < plan.CountSteps(); i++) {
		const PlanStep& step = plan.StepAt(i);
		if (step.variant_count < 2)
			continue;

		if (commands == VK_NULL_HANDLE) {
			VkResult result = vk.AllocateCommandBuffers(device, &allocation,
				&commands);
			if (result != VK_SUCCESS)
				return fDevice->Error(result);
		}

		uint32 best = 0;
		bigtime_t bestTime = B_INFINITE_TIMEOUT;
		for (uint32 j = 0; j < step.variant_count; j++) {
			fChoices[i] = j;

			// beginning it again empties it
			VkPipeline bound = VK_NULL_HANDLE;
			VkResult result = vk.BeginCommandBuffer(commands, &begin);
			if (result != VK_SUCCESS)
				return fDevice->Error(result);
			status_t status = _RecordStep(commands, plan, i, bound);
			if (status != B_OK)
				return status;
			result = vk.EndCommandBuffer(commands);
			if (result != VK_SUCCESS)
				return fDevice->Error(result);

			for (int32 k = 0; k < kRuns; k++) {
				bigtime_t start = system_time();
				status = fDevice->Submit(commands, fFence);
				if (status == B_OK)
					status = fDevice->Wait(fFence);
				if (status != B_OK)
					return status;
				vk.ResetFences(device, 1, &fFence);

				bigtime_t time = system_time() - start;
				if (k > 0 && time < bestTime) {
					bestTime = time;
					best = j;
				}
			}
		}
		fChoices[i] = best;
	}

	return B_OK;
}


/*!	Records the commands of a run: one buffer with everything, or, to
	see where the time goes (NEURAL_VULKAN_PROFILE), one for each step,
	which costs a submission each. */
status_t
VulkanProgram::_Record(const VulkanPlan& plan)
{
	const VulkanDeviceFunctions& vk = fDevice->Functions();
	VkDevice device = fDevice->Handle();
	int32 stepCount = plan.CountSteps();
	bool profile = has_setting("NEURAL_VULKAN_PROFILE");
	VkResult result;

	fCommandCount = profile ? stepCount + 2 : 1;
	fCommands = (VkCommandBuffer*)calloc(fCommandCount,
		sizeof(VkCommandBuffer));
	if (fCommands == NULL)
		return B_NO_MEMORY;
	if (profile) {
		fPhases = (Phase*)calloc(fCommandCount, sizeof(Phase));
		if (fPhases == NULL)
			return B_NO_MEMORY;
	}

	VkCommandBufferAllocateInfo allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = fCommandPool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = fCommandCount;
	result = vk.AllocateCommandBuffers(device, &allocation, fCommands);
	if (result != VK_SUCCESS)
		return fDevice->Error(result);

	VkCommandBufferBeginInfo begin;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	for (int32 i = 0; i < fCommandCount; i++) {
		VkCommandBuffer commands = fCommands[i];
		result = vk.BeginCommandBuffer(commands, &begin);
		if (result != VK_SUCCESS)
			return fDevice->Error(result);

		VkPipeline bound = VK_NULL_HANDLE;
		status_t status = B_OK;
		if (!profile) {
			_RecordCopies(commands, plan, true);
			for (int32 j = 0; j < stepCount && status == B_OK; j++)
				status = _RecordStep(commands, plan, j, bound);
			_RecordCopies(commands, plan, false);
		} else if (i == 0 || i == fCommandCount - 1) {
			_RecordCopies(commands, plan, i == 0);
			fPhases[i].op = -1;
		} else {
			const PlanStep& step = plan.StepAt(i - 1);
			status = _RecordStep(commands, plan, i - 1, bound);
			fPhases[i].shader = step.variants[fChoices[i - 1]].shader;
			fPhases[i].op = step.op;
			fPhases[i].code = plan.Graph().OperatorAt(step.op)->code;
		}
		if (status != B_OK)
			return status;

		result = vk.EndCommandBuffer(commands);
		if (result != VK_SUCCESS)
			return fDevice->Error(result);
	}

	return B_OK;
}


status_t
VulkanProgram::Run(void* const* tensors)
{
	BAutolock locker(fLock);
	if (fDevice->IsLost())
		return B_NEURAL_DEVICE_LOST;

	bigtime_t start = system_time();
	for (int32 i = 0; i < fInputCount; i++) {
		const Transfer& input = fInputs[i];
		copy_to_device((float*)((uint8*)fShared.address + input.offset),
			(const float*)tensors[input.tensor], input.positions,
			input.channels, input.groups);
	}
	bigtime_t copiedIn = system_time();

	bigtime_t submitTime = 0;
	bigtime_t waitTime = 0;
	for (int32 i = 0; i < fCommandCount; i++) {
		bigtime_t before = system_time();
		status_t status = fDevice->Submit(fCommands[i], fFence);
		bigtime_t submitted = system_time();
		if (status == B_OK)
			status = fDevice->Wait(fFence);
		if (status != B_OK) {
			// B_NEURAL_DEVICE_LOST has the kit go on with the CPU; any
			// other failure of a device is no better
			fDevice->SetLost();
			return B_NEURAL_DEVICE_LOST;
		}
		fDevice->Functions().ResetFences(fDevice->Handle(), 1, &fFence);

		bigtime_t done = system_time();
		submitTime += submitted - before;
		waitTime += done - submitted;
		if (fPhases != NULL)
			fPhases[i].time += done - before;
	}
	bigtime_t finished = system_time();

	for (int32 i = 0; i < fOutputCount; i++) {
		const Transfer& output = fOutputs[i];
		copy_from_device((float*)tensors[output.tensor],
			(const float*)((uint8*)fShared.address + output.offset),
			output.positions, output.channels, output.groups);
	}

	fRuns++;
	fCopyInTime += copiedIn - start;
	fSubmitTime += submitTime;
	fWaitTime += waitTime;
	fCopyOutTime += system_time() - finished;
	fLastDeviceTime = finished - copiedIn;
	return B_OK;
}


bigtime_t
VulkanProgram::LastDeviceTime() const
{
	return fLastDeviceTime;
}


void
VulkanProgram::_PrintStatistics() const
{
	fprintf(stderr, "vulkan.neural: %" B_PRId64 " runs: copy in %.1f us, "
		"submit %.1f us, wait %.1f us, copy out %.1f us\n", fRuns,
		(double)fCopyInTime / fRuns, (double)fSubmitTime / fRuns,
		(double)fWaitTime / fRuns, (double)fCopyOutTime / fRuns);

	if (fPhases == NULL)
		return;
	for (int32 i = 0; i < fCommandCount; i++) {
		const Phase& phase = fPhases[i];
		if (phase.op < 0) {
			fprintf(stderr, "       %-22s %-22s %9.1f us\n",
				i == 0 ? "(copy in)" : "(copy out)", "",
				(double)phase.time / fRuns);
		} else {
			fprintf(stderr, "  %4" B_PRId32 " %-22s %-22s %9.1f us\n",
				phase.op, neural_operator_name(phase.code),
				vulkan_shader_name(phase.shader),
				(double)phase.time / fRuns);
		}
	}
}
