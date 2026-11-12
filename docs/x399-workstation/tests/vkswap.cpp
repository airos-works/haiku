/* vkswap: a Vulkan swapchain in a Wayland (xdg-shell) window, the way vkcube
 * and other Vulkan programs present on Haiku: through the in-process Wayland
 * server (wayland-server-inproc.so) and Mesa's Wayland WSI.
 *
 * Each frame is cleared to a colour of its own. After vkQueuePresentKHR the
 * program looks into the wl_shm buffers the server mapped in this team (areas
 * named "wl_shm") for the frame it just presented: when the WSI copied the
 * image before the GPU had finished it, the buffer holds an older frame
 * ("stale") or parts of two ("torn"). It also reports the frame rate, the CPU
 * the program used per frame, and in FIFO mode where in the display's frame
 * each present landed (phase against the blank, from BScreen::WaitForRetrace):
 * presents paced by the display stay at one phase, presents paced by a timer
 * drift through the frame.
 *
 * usage: vkswap [seconds] [fifo|mailbox|immediate] [WxH]
 *   VKSWAP_DEVICE=n   the n-th physical device (default: the first GPU)
 *   VKSWAP_NOCHECK=1  skip the look into the buffers (it costs a little CPU)
 *   VKSWAP_RECREATE=n make the swapchain anew every n frames (oldSwapchain)
 *   VKSWAP_CLEANUP=1  destroy everything before exiting, instead of leaving
 *                     it to the team's end
 *
 * Build (cross, against a sysroot with wayland_devel and wayland_protocols):
 *   wayland-scanner client-header xdg-shell.xml xdg-shell-client-protocol.h
 *   wayland-scanner private-code xdg-shell.xml xdg-shell-protocol.c
 *   x86_64-unknown-haiku-g++ -O2 -I<mesa>/include -I. vkswap.cpp \
 *       xdg-shell-protocol.c -lwayland-client -lbe -o vkswap
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>
#include <Screen.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


static void
die(const char* what, int result = 0)
{
	fprintf(stderr, "vkswap: %s (%d)\n", what, result);
	exit(1);
}


#define CHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) die(#call, _r); } while (0)


// #pragma mark - Wayland


static struct wl_compositor* sCompositor;
static struct xdg_wm_base* sWmBase;
static bool sConfigured;


static void
wm_base_ping(void*, struct xdg_wm_base* base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener kWmBaseListener = { wm_base_ping };


static void
registry_global(void*, struct wl_registry* registry, uint32_t name,
	const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		sCompositor = (struct wl_compositor*)wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		sWmBase = (struct xdg_wm_base*)wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(sWmBase, &kWmBaseListener, NULL);
	}
}

static void
registry_global_remove(void*, struct wl_registry*, uint32_t)
{
}

static const struct wl_registry_listener kRegistryListener
	= { registry_global, registry_global_remove };


static void
xdg_surface_configure(void*, struct xdg_surface* surface, uint32_t serial)
{
	xdg_surface_ack_configure(surface, serial);
	sConfigured = true;
}

static const struct xdg_surface_listener kXdgSurfaceListener
	= { xdg_surface_configure };


static void
toplevel_configure(void*, struct xdg_toplevel*, int32_t, int32_t,
	struct wl_array*)
{
}

static void
toplevel_close(void*, struct xdg_toplevel*)
{
}

static const struct xdg_toplevel_listener kToplevelListener
	= { toplevel_configure, toplevel_close };


// #pragma mark - the display's blanks


static volatile bigtime_t sLastBlank;
static volatile bigtime_t sBlankPeriod;
static volatile bool sQuit;


static status_t
blank_thread(void*)
{
	BScreen screen(B_MAIN_SCREEN_ID);
	bigtime_t previous = 0;
	while (!sQuit) {
		if (screen.WaitForRetrace(100000) != B_OK)
			continue;
		bigtime_t now = system_time();
		if (previous != 0) {
			bigtime_t period = now - previous;
			if (period > 5000 && period < 50000)
				sBlankPeriod = sBlankPeriod == 0 ? period
					: (sBlankPeriod * 15 + period) / 16;
		}
		previous = now;
		sLastBlank = now;
	}
	return B_OK;
}


// #pragma mark - what the server holds


struct Color { uint8_t b, g, r, a; };


static Color
frame_color(uint64_t frame)
{
	// distinct for each of 256 frames in a row, never black or white
	Color color;
	color.r = (uint8_t)(16 + (frame * 37) % 224);
	color.g = (uint8_t)(16 + (frame * 101) % 224);
	color.b = (uint8_t)(16 + (frame * 13) % 224);
	color.a = 255;
	return color;
}


static bool
same(const uint8_t* pixel, Color color)
{
	return pixel[0] == color.b && pixel[1] == color.g && pixel[2] == color.r;
}


enum Look { kFresh, kStale, kTorn, kNotFound };


/* Whether one of the server's wl_shm buffers holds frame in full: sampled at
 * the top, middle and bottom rows. */
static Look
look_for(uint64_t frame, uint32_t width, uint32_t height)
{
	const Color expected = frame_color(frame);
	const uint32_t stride = width * 4;
	const uint32_t rows[3] = { 1, height / 2, height - 2 };
	bool partial = false;
	bool any = false;

	ssize_t cookie = 0;
	area_info info;
	while (get_next_area_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strcmp(info.name, "wl_shm") != 0
			|| info.size < (size_t)stride * height)
			continue;
		any = true;
		const uint8_t* base = (const uint8_t*)info.address;
		int matches = 0;
		for (int i = 0; i < 3; i++) {
			if (same(base + (size_t)rows[i] * stride + (width / 2) * 4,
					expected))
				matches++;
		}
		if (matches == 3)
			return kFresh;
		if (matches > 0)
			partial = true;
	}
	if (!any)
		return kNotFound;
	return partial ? kTorn : kStale;
}


// #pragma mark -


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 5;
	const char* modeName = argc > 2 ? argv[2] : "fifo";
	uint32_t width = 1280, height = 720;
	if (argc > 3)
		sscanf(argv[3], "%ux%u", &width, &height);
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	if (strcmp(modeName, "mailbox") == 0)
		mode = VK_PRESENT_MODE_MAILBOX_KHR;
	else if (strcmp(modeName, "immediate") == 0)
		mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	const bool check = getenv("VKSWAP_NOCHECK") == NULL;

	// the window
	struct wl_display* display = wl_display_connect(NULL);
	if (display == NULL)
		die("wl_display_connect");
	struct wl_registry* registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &kRegistryListener, NULL);
	wl_display_roundtrip(display);
	if (sCompositor == NULL || sWmBase == NULL)
		die("no wl_compositor or xdg_wm_base");
	struct wl_surface* surface = wl_compositor_create_surface(sCompositor);
	struct xdg_surface* xdgSurface
		= xdg_wm_base_get_xdg_surface(sWmBase, surface);
	xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, NULL);
	struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, NULL);
	xdg_toplevel_set_title(toplevel, "vkswap");
	wl_surface_commit(surface);
	while (!sConfigured)
		wl_display_dispatch(display);

	// Vulkan, through the loader
	void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
	if (loader == NULL)
		die("no libvulkan.so.1");
	PFN_vkGetInstanceProcAddr getInstanceProcAddr
		= (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
#define GLOBAL(name) PFN_##name name = (PFN_##name)getInstanceProcAddr(NULL, #name)
	GLOBAL(vkCreateInstance);
#define INSTANCE(name) PFN_##name name = (PFN_##name)getInstanceProcAddr(instance, #name)

	const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "vkswap";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &app;
	instanceInfo.enabledExtensionCount = 2;
	instanceInfo.ppEnabledExtensionNames = instanceExtensions;
	VkInstance instance;
	CHECK(vkCreateInstance(&instanceInfo, NULL, &instance));

	INSTANCE(vkEnumeratePhysicalDevices);
	INSTANCE(vkGetPhysicalDeviceProperties);
	INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
	INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
	INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
	INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
	INSTANCE(vkGetPhysicalDeviceSurfacePresentModesKHR);
	INSTANCE(vkCreateWaylandSurfaceKHR);
	INSTANCE(vkCreateDevice);
	INSTANCE(vkGetDeviceProcAddr);

	VkWaylandSurfaceCreateInfoKHR surfaceInfo
		= { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
	surfaceInfo.display = display;
	surfaceInfo.surface = surface;
	VkSurfaceKHR vkSurface;
	CHECK(vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, NULL, &vkSurface));

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, NULL);
	if (count == 0)
		die("no Vulkan device");
	VkPhysicalDevice devices[8];
	if (count > 8)
		count = 8;
	vkEnumeratePhysicalDevices(instance, &count, devices);
	uint32_t pick = 0;
	if (getenv("VKSWAP_DEVICE") != NULL)
		pick = atoi(getenv("VKSWAP_DEVICE")) % count;
	else {
		for (uint32_t i = 0; i < count; i++) {
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devices[i], &props);
			if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
				pick = i;
				break;
			}
		}
	}
	VkPhysicalDevice physical = devices[pick];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical, &props);
	printf("device: %s\n", props.deviceName);

	uint32_t families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
	VkQueueFamilyProperties familyProps[16];
	if (families > 16)
		families = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, familyProps);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < families; i++) {
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, vkSurface, &present);
		if ((familyProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
			family = i;
			break;
		}
	}
	if (family == UINT32_MAX)
		die("no queue family that can present");

	float priority = 1;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = family;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = 1;
	deviceInfo.ppEnabledExtensionNames = deviceExtensions;
	VkDevice device;
	CHECK(vkCreateDevice(physical, &deviceInfo, NULL, &device));

#define DEVICE(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
	DEVICE(vkGetDeviceQueue);
	DEVICE(vkCreateSwapchainKHR);
	DEVICE(vkGetSwapchainImagesKHR);
	DEVICE(vkAcquireNextImageKHR);
	DEVICE(vkQueuePresentKHR);
	DEVICE(vkQueueSubmit);
	DEVICE(vkCreateCommandPool);
	DEVICE(vkAllocateCommandBuffers);
	DEVICE(vkBeginCommandBuffer);
	DEVICE(vkEndCommandBuffer);
	DEVICE(vkResetCommandBuffer);
	DEVICE(vkCmdPipelineBarrier);
	DEVICE(vkCmdClearColorImage);
	DEVICE(vkCreateSemaphore);
	DEVICE(vkCreateFence);
	DEVICE(vkWaitForFences);
	DEVICE(vkResetFences);
	DEVICE(vkDeviceWaitIdle);
	DEVICE(vkDestroySwapchainKHR);
	DEVICE(vkDestroySemaphore);
	DEVICE(vkDestroyFence);
	DEVICE(vkDestroyCommandPool);
	DEVICE(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);

	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, vkSurface, &caps));
	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, vkSurface, &formatCount, NULL);
	VkSurfaceFormatKHR formats[64];
	if (formatCount > 64)
		formatCount = 64;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, vkSurface, &formatCount, formats);
	VkSurfaceFormatKHR format = formats[0];
	for (uint32_t i = 0; i < formatCount; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
			format = formats[i];
	}
	uint32_t modeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, vkSurface, &modeCount, NULL);
	VkPresentModeKHR modes[16];
	if (modeCount > 16)
		modeCount = 16;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, vkSurface, &modeCount, modes);
	bool haveMode = false;
	for (uint32_t i = 0; i < modeCount; i++)
		haveMode = haveMode || modes[i] == mode;
	if (!haveMode) {
		printf("present mode %s not offered; FIFO instead\n", modeName);
		mode = VK_PRESENT_MODE_FIFO_KHR;
		modeName = "fifo";
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount != 0 && imageCount > caps.maxImageCount)
		imageCount = caps.maxImageCount;
	VkSwapchainCreateInfoKHR swapInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	swapInfo.surface = vkSurface;
	swapInfo.minImageCount = imageCount;
	swapInfo.imageFormat = format.format;
	swapInfo.imageColorSpace = format.colorSpace;
	swapInfo.imageExtent.width = width;
	swapInfo.imageExtent.height = height;
	swapInfo.imageArrayLayers = 1;
	swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
		| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapInfo.presentMode = mode;
	swapInfo.clipped = VK_TRUE;
	VkSwapchainKHR swapchain;
	CHECK(vkCreateSwapchainKHR(device, &swapInfo, NULL, &swapchain));
	uint32_t count2 = 0;
	vkGetSwapchainImagesKHR(device, swapchain, &count2, NULL);
	VkImage images[16];
	if (count2 > 16)
		count2 = 16;
	vkGetSwapchainImagesKHR(device, swapchain, &count2, images);
	printf("swapchain: %u images, %ux%u, %s, format %d\n", count2, width,
		height, modeName, format.format);
	const uint64_t recreateEvery = getenv("VKSWAP_RECREATE") != NULL
		? strtoull(getenv("VKSWAP_RECREATE"), NULL, 10) : 0;
	uint64_t recreated = 0;

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = family;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &poolInfo, NULL, &pool));
	const uint32_t kSlots = 3;
	VkCommandBuffer cmd[kSlots];
	VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.commandPool = pool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = kSlots;
	CHECK(vkAllocateCommandBuffers(device, &allocInfo, cmd));
	VkSemaphore acquired[kSlots], rendered[kSlots];
	VkFence fences[kSlots];
	for (uint32_t i = 0; i < kSlots; i++) {
		VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		CHECK(vkCreateSemaphore(device, &semInfo, NULL, &acquired[i]));
		CHECK(vkCreateSemaphore(device, &semInfo, NULL, &rendered[i]));
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CHECK(vkCreateFence(device, &fenceInfo, NULL, &fences[i]));
	}

	thread_id blanks = spawn_thread(blank_thread, "blanks", B_REAL_TIME_PRIORITY, NULL);
	resume_thread(blanks);
	snooze(200000);

	uint64_t frames = 0, stale = 0, torn = 0, notFound = 0;
	double phaseSum = 0, phaseSum2 = 0, phaseMin = 1, phaseMax = 0;
	uint64_t phases = 0;
	bigtime_t intervalMin = INT64_MAX, intervalMax = 0, previousPresent = 0;
	uint64_t longIntervals = 0;

	thread_info threadInfo;
	get_thread_info(find_thread(NULL), &threadInfo);
	const bigtime_t threadCpu0 = threadInfo.user_time + threadInfo.kernel_time;
	team_usage_info teamUsage;
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &teamUsage);
	const bigtime_t teamCpu0 = teamUsage.user_time + teamUsage.kernel_time;
	const bigtime_t start = system_time();
	const bigtime_t end = start + (bigtime_t)(seconds * 1000000);

	while (system_time() < end) {
		if (recreateEvery != 0 && frames != 0 && frames % recreateEvery == 0) {
			// a new swapchain from the old one, as on a window resize
			vkDeviceWaitIdle(device);
			swapInfo.oldSwapchain = swapchain;
			VkSwapchainKHR newChain;
			CHECK(vkCreateSwapchainKHR(device, &swapInfo, NULL, &newChain));
			vkDestroySwapchainKHR(device, swapchain, NULL);
			swapchain = newChain;
			swapInfo.oldSwapchain = VK_NULL_HANDLE;
			count2 = 16;
			vkGetSwapchainImagesKHR(device, swapchain, &count2, images);
			recreated++;
		}
		const uint32_t slot = frames % kSlots;
		CHECK(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX));
		CHECK(vkResetFences(device, 1, &fences[slot]));
		uint32_t index;
		VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
			acquired[slot], VK_NULL_HANDLE, &index);
		if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
			die("vkAcquireNextImageKHR", acquire);

		vkResetCommandBuffer(cmd[slot], 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		CHECK(vkBeginCommandBuffer(cmd[slot], &begin));
		VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = images[index];
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		const Color color = frame_color(frames);
		VkClearColorValue clear;
		clear.float32[0] = color.r / 255.0f;
		clear.float32[1] = color.g / 255.0f;
		clear.float32[2] = color.b / 255.0f;
		clear.float32[3] = 1;
		vkCmdClearColorImage(cmd[slot], images[index],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
			&barrier.subresourceRange);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = 0;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
			&barrier);
		CHECK(vkEndCommandBuffer(cmd[slot]));

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &acquired[slot];
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd[slot];
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &rendered[slot];
		CHECK(vkQueueSubmit(queue, 1, &submit, fences[slot]));

		VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &rendered[slot];
		present.swapchainCount = 1;
		present.pSwapchains = &swapchain;
		present.pImageIndices = &index;
		VkResult presented = vkQueuePresentKHR(queue, &present);
		if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
			die("vkQueuePresentKHR", presented);

		const bigtime_t now = system_time();
		if (previousPresent != 0 && frames > 2) {
			bigtime_t interval = now - previousPresent;
			if (interval < intervalMin)
				intervalMin = interval;
			if (interval > intervalMax)
				intervalMax = interval;
			if (sBlankPeriod != 0 && interval > sBlankPeriod * 3 / 2)
				longIntervals++;
		}
		previousPresent = now;
		if (sBlankPeriod != 0 && sLastBlank != 0 && frames > 2) {
			double phase = fmod((double)(now - sLastBlank), (double)sBlankPeriod)
				/ sBlankPeriod;
			phaseSum += phase;
			phaseSum2 += phase * phase;
			if (phase < phaseMin)
				phaseMin = phase;
			if (phase > phaseMax)
				phaseMax = phase;
			phases++;
		}

		if (check) {
			switch (look_for(frames, width, height)) {
				case kFresh: break;
				case kStale: stale++; break;
				case kTorn: torn++; break;
				case kNotFound: notFound++; break;
			}
		}
		frames++;
	}
	const double elapsed = (system_time() - start) / 1e6;
	vkDeviceWaitIdle(device);
	sQuit = true;

	get_thread_info(find_thread(NULL), &threadInfo);
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &teamUsage);
	const bigtime_t threadCpu = threadInfo.user_time + threadInfo.kernel_time
		- threadCpu0;
	const bigtime_t teamCpu = teamUsage.user_time + teamUsage.kernel_time
		- teamCpu0;
	printf("%" B_PRIu64 " frames in %.1f s: %.1f fps (%s, %ux%u)\n", frames,
		elapsed, frames / elapsed, modeName, width, height);
	printf("CPU: presenting thread %.0f us/frame, whole team %.0f us/frame\n",
		(double)threadCpu / frames, (double)teamCpu / frames);
	if (check) {
		printf("buffers: %" B_PRIu64 " fresh, %" B_PRIu64 " stale, %" B_PRIu64
			" torn, %" B_PRIu64 " not found\n",
			frames - stale - torn - notFound, stale, torn, notFound);
	}
	if (recreated > 0)
		printf("swapchain made anew %" B_PRIu64 " times\n", recreated);
	if (phases > 0) {
		double mean = phaseSum / phases;
		double deviation = sqrt(fmax(0, phaseSum2 / phases - mean * mean));
		printf("present phase in the display's frame (period %.0f us): mean "
			"%.2f, deviation %.2f, range %.2f-%.2f; intervals %" B_PRIdBIGTIME
			"-%" B_PRIdBIGTIME " us, %" B_PRIu64 " over 1.5 periods\n",
			(double)sBlankPeriod, mean, deviation, phaseMin, phaseMax,
			intervalMin, intervalMax, longIntervals);
	}

	if (getenv("VKSWAP_CLEANUP") != NULL) {
		INSTANCE(vkDestroySurfaceKHR);
		INSTANCE(vkDestroyInstance);
		vkDestroySwapchainKHR(device, swapchain, NULL);
		for (uint32_t i = 0; i < kSlots; i++) {
			vkDestroySemaphore(device, acquired[i], NULL);
			vkDestroySemaphore(device, rendered[i], NULL);
			vkDestroyFence(device, fences[i], NULL);
		}
		vkDestroyCommandPool(device, pool, NULL);
		vkDestroyDevice(device, NULL);
		vkDestroySurfaceKHR(instance, vkSurface, NULL);
		vkDestroyInstance(instance, NULL);
		xdg_toplevel_destroy(toplevel);
		xdg_surface_destroy(xdgSurface);
		wl_surface_destroy(surface);
		wl_display_roundtrip(display);
		wl_display_disconnect(display);
		printf("cleaned up\n");
	}
	_exit(0);
}
