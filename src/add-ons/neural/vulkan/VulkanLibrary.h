/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef VULKAN_LIBRARY_H
#define VULKAN_LIBRARY_H


#include <String.h>
#include <SupportDefs.h>

#include "VulkanApi.h"


//! A graphics processor Vulkan knows of, and what the back end needs of it.
struct VulkanPhysicalDevice {
	VkPhysicalDevice	handle;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceMemoryProperties memory;
	//! the queue family the work is given to
	uint32				queue_family;
	//! empty when the driver does not say
	char				driver_name[VK_MAX_DRIVER_NAME_SIZE];
	char				driver_info[VK_MAX_DRIVER_INFO_SIZE];
	uint32				driver_id;
	bool				has_memory_budget;
	//! the largest single allocation the driver makes
	uint64				max_allocation_size;
	//! the heap of the device's own memory
	uint32				local_heap;
	uint64				local_heap_size;
};


/*!	The Vulkan loader, an instance made with it and the graphics
	processors it has. All of it may be missing: Init() fails then and the
	back end has no devices.
*/
class VulkanLibrary {
public:
								VulkanLibrary();
								~VulkanLibrary();

			status_t			Init();

			VkInstance			Instance() const { return fInstance; }
			const VulkanInstanceFunctions& Functions() const
									{ return fFunctions; }
	//! VK_API_VERSION_1_0 or _1_1: what the instance was made for
			uint32				ApiVersion() const { return fApiVersion; }

			int32				CountDevices() const { return fDeviceCount; }
			const VulkanPhysicalDevice& DeviceAt(int32 index) const
									{ return fDevices[index]; }

	/*!	The driver library (ICD) the loader took for the device, as well
		as that can be told: the loader keeps it to itself. */
			BString				DriverPath(int32 index) const;

private:
			status_t			_CreateInstance();
			status_t			_FindDevices();
			bool				_InitDevice(VkPhysicalDevice handle,
									VulkanPhysicalDevice& device);
			void				_FindDriverImages();

			void*				fHandle;
			PFN_vkGetInstanceProcAddr fGetInstanceProcAddr;
			VkInstance			fInstance;
			uint32				fApiVersion;
			VulkanInstanceFunctions fFunctions;

			VulkanPhysicalDevice* fDevices;
			int32				fDeviceCount;

			BString				fDriverPaths;
				// one per line
};


#endif	// VULKAN_LIBRARY_H
