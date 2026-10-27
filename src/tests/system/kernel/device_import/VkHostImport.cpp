// vkhostimport [sleep|exit|exec|free] [MiB] [area]: summit#119. A team that
// ended (killed, or exited or exec'd without freeing) while a GPU driver had
// its memory locked took the machine down: the address space went before the
// team's files were closed, and only closing them unlocks the memory.
// Not built by jam (the tree has no Vulkan headers): cross-build against
// Mesa's include/ and libbe/libroot, run with VK_ICD_FILENAMES naming NVK.
//
// vkhostimport [sleep|exit|exec|free] [MiB] [area]: imports host memory into the
// NVIDIA GPU (VK_EXT_external_memory_host, NVK -> nvidia_rm
// os_lock_user_pages), then sleeps (to be killed), exits without freeing it,
// or frees it and exits, or execs a shell (exec). "area": the memory is a shared area (as app_server's
// screen copy cloned into a web process), else anonymous private memory.
#include <OS.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define FN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)

int
main(int argc, char** argv)
{
	const char* mode = argc > 1 ? argv[1] : "sleep";
	size_t size = (argc > 2 ? atoi(argv[2]) : 16) * 1024 * 1024;
	bool useArea = argc > 3 && strcmp(argv[3], "area") == 0;
	void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (lib == NULL) {
		printf("no libvulkan: %s\n", dlerror());
		return 1;
	}
	PFN_vkGetInstanceProcAddr gipa
		= (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkInstance instance = VK_NULL_HANDLE;
	FN(vkCreateInstance);
	VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	appInfo.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &appInfo;
	VkResult r = vkCreateInstance(&ici, NULL, &instance);
	if (r != VK_SUCCESS) {
		printf("vkCreateInstance %d\n", r);
		return 1;
	}
	FN(vkEnumeratePhysicalDevices);
	FN(vkGetPhysicalDeviceProperties2);
	FN(vkCreateDevice);
	FN(vkGetDeviceProcAddr);
	uint32_t count = 8;
	VkPhysicalDevice devices[8];
	vkEnumeratePhysicalDevices(instance, &count, devices);
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps
		= {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties2 props
			= {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		props.pNext = &hostProps;
		vkGetPhysicalDeviceProperties2(devices[i], &props);
		printf("device %u: %s vendor %#x\n", i, props.properties.deviceName,
			props.properties.vendorID);
		if (props.properties.vendorID == 0x10de)
			pd = devices[i];
	}
	if (pd == VK_NULL_HANDLE) {
		printf("no NVIDIA device\n");
		return 1;
	}
	float priority = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	const char* extensions[] = {"VK_EXT_external_memory_host"};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = extensions;
	VkDevice device;
	r = vkCreateDevice(pd, &dci, NULL, &device);
	if (r != VK_SUCCESS) {
		printf("vkCreateDevice %d\n", r);
		return 1;
	}
	PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps
		= (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device,
			"vkGetMemoryHostPointerPropertiesEXT");
	PFN_vkAllocateMemory allocate
		= (PFN_vkAllocateMemory)vkGetDeviceProcAddr(device, "vkAllocateMemory");
	PFN_vkFreeMemory freeMemory
		= (PFN_vkFreeMemory)vkGetDeviceProcAddr(device, "vkFreeMemory");

	size_t alignment = hostProps.minImportedHostPointerAlignment;
	if (alignment < B_PAGE_SIZE)
		alignment = B_PAGE_SIZE;
	size = (size + alignment - 1) & ~(alignment - 1);
	void* memory = NULL;
	if (useArea) {
		area_id area = create_area("vkhostimport", &memory, B_ANY_ADDRESS,
			size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
		if (area < 0) {
			printf("create_area: %s\n", strerror(area));
			return 1;
		}
	} else {
		memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}
	memset(memory, 0x5a, size);
	VkMemoryHostPointerPropertiesEXT mhp
		= {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
	r = getHostProps(device,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, memory, &mhp);
	if (r != VK_SUCCESS || mhp.memoryTypeBits == 0) {
		printf("vkGetMemoryHostPointerPropertiesEXT %d bits %#x\n", r,
			mhp.memoryTypeBits);
		return 1;
	}
	VkImportMemoryHostPointerInfoEXT import
		= {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	import.pHostPointer = memory;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &import;
	mai.allocationSize = size;
	mai.memoryTypeIndex = __builtin_ctz(mhp.memoryTypeBits);
	VkDeviceMemory deviceMemory;
	r = allocate(device, &mai, NULL, &deviceMemory);
	if (r != VK_SUCCESS) {
		printf("vkAllocateMemory (import) %d\n", r);
		return 1;
	}
	printf("imported %zu bytes at %p (%s), team %d\n", size, memory,
		useArea ? "area" : "anonymous", (int)getpid());
	fflush(stdout);
	if (strcmp(mode, "free") == 0) {
		freeMemory(device, deviceMemory, NULL);
		printf("freed\n");
		return 0;
	}
	if (strcmp(mode, "exec") == 0) {
		printf("exec without freeing\n");
		fflush(stdout);
		execl("/bin/sh", "sh", "-c", "echo exec ok; sleep 1", (char*)NULL);
		return 1;
	}
	if (strcmp(mode, "exit") == 0) {
		printf("exit without freeing\n");
		fflush(stdout);
		_exit(0);
	}
	for (;;)
		snooze(1000000);
}
