/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// One triangle through Mesa's PowerVR Vulkan driver, checked byte for byte.
// A graphics pipeline draws into a 64x64 VK_FORMAT_R8G8B8A8_UNORM image
// cleared in the render pass. Then the image is copied into a HOST_VISIBLE
// buffer and every byte is compared with a CPU reference.
//
// The reference is unambiguous: the fragment colour is a constant (no
// interpolation), and the triangle's corners are the framebuffer points
// (0, 0), (64.25, 0) and (0, 64.25), from gl_VertexIndex, without a vertex
// buffer. A pixel (x, y) has its centre at x + y + 1, never on the edge
// x + y = 64.25: it is covered if and only if x + y <= 63. The colours are
// k/255 values whose float form lies just above k/255, so rounding to
// nearest and truncation both give k.
//
// On the GPU: a render (geometry + fragment) job with a free list and an
// HWRT data set, then a transfer (TRANSFER_FRAG) job for the copy.
// --linear renders into a LINEAR image in host visible memory and reads it
// in place: no copy, so no transfer job.
//
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr_vktriangle [--linear]
//       [--ppm FILE] [--timeout MS]


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

#define SIZE			64
#define FORMAT			VK_FORMAT_R8G8B8A8_UNORM
#define FILL_BYTE		0xee

// clear (0.2, 0.4, 0.6, 1.0), triangle (0.8, 0.6, 0.2, 0.4)
static const float kClear[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
static const uint8_t kClearBytes[4] = { 51, 102, 153, 255 };
static const uint8_t kTriangleBytes[4] = { 204, 153, 51, 102 };

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


// The shaders: SPIR-V 1.0 modules of the assembly below, assembled with
// SPIRV-Tools' spirv-as (--target-env spv1.0) and validated with spirv-val
// (--target-env vulkan1.0), both from toolchains/mesa-native-deps.
//
// vertex (GLSL equivalent:
//   gl_Position = vec4(gl_VertexIndex == 1 ? 1.0078125 : -1.0,
//                      gl_VertexIndex == 2 ? 1.0078125 : -1.0, 0.0, 1.0);
// 1.0078125 in NDC is framebuffer coordinate 64.25):
//                OpCapability Shader
//                OpMemoryModel Logical GLSL450
//                OpEntryPoint Vertex %main "main" %idx %out
//                OpDecorate %idx BuiltIn VertexIndex
//                OpMemberDecorate %PerVertex 0 BuiltIn Position
//                OpDecorate %PerVertex Block
//        %void = OpTypeVoid
//      %fnvoid = OpTypeFunction %void
//         %int = OpTypeInt 32 1
//       %float = OpTypeFloat 32
//        %bool = OpTypeBool
//     %v4float = OpTypeVector %float 4
//   %PerVertex = OpTypeStruct %v4float
//   %ptr_out_s = OpTypePointer Output %PerVertex
//         %out = OpVariable %ptr_out_s Output
//  %ptr_in_int = OpTypePointer Input %int
//         %idx = OpVariable %ptr_in_int Input
//  %ptr_out_v4 = OpTypePointer Output %v4float
//       %int_0 = OpConstant %int 0
//       %int_1 = OpConstant %int 1
//       %int_2 = OpConstant %int 2
//      %f_near = OpConstant %float -1
//       %f_far = OpConstant %float 1.0078125
//      %f_zero = OpConstant %float 0
//       %f_one = OpConstant %float 1
//        %main = OpFunction %void None %fnvoid
//       %entry = OpLabel
//           %i = OpLoad %int %idx
//         %is1 = OpIEqual %bool %i %int_1
//         %is2 = OpIEqual %bool %i %int_2
//           %x = OpSelect %float %is1 %f_far %f_near
//           %y = OpSelect %float %is2 %f_far %f_near
//           %p = OpCompositeConstruct %v4float %x %y %f_zero %f_one
//         %dst = OpAccessChain %ptr_out_v4 %out %int_0
//                OpStore %dst %p
//                OpReturn
//                OpFunctionEnd
static const uint32_t kVertexShader[] = {
	0x07230203, 0x00010000, 0x00070000, 0x0000001d, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
	0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00000003, 0x00040047,
	0x00000002, 0x0000000b, 0x0000002a, 0x00050048, 0x00000004, 0x00000000,
	0x0000000b, 0x00000000, 0x00030047, 0x00000004, 0x00000002, 0x00020013,
	0x00000005, 0x00030021, 0x00000006, 0x00000005, 0x00040015, 0x00000007,
	0x00000020, 0x00000001, 0x00030016, 0x00000008, 0x00000020, 0x00020014,
	0x00000009, 0x00040017, 0x0000000a, 0x00000008, 0x00000004, 0x0003001e,
	0x00000004, 0x0000000a, 0x00040020, 0x0000000b, 0x00000003, 0x00000004,
	0x0004003b, 0x0000000b, 0x00000003, 0x00000003, 0x00040020, 0x0000000c,
	0x00000001, 0x00000007, 0x0004003b, 0x0000000c, 0x00000002, 0x00000001,
	0x00040020, 0x0000000d, 0x00000003, 0x0000000a, 0x0004002b, 0x00000007,
	0x0000000e, 0x00000000, 0x0004002b, 0x00000007, 0x0000000f, 0x00000001,
	0x0004002b, 0x00000007, 0x00000010, 0x00000002, 0x0004002b, 0x00000008,
	0x00000011, 0xbf800000, 0x0004002b, 0x00000008, 0x00000012, 0x3f810000,
	0x0004002b, 0x00000008, 0x00000013, 0x00000000, 0x0004002b, 0x00000008,
	0x00000014, 0x3f800000, 0x00050036, 0x00000005, 0x00000001, 0x00000000,
	0x00000006, 0x000200f8, 0x00000015, 0x0004003d, 0x00000007, 0x00000016,
	0x00000002, 0x000500aa, 0x00000009, 0x00000017, 0x00000016, 0x0000000f,
	0x000500aa, 0x00000009, 0x00000018, 0x00000016, 0x00000010, 0x000600a9,
	0x00000008, 0x00000019, 0x00000017, 0x00000012, 0x00000011, 0x000600a9,
	0x00000008, 0x0000001a, 0x00000018, 0x00000012, 0x00000011, 0x00070050,
	0x0000000a, 0x0000001b, 0x00000019, 0x0000001a, 0x00000013, 0x00000014,
	0x00050041, 0x0000000d, 0x0000001c, 0x00000003, 0x0000000e, 0x0003003e,
	0x0000001c, 0x0000001b, 0x000100fd, 0x00010038,
};

// fragment (GLSL equivalent: color = vec4(0.8, 0.6, 0.2, 0.4)):
//                OpCapability Shader
//                OpMemoryModel Logical GLSL450
//                OpEntryPoint Fragment %main "main" %color
//                OpExecutionMode %main OriginUpperLeft
//                OpDecorate %color Location 0
//        %void = OpTypeVoid
//      %fnvoid = OpTypeFunction %void
//       %float = OpTypeFloat 32
//     %v4float = OpTypeVector %float 4
//  %ptr_out_v4 = OpTypePointer Output %v4float
//       %color = OpVariable %ptr_out_v4 Output
//         %f_r = OpConstant %float 0.8
//         %f_g = OpConstant %float 0.6
//         %f_b = OpConstant %float 0.2
//         %f_a = OpConstant %float 0.4
//        %rgba = OpConstantComposite %v4float %f_r %f_g %f_b %f_a
//        %main = OpFunction %void None %fnvoid
//       %entry = OpLabel
//                OpStore %color %rgba
//                OpReturn
//                OpFunctionEnd
static const uint32_t kFragmentShader[] = {
	0x07230203, 0x00010000, 0x00070000, 0x0000000e, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
	0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00030010, 0x00000001,
	0x00000007, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00020013,
	0x00000003, 0x00030021, 0x00000004, 0x00000003, 0x00030016, 0x00000005,
	0x00000020, 0x00040017, 0x00000006, 0x00000005, 0x00000004, 0x00040020,
	0x00000007, 0x00000003, 0x00000006, 0x0004003b, 0x00000007, 0x00000002,
	0x00000003, 0x0004002b, 0x00000005, 0x00000008, 0x3f4ccccd, 0x0004002b,
	0x00000005, 0x00000009, 0x3f19999a, 0x0004002b, 0x00000005, 0x0000000a,
	0x3e4ccccd, 0x0004002b, 0x00000005, 0x0000000b, 0x3ecccccd, 0x0007002c,
	0x00000006, 0x0000000c, 0x00000008, 0x00000009, 0x0000000a, 0x0000000b,
	0x00050036, 0x00000003, 0x00000001, 0x00000000, 0x00000004, 0x000200f8,
	0x0000000d, 0x0003003e, 0x00000002, 0x0000000c, 0x000100fd, 0x00010038,
};


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


static const uint8_t*
expected_pixel(uint32_t x, uint32_t y)
{
	return x + y <= SIZE - 1 ? kTriangleBytes : kClearBytes;
}


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
	printf("FAIL: no memory type 0x%x in 0x%x\n", wanted, typeBits);
	exit(1);
}


// Compares the SIZE x SIZE RGBA8 pixels at "pixels" (rows "pitch" bytes
// apart) with the reference; prints the first mismatches, a map of every
// fourth row and column, and returns the number of wrong pixels.
static uint32_t
check_pixels(const uint8_t* pixels, size_t pitch)
{
	uint32_t wrongPixels = 0;
	uint32_t wrongBytes = 0;
	uint32_t triangle = 0;
	uint32_t clear = 0;
	uint32_t untouched = 0;
	for (uint32_t y = 0; y < SIZE; y++) {
		for (uint32_t x = 0; x < SIZE; x++) {
			const uint8_t* got = pixels + y * pitch + x * 4;
			const uint8_t* want = expected_pixel(x, y);
			if (memcmp(got, kTriangleBytes, 4) == 0)
				triangle++;
			else if (memcmp(got, kClearBytes, 4) == 0)
				clear++;
			else if (got[0] == FILL_BYTE && got[1] == FILL_BYTE
				&& got[2] == FILL_BYTE && got[3] == FILL_BYTE)
				untouched++;
			if (memcmp(got, want, 4) == 0)
				continue;
			for (int c = 0; c < 4; c++)
				wrongBytes += got[c] != want[c];
			if (wrongPixels < 16) {
				printf("  pixel (%2u, %2u): %3u %3u %3u %3u, expected "
					"%3u %3u %3u %3u\n", x, y, got[0], got[1], got[2], got[3],
					want[0], want[1], want[2], want[3]);
			}
			wrongPixels++;
		}
	}
	printf("pixels: %u wrong (%u bytes); %u triangle, %u clear, %u never "
		"written, %u other (expected %u triangle, %u clear)\n", wrongPixels,
		wrongBytes, triangle, clear, untouched,
		SIZE * SIZE - triangle - clear - untouched, SIZE * (SIZE + 1) / 2,
		SIZE * SIZE - SIZE * (SIZE + 1) / 2);
	if (wrongPixels != 0) {
		printf("every 4th row and column (T triangle, . clear, - never "
			"written, ? other; lower case: wrong):\n");
		for (uint32_t y = 0; y < SIZE; y += 4) {
			printf("  ");
			for (uint32_t x = 0; x < SIZE; x += 4) {
				const uint8_t* got = pixels + y * pitch + x * 4;
				int ok = memcmp(got, expected_pixel(x, y), 4) == 0;
				char c = '?';
				if (memcmp(got, kTriangleBytes, 4) == 0)
					c = ok ? 'T' : 't';
				else if (memcmp(got, kClearBytes, 4) == 0)
					c = ok ? '.' : ',';
				else if (got[0] == FILL_BYTE && got[3] == FILL_BYTE)
					c = '-';
				putchar(c);
			}
			putchar('\n');
		}
	}
	return wrongPixels;
}


static void
write_ppm(const char* path, const uint8_t* pixels, size_t pitch)
{
	FILE* file = fopen(path, "wb");
	if (file == NULL) {
		printf("cannot write %s\n", path);
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", SIZE, SIZE);
	for (uint32_t y = 0; y < SIZE; y++) {
		for (uint32_t x = 0; x < SIZE; x++)
			fwrite(pixels + y * pitch + x * 4, 1, 3, file);
	}
	fclose(file);
	printf("image (RGB, alpha dropped) written to %s\n", path);
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	int linear = 0;
	const char* ppmPath = NULL;
	uint64_t timeoutMs = 5000;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--linear") == 0)
			linear = 1;
		else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
			ppmPath = argv[++i];
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			timeoutMs = strtoull(argv[++i], NULL, 0);
		else {
			printf("usage: %s [--linear] [--ppm FILE] [--timeout MS]\n",
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
		.pApplicationName = "pvr_vktriangle",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties);
	INSTANCE_FN(vkGetPhysicalDeviceFormatProperties);
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
	printf("device: %s; %dx%d R8G8B8A8_UNORM, %s image%s\n",
		properties.deviceName, SIZE, SIZE, linear ? "LINEAR" : "OPTIMAL",
		linear ? ", read in place" : ", copied to a buffer");

	VkFormatProperties formatProperties;
	vkGetPhysicalDeviceFormatProperties(physicalDevice, FORMAT,
		&formatProperties);
	VkFormatFeatureFlags features = linear
		? formatProperties.linearTilingFeatures
		: formatProperties.optimalTilingFeatures;
	VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
		| (linear ? 0 : VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
	if ((features & needed) != needed) {
		printf("FAIL: R8G8B8A8_UNORM %s features 0x%x lack 0x%x\n",
			linear ? "linear" : "optimal", features, needed);
		return 1;
	}

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
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			family = i;
			break;
		}
	}
	if (family == UINT32_MAX) {
		printf("FAIL: no graphics queue family\n");
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
	DEVICE_FN(vkCreateImage);
	DEVICE_FN(vkGetImageMemoryRequirements);
	DEVICE_FN(vkGetImageSubresourceLayout);
	DEVICE_FN(vkBindImageMemory);
	DEVICE_FN(vkCreateImageView);
	DEVICE_FN(vkCreateBuffer);
	DEVICE_FN(vkGetBufferMemoryRequirements);
	DEVICE_FN(vkBindBufferMemory);
	DEVICE_FN(vkAllocateMemory);
	DEVICE_FN(vkMapMemory);
	DEVICE_FN(vkCreateRenderPass);
	DEVICE_FN(vkCreateFramebuffer);
	DEVICE_FN(vkCreateShaderModule);
	DEVICE_FN(vkCreatePipelineLayout);
	DEVICE_FN(vkCreateGraphicsPipelines);
	DEVICE_FN(vkCreateCommandPool);
	DEVICE_FN(vkAllocateCommandBuffers);
	DEVICE_FN(vkBeginCommandBuffer);
	DEVICE_FN(vkCmdBeginRenderPass);
	DEVICE_FN(vkCmdBindPipeline);
	DEVICE_FN(vkCmdDraw);
	DEVICE_FN(vkCmdEndRenderPass);
	DEVICE_FN(vkCmdCopyImageToBuffer);
	DEVICE_FN(vkCmdPipelineBarrier);
	DEVICE_FN(vkEndCommandBuffer);
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkQueueSubmit);
	DEVICE_FN(vkWaitForFences);
	DEVICE_FN(vkDeviceWaitIdle);
	DEVICE_FN(vkDestroyFence);
	DEVICE_FN(vkDestroyCommandPool);
	DEVICE_FN(vkDestroyPipeline);
	DEVICE_FN(vkDestroyPipelineLayout);
	DEVICE_FN(vkDestroyShaderModule);
	DEVICE_FN(vkDestroyFramebuffer);
	DEVICE_FN(vkDestroyRenderPass);
	DEVICE_FN(vkDestroyImageView);
	DEVICE_FN(vkDestroyImage);
	DEVICE_FN(vkDestroyBuffer);
	DEVICE_FN(vkFreeMemory);
	DEVICE_FN(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(sDevice, family, 0, &queue);

	// the image
	VkImageCreateInfo imageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FORMAT,
		.extent = { SIZE, SIZE, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| (linear ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage image;
	CHECK(vkCreateImage(sDevice, &imageInfo, NULL, &image));
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(sDevice, image, &requirements);
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = find_memory_type(physicalDevice,
			requirements.memoryTypeBits, linear
				? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
					| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
				: 0),
	};
	VkDeviceMemory imageMemory;
	CHECK(vkAllocateMemory(sDevice, &allocateInfo, NULL, &imageMemory));
	CHECK(vkBindImageMemory(sDevice, image, imageMemory, 0));

	VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = FORMAT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageView view;
	CHECK(vkCreateImageView(sDevice, &viewInfo, NULL, &view));

	// where the pixels are read: the image itself, or a buffer
	const uint8_t* pixels;
	size_t pitch;
	size_t readSize;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
	if (linear) {
		VkImageSubresource subresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		VkSubresourceLayout layout;
		vkGetImageSubresourceLayout(sDevice, image, &subresource, &layout);
		void* map;
		CHECK(vkMapMemory(sDevice, imageMemory, 0, VK_WHOLE_SIZE, 0, &map));
		pixels = (const uint8_t*)map + layout.offset;
		pitch = layout.rowPitch;
		readSize = layout.size;
		memset((uint8_t*)map + layout.offset, FILL_BYTE, readSize);
		printf("image: %llu bytes, row pitch %zu, mapped at %p\n",
			(unsigned long long)requirements.size, pitch, map);
	} else {
		VkBufferCreateInfo bufferInfo = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = SIZE * SIZE * 4,
			.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		CHECK(vkCreateBuffer(sDevice, &bufferInfo, NULL, &buffer));
		VkMemoryRequirements bufferRequirements;
		vkGetBufferMemoryRequirements(sDevice, buffer, &bufferRequirements);
		VkMemoryAllocateInfo bufferAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = bufferRequirements.size,
			.memoryTypeIndex = find_memory_type(physicalDevice,
				bufferRequirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
					| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
		};
		CHECK(vkAllocateMemory(sDevice, &bufferAllocateInfo, NULL,
			&bufferMemory));
		CHECK(vkBindBufferMemory(sDevice, buffer, bufferMemory, 0));
		void* map;
		CHECK(vkMapMemory(sDevice, bufferMemory, 0, VK_WHOLE_SIZE, 0, &map));
		pixels = map;
		pitch = SIZE * 4;
		readSize = SIZE * SIZE * 4;
		memset(map, FILL_BYTE, readSize);
		printf("image: %llu bytes; buffer %zu bytes mapped at %p\n",
			(unsigned long long)requirements.size, readSize, map);
	}

	// render pass: clear, draw, then the copy (or the host) reads it
	VkAttachmentDescription attachment = {
		.format = FORMAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = linear ? VK_IMAGE_LAYOUT_GENERAL
			: VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference colorReference = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colorReference,
	};
	VkSubpassDependency dependency = {
		.srcSubpass = 0,
		.dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = linear ? VK_PIPELINE_STAGE_HOST_BIT
			: VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = linear ? VK_ACCESS_HOST_READ_BIT
			: VK_ACCESS_TRANSFER_READ_BIT,
	};
	VkRenderPassCreateInfo renderPassInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency,
	};
	VkRenderPass renderPass;
	CHECK(vkCreateRenderPass(sDevice, &renderPassInfo, NULL, &renderPass));
	VkFramebufferCreateInfo framebufferInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = renderPass,
		.attachmentCount = 1,
		.pAttachments = &view,
		.width = SIZE,
		.height = SIZE,
		.layers = 1,
	};
	VkFramebuffer framebuffer;
	CHECK(vkCreateFramebuffer(sDevice, &framebufferInfo, NULL, &framebuffer));

	// the pipeline
	VkShaderModuleCreateInfo vertexInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(kVertexShader),
		.pCode = kVertexShader,
	};
	VkShaderModule vertexModule;
	CHECK(vkCreateShaderModule(sDevice, &vertexInfo, NULL, &vertexModule));
	VkShaderModuleCreateInfo fragmentInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(kFragmentShader),
		.pCode = kFragmentShader,
	};
	VkShaderModule fragmentModule;
	CHECK(vkCreateShaderModule(sDevice, &fragmentInfo, NULL,
		&fragmentModule));
	VkPipelineLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	};
	VkPipelineLayout pipelineLayout;
	CHECK(vkCreatePipelineLayout(sDevice, &layoutInfo, NULL, &pipelineLayout));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertexModule,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragmentModule,
			.pName = "main",
		},
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkViewport viewport = { 0.0f, 0.0f, SIZE, SIZE, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { SIZE, SIZE } };
	VkPipelineViewportStateCreateInfo viewportState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blendAttachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo colorBlend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blendAttachment,
	};
	VkGraphicsPipelineCreateInfo pipelineInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pColorBlendState = &colorBlend,
		.layout = pipelineLayout,
		.renderPass = renderPass,
		.subpass = 0,
	};
	VkPipeline pipeline;
	double start = now_ms();
	CHECK(vkCreateGraphicsPipelines(sDevice, VK_NULL_HANDLE, 1, &pipelineInfo,
		NULL, &pipeline));
	printf("pipeline: compiled in %.2f ms\n", now_ms() - start);

	// the command buffer
	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family,
	};
	VkCommandPool commandPool;
	CHECK(vkCreateCommandPool(sDevice, &poolInfo, NULL, &commandPool));
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
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
	VkClearValue clearValue;
	memcpy(clearValue.color.float32, kClear, sizeof(kClear));
	VkRenderPassBeginInfo renderPassBegin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = renderPass,
		.framebuffer = framebuffer,
		.renderArea = { { 0, 0 }, { SIZE, SIZE } },
		.clearValueCount = 1,
		.pClearValues = &clearValue,
	};
	vkCmdBeginRenderPass(commandBuffer, &renderPassBegin,
		VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdDraw(commandBuffer, 3, 1, 0, 0);
	vkCmdEndRenderPass(commandBuffer);
	if (!linear) {
		VkBufferImageCopy region = {
			.bufferOffset = 0,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { SIZE, SIZE, 1 },
		};
		vkCmdCopyImageToBuffer(commandBuffer, image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
		VkMemoryBarrier barrier = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		};
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	}
	CHECK(vkEndCommandBuffer(commandBuffer));

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commandBuffer,
	};
	start = now_ms();
	CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
	double submitted = now_ms();
	result = vkWaitForFences(sDevice, 1, &fence, VK_TRUE,
		timeoutMs * 1000000ull);
	double done = now_ms();
	printf("submit %.3f ms, fence %s after %.3f ms\n", submitted - start,
		result == VK_SUCCESS ? "signalled"
			: result == VK_TIMEOUT ? "TIMED OUT" : "FAILED",
		done - submitted);
	if (result != VK_SUCCESS) {
		printf("FAIL: vkWaitForFences: %d\n", (int)result);
		return 1;
	}

	start = now_ms();
	uint32_t wrong = check_pixels(pixels, pitch);
	printf("checked in %.3f ms\n", now_ms() - start);
	if (ppmPath != NULL)
		write_ppm(ppmPath, pixels, pitch);

	vkDeviceWaitIdle(sDevice);
	vkDestroyFence(sDevice, fence, NULL);
	vkDestroyCommandPool(sDevice, commandPool, NULL);
	vkDestroyPipeline(sDevice, pipeline, NULL);
	vkDestroyPipelineLayout(sDevice, pipelineLayout, NULL);
	vkDestroyShaderModule(sDevice, fragmentModule, NULL);
	vkDestroyShaderModule(sDevice, vertexModule, NULL);
	vkDestroyFramebuffer(sDevice, framebuffer, NULL);
	vkDestroyRenderPass(sDevice, renderPass, NULL);
	vkDestroyImageView(sDevice, view, NULL);
	vkDestroyImage(sDevice, image, NULL);
	vkFreeMemory(sDevice, imageMemory, NULL);
	if (buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(sDevice, buffer, NULL);
		vkFreeMemory(sDevice, bufferMemory, NULL);
	}
	vkDestroyDevice(sDevice, NULL);
	vkDestroyInstance(sInstance, NULL);

	printf("%s\n", wrong == 0 ? "PASS" : "FAIL");
	return wrong == 0 ? 0 : 1;
}
