/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	libvulkan.so.1 for air/OS arm64, which has no Vulkan loader: the few
	entry points a program finds by name, forwarded to the one Vulkan driver
	there is, Mesa's PowerVR driver (libvulkan_powervr_mesa.so), through its
	loader interface (vk_icdGetInstanceProcAddr).

	Zink needs exactly this: it dlopen()s libvulkan.so.1, takes
	vkGetInstanceProcAddr and vkGetDeviceProcAddr from it and gets
	everything else through those. A program that starts the same way
	works too. This is not a loader: no layers, no other driver, no window
	system surfaces, and no prototypes beyond the global commands. The
	driver's handles are used as they are.

	vkGetDeviceProcAddr needs the driver's own vkGetDeviceProcAddr, which is
	an instance-level lookup; it is taken from the first instance seen (the
	same function serves every device). */


#include <stdint.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


#define EXPORT __attribute__((visibility("default")))

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

static PFN_vkGetDeviceProcAddr sDriverGetDeviceProcAddr;


EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t* apiVersion);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* layerName,
	uint32_t* count, VkExtensionProperties* properties);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* count,
	VkLayerProperties* properties);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* createInfo,
	const VkAllocationCallbacks* allocator, VkInstance* instance);


static void
remember_instance(VkInstance instance)
{
	if (instance == VK_NULL_HANDLE
		|| __atomic_load_n(&sDriverGetDeviceProcAddr, __ATOMIC_ACQUIRE) != NULL)
		return;
	PFN_vkGetDeviceProcAddr function = (PFN_vkGetDeviceProcAddr)
		vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
	if (function != NULL) {
		__atomic_store_n(&sDriverGetDeviceProcAddr, function,
			__ATOMIC_RELEASE);
	}
}


EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
	if (name == NULL)
		return NULL;
	remember_instance(instance);
	// the lookups and the global commands stay here, as with a loader: a
	// driver has no layers to enumerate (zink asks)
	if (strcmp(name, "vkGetInstanceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	if (strcmp(name, "vkGetDeviceProcAddr") == 0 && instance != VK_NULL_HANDLE)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	if (instance == VK_NULL_HANDLE) {
		if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
			return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
		if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
			return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
		if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
			return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
		if (strcmp(name, "vkCreateInstance") == 0)
			return (PFN_vkVoidFunction)vkCreateInstance;
	}
	return vk_icdGetInstanceProcAddr(instance, name);
}


EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name)
{
	PFN_vkGetDeviceProcAddr function
		= __atomic_load_n(&sDriverGetDeviceProcAddr, __ATOMIC_ACQUIRE);
	if (function == NULL || device == VK_NULL_HANDLE || name == NULL)
		return NULL;
	if (strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	return function(device, name);
}


// the global commands, for programs that call them directly


EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t* apiVersion)
{
	PFN_vkEnumerateInstanceVersion function = (PFN_vkEnumerateInstanceVersion)
		vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
	if (function == NULL) {
		*apiVersion = VK_API_VERSION_1_0;
		return VK_SUCCESS;
	}
	return function(apiVersion);
}


EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* layerName,
	uint32_t* count, VkExtensionProperties* properties)
{
	if (layerName != NULL)
		return VK_ERROR_LAYER_NOT_PRESENT;
	PFN_vkEnumerateInstanceExtensionProperties function
		= (PFN_vkEnumerateInstanceExtensionProperties)
			vk_icdGetInstanceProcAddr(NULL,
				"vkEnumerateInstanceExtensionProperties");
	if (function == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	return function(NULL, count, properties);
}


EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* count,
	VkLayerProperties* properties)
{
	(void)properties;
	*count = 0;
	return VK_SUCCESS;
}


EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* createInfo,
	const VkAllocationCallbacks* allocator, VkInstance* instance)
{
	if (createInfo->enabledLayerCount != 0)
		return VK_ERROR_LAYER_NOT_PRESENT;
	PFN_vkCreateInstance function = (PFN_vkCreateInstance)
		vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
	if (function == NULL)
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	VkResult result = function(createInfo, allocator, instance);
	if (result == VK_SUCCESS)
		remember_instance(*instance);
	return result;
}
