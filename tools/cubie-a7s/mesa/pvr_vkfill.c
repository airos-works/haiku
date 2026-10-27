/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// A compute job on the PowerVR GPU through Mesa's Vulkan driver: a shader
// writes gl_GlobalInvocationID.x * 3 + 1 into each word of a 1 MiB storage
// buffer in HOST_VISIBLE | HOST_COHERENT memory. One dispatch, a fence wait,
// then every word is checked. The buffer is filled with 0xdeadbeef first, so
// words the GPU never wrote show up as such.
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vkfill [runs] [timeout-ms]
//       [--cached]
// (defaults: 1 run, 5000 ms fence timeout)
// --cached uses HOST_VISIBLE | HOST_CACHED memory, which is not coherent on
// air/OS: the fill is flushed (vkFlushMappedMemoryRanges) before the
// dispatch, and the buffer invalidated (vkInvalidateMappedMemoryRanges)
// before it is checked.


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

#define BUFFER_SIZE		(1024 * 1024)
#define WORD_COUNT		(BUFFER_SIZE / 4)
#define LOCAL_SIZE		64
#define FILL_PATTERN	0xdeadbeefu

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


// The shader. There is no GLSL compiler in the build environment: this is
// the SPIR-V 1.0 module of the assembly below, assembled with SPIRV-Tools'
// spirv-as (--target-env spv1.0) and validated with spirv-val
// (--target-env vulkan1.0), both from toolchains/mesa-native-deps.
//
//   GLSL equivalent:
//     #version 450
//     layout(local_size_x = 64) in;
//     layout(std430, set = 0, binding = 0) buffer Out { uint data[]; };
//     void main() { uint i = gl_GlobalInvocationID.x; data[i] = i * 3u + 1u; }
//
//                OpCapability Shader
//                OpMemoryModel Logical GLSL450
//                OpEntryPoint GLCompute %main "main" %gid
//                OpExecutionMode %main LocalSize 64 1 1
//                OpDecorate %gid BuiltIn GlobalInvocationId
//                OpDecorate %rta ArrayStride 4
//                OpMemberDecorate %Out 0 Offset 0
//                OpDecorate %Out BufferBlock
//                OpDecorate %buf DescriptorSet 0
//                OpDecorate %buf Binding 0
//        %void = OpTypeVoid
//      %fnvoid = OpTypeFunction %void
//        %uint = OpTypeInt 32 0
//         %int = OpTypeInt 32 1
//      %v3uint = OpTypeVector %uint 3
//    %ptr_in_3 = OpTypePointer Input %v3uint
//         %gid = OpVariable %ptr_in_3 Input
//         %rta = OpTypeRuntimeArray %uint
//         %Out = OpTypeStruct %rta
//     %ptr_Out = OpTypePointer Uniform %Out
//         %buf = OpVariable %ptr_Out Uniform
//       %int_0 = OpConstant %int 0
//      %uint_0 = OpConstant %uint 0
//      %uint_1 = OpConstant %uint 1
//      %uint_3 = OpConstant %uint 3
//    %ptr_in_u = OpTypePointer Input %uint
//     %ptr_u_u = OpTypePointer Uniform %uint
//        %main = OpFunction %void None %fnvoid
//       %entry = OpLabel
//          %px = OpAccessChain %ptr_in_u %gid %uint_0
//           %x = OpLoad %uint %px
//           %m = OpIMul %uint %x %uint_3
//           %v = OpIAdd %uint %m %uint_1
//         %dst = OpAccessChain %ptr_u_u %buf %int_0 %x
//                OpStore %dst %v
//                OpReturn
//                OpFunctionEnd
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


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	int runs = 1;
	uint64_t timeoutMs = 5000;
	int cached = 0;
	int positional = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--cached") == 0)
			cached = 1;
		else if (positional++ == 0)
			runs = atoi(argv[i]);
		else
			timeoutMs = strtoull(argv[i], NULL, 0);
	}
	if (runs < 1)
		runs = 1;

	PFN_vkCreateInstance vkCreateInstance
		= (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
			"vkCreateInstance");
	if (vkCreateInstance == NULL) {
		printf("FAIL: no vkCreateInstance\n");
		return 1;
	}
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vkfill",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties);
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

	// a queue family that can compute
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
	DEVICE_FN(vkCreateBuffer);
	DEVICE_FN(vkGetBufferMemoryRequirements);
	DEVICE_FN(vkAllocateMemory);
	DEVICE_FN(vkBindBufferMemory);
	DEVICE_FN(vkMapMemory);
	DEVICE_FN(vkUnmapMemory);
	DEVICE_FN(vkFlushMappedMemoryRanges);
	DEVICE_FN(vkInvalidateMappedMemoryRanges);
	DEVICE_FN(vkCreateDescriptorSetLayout);
	DEVICE_FN(vkCreatePipelineLayout);
	DEVICE_FN(vkCreateDescriptorPool);
	DEVICE_FN(vkAllocateDescriptorSets);
	DEVICE_FN(vkUpdateDescriptorSets);
	DEVICE_FN(vkCreateShaderModule);
	DEVICE_FN(vkCreateComputePipelines);
	DEVICE_FN(vkCreateCommandPool);
	DEVICE_FN(vkAllocateCommandBuffers);
	DEVICE_FN(vkBeginCommandBuffer);
	DEVICE_FN(vkCmdBindPipeline);
	DEVICE_FN(vkCmdBindDescriptorSets);
	DEVICE_FN(vkCmdDispatch);
	DEVICE_FN(vkCmdPipelineBarrier);
	DEVICE_FN(vkEndCommandBuffer);
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkResetFences);
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
		| (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
			: VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	uint32_t memoryType = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0
			&& (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
			memoryType = i;
			break;
		}
	}
	if (memoryType == UINT32_MAX) {
		printf("FAIL: no HOST_VISIBLE | %s memory type\n",
			cached ? "HOST_CACHED" : "HOST_COHERENT");
		return 1;
	}
	const int coherent = (memory.memoryTypes[memoryType].propertyFlags
		& VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
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
	printf("buffer: %u bytes, memory type %u (flags 0x%x%s), mapped at %p\n",
		BUFFER_SIZE, memoryType,
		(unsigned)memory.memoryTypes[memoryType].propertyFlags,
		coherent ? "" : ", flushed and invalidated", (void*)words);
	VkMappedMemoryRange wholeRange = {
		.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = deviceMemory,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};

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
	double start = now_ms();
	CHECK(vkCreateComputePipelines(sDevice, VK_NULL_HANDLE, 1, &pipelineInfo,
		NULL, &pipeline));
	printf("pipeline: compiled in %.2f ms\n", now_ms() - start);

	// the command buffer: dispatch, then make the writes visible to the host
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
	};
	CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
	vkCmdDispatch(commandBuffer, WORD_COUNT / LOCAL_SIZE, 1, 1);
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	CHECK(vkEndCommandBuffer(commandBuffer));

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));

	int failedRuns = 0;
	for (int run = 0; run < runs; run++) {
		for (uint32_t i = 0; i < WORD_COUNT; i++)
			words[i] = FILL_PATTERN;
		if (!coherent)
			CHECK(vkFlushMappedMemoryRanges(sDevice, 1, &wholeRange));

		VkSubmitInfo submitInfo = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &commandBuffer,
		};
		CHECK(vkResetFences(sDevice, 1, &fence));
		start = now_ms();
		CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
		double submitted = now_ms();
		result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE,
			timeoutMs * 1000000ull);
		double done = now_ms();
		printf("run %d: submit %.3f ms, fence %s after %.3f ms "
			"(%u workgroups of %u)\n", run, submitted - start,
			result == VK_SUCCESS ? "signalled"
				: result == VK_TIMEOUT ? "TIMED OUT" : "FAILED",
			done - submitted, WORD_COUNT / LOCAL_SIZE, LOCAL_SIZE);
		if (result != VK_SUCCESS) {
			printf("FAIL: vkWaitForFences: %d\n", (int)result);
			failedRuns++;
			break;
		}

		uint32_t mismatches = 0;
		uint32_t untouched = 0;
		start = now_ms();
		if (!coherent)
			CHECK(vkInvalidateMappedMemoryRanges(sDevice, 1, &wholeRange));
		for (uint32_t i = 0; i < WORD_COUNT; i++) {
			uint32_t expected = i * 3 + 1;
			if (words[i] == expected)
				continue;
			if (words[i] == FILL_PATTERN)
				untouched++;
			if (mismatches < 16) {
				printf("  word %6u (offset 0x%06x): 0x%08x, expected 0x%08x\n",
					i, i * 4, words[i], expected);
			}
			mismatches++;
		}
		printf("run %d: %u of %u words wrong (%u never written), "
			"checked in %.3f ms\n", run, mismatches, (unsigned)WORD_COUNT,
			untouched, now_ms() - start);
		if (mismatches != 0)
			failedRuns++;
	}

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

	printf("%s\n", failedRuns == 0 ? "PASS" : "FAIL");
	return failedRuns == 0 ? 0 : 1;
}
