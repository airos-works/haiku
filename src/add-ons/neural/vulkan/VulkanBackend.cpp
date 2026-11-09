/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Neural Kit on graphics processors, by way of Vulkan compute
	shaders: any device with a Vulkan driver runs networks of floating
	point numbers, whoever made it. See VulkanPlan for what is run and
	shaders/ for how.
*/


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Locker.h>
#include <Message.h>

#include <add-ons/neural/NeuralBackend.h>

#include "VulkanDevice.h"
#include "VulkanLibrary.h"
#include "VulkanPlan.h"
#include "VulkanProgram.h"


class VulkanBackend : public BNeuralBackend {
public:
								VulkanBackend(image_id image);
	virtual						~VulkanBackend();

	virtual	const char*			Name() const;
	virtual	int32				CountDevices();
	virtual	status_t			GetDeviceInfo(int32 device,
									neural_device_info* info,
									BMessage* details);
	virtual	status_t			GetSupportedOperators(int32 device,
									const BNeuralGraph& graph,
									bool* supported);
	virtual	status_t			CreateProgram(int32 device,
									const BNeuralGraph& graph,
									const neural_partition& partition,
									uint32 flags,
									BNeuralProgram** _program);

private:
			bool				_Load();
			status_t			_GetDevice(int32 index,
									VulkanDevice** _device);

			BLocker				fLock;
			bool				fLoaded;
			VulkanLibrary*		fLibrary;
			//! opened when the first program is made for them
			VulkanDevice**		fDevices;
};


static const char*
vendor_name(uint32 id)
{
	switch (id) {
		case 0x1002:
			return "AMD";
		case 0x1010:
			return "Imagination";
		case 0x106b:
			return "Apple";
		case 0x10de:
			return "NVIDIA";
		case 0x13b5:
			return "ARM";
		case 0x14e4:
			return "Broadcom";
		case 0x5143:
			return "Qualcomm";
		case 0x8086:
			return "Intel";
		default:
			return "";
	}
}


//! Operators that only say how the same numbers are to be seen.
static bool
is_shape_operator(neural_operator_code code)
{
	return code == B_NEURAL_OP_RESHAPE || code == B_NEURAL_OP_SQUEEZE;
}


VulkanBackend::VulkanBackend(image_id image)
	:
	BNeuralBackend(image),
	fLock("vulkan neural back end"),
	fLoaded(false),
	fLibrary(NULL),
	fDevices(NULL)
{
}


VulkanBackend::~VulkanBackend()
{
	if (fLibrary != NULL && fDevices != NULL) {
		for (int32 i = 0; i < fLibrary->CountDevices(); i++)
			delete fDevices[i];
	}
	free(fDevices);
	delete fLibrary;
}


const char*
VulkanBackend::Name() const
{
	return "vulkan";
}


/*!	Opens the Vulkan loader and asks it for the graphics processors,
	once: drivers do not come and go, and neither do the cards. Without a
	loader, or without a driver for anything, there is nothing to say and
	nothing is said. */
bool
VulkanBackend::_Load()
{
	if (fLoaded)
		return fLibrary != NULL;
	fLoaded = true;

	VulkanLibrary* library = new(std::nothrow) VulkanLibrary();
	if (library == NULL)
		return false;
	if (library->Init() != B_OK || library->CountDevices() == 0) {
		delete library;
		return false;
	}

	fDevices = (VulkanDevice**)calloc(library->CountDevices(),
		sizeof(VulkanDevice*));
	if (fDevices == NULL) {
		delete library;
		return false;
	}

	fLibrary = library;
	return true;
}


int32
VulkanBackend::CountDevices()
{
	BAutolock locker(fLock);
	return _Load() ? fLibrary->CountDevices() : 0;
}


status_t
VulkanBackend::GetDeviceInfo(int32 index, neural_device_info* info,
	BMessage* details)
{
	BAutolock locker(fLock);
	if (!_Load() || index < 0 || index >= fLibrary->CountDevices())
		return B_BAD_INDEX;

	const VulkanPhysicalDevice& device = fLibrary->DeviceAt(index);
	const VkPhysicalDeviceProperties& properties = device.properties;

	// The number among the graphics processors is all there is to tell
	// two cards of a kind apart, and it stays while the cards do.
	snprintf(info->id, sizeof(info->id), "%" B_PRId32, index);
	strlcpy(info->name, properties.deviceName, sizeof(info->name));
	strlcpy(info->vendor, vendor_name(properties.vendorID),
		sizeof(info->vendor));
	info->kind = B_NEURAL_DEVICE_GPU;
	info->flags = B_NEURAL_DEVICE_FLOAT;
	info->priority = 50;

	if (details == NULL)
		return B_OK;

	details->AddString("driver", device.driver_name[0] != '\0'
		? device.driver_name : "Vulkan");

	BString version(device.driver_info);
	if (version.IsEmpty()) {
		// how most drivers pack it, as far as anyone can tell
		version.SetToFormat("%" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32,
			VK_API_VERSION_MAJOR(properties.driverVersion),
			VK_API_VERSION_MINOR(properties.driverVersion),
			VK_API_VERSION_PATCH(properties.driverVersion));
	}
	details->AddString("version", version);

	BString api;
	api.SetToFormat("Vulkan %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32,
		VK_API_VERSION_MAJOR(properties.apiVersion),
		VK_API_VERSION_MINOR(properties.apiVersion),
		VK_API_VERSION_PATCH(properties.apiVersion));
	details->AddString("api", api);

	details->AddString("type",
		properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
			? "discrete"
			: properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
				? "integrated" : "virtual");
	details->AddInt64("memory", device.local_heap_size);

	VulkanLimits limits;
	limits.SetTo(device);
	details->AddInt64("largest tensor", limits.max_range);

	BString path = fLibrary->DriverPath(index);
	if (!path.IsEmpty())
		details->AddString("icd", path);

	// what the programs of this team hold, once there were any
	if (fDevices[index] != NULL) {
		details->AddInt64("memory used", fDevices[index]->UsedMemory());
		details->AddInt64("memory available",
			fDevices[index]->AvailableMemory());
	}

	return B_OK;
}


status_t
VulkanBackend::GetSupportedOperators(int32 index, const BNeuralGraph& graph,
	bool* supported)
{
	VulkanLimits limits;
	{
		BAutolock locker(fLock);
		if (!_Load() || index < 0 || index >= fLibrary->CountDevices())
			return B_BAD_INDEX;
		limits.SetTo(fLibrary->DeviceAt(index));
	}

	// An operator is supported when a plan takes it: programs are made
	// of plans, so the two cannot disagree.
	VulkanPlan plan(graph, limits);
	if (plan.InitCheck() != B_OK)
		return plan.InitCheck();

	int32 count = graph.CountOperators();
	for (int32 i = 0; i < count; i++)
		supported[i] = plan.AddOperator(i) == B_OK;

	// The kit makes a program of every stretch of supported operators.
	// One of nothing but new shapes would have the numbers travel to the
	// device and back for what the CPU does by not doing anything.
	for (int32 first = 0; first < count;) {
		int32 end = first;
		bool onlyShapes = true;
		while (end < count && supported[end]) {
			onlyShapes &= is_shape_operator(graph.OperatorAt(end)->code);
			end++;
		}
		if (onlyShapes) {
			for (int32 i = first; i < end; i++)
				supported[i] = false;
		}
		first = end + 1;
	}
	return B_OK;
}


status_t
VulkanBackend::_GetDevice(int32 index, VulkanDevice** _device)
{
	BAutolock locker(fLock);
	if (!_Load() || index < 0 || index >= fLibrary->CountDevices())
		return B_BAD_INDEX;

	if (fDevices[index] == NULL) {
		VulkanDevice* device = new(std::nothrow) VulkanDevice(*fLibrary,
			index);
		if (device == NULL)
			return B_NO_MEMORY;

		status_t status = device->Init();
		if (status != B_OK) {
			delete device;
			// not B_NO_MEMORY, whatever it was: the kit takes that to
			// mean that nothing can run the network
			return status == B_NO_MEMORY ? B_ERROR : status;
		}
		fDevices[index] = device;
	}

	*_device = fDevices[index];
	return B_OK;
}


status_t
VulkanBackend::CreateProgram(int32 index, const BNeuralGraph& graph,
	const neural_partition& partition, uint32 flags,
	BNeuralProgram** _program)
{
	VulkanDevice* device;
	status_t status = _GetDevice(index, &device);
	if (status != B_OK)
		return status;
	if (device->IsLost())
		return B_NEURAL_DEVICE_LOST;

	// see GetSupportedOperators()
	bool onlyShapes = true;
	for (int32 i = 0; i < partition.operator_count; i++) {
		const neural_graph_operator* op
			= graph.OperatorAt(partition.operators[i]);
		onlyShapes &= op != NULL && is_shape_operator(op->code);
	}
	if (onlyShapes)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	VulkanPlan plan(graph, device->Limits());
	status = plan.InitCheck();
	for (int32 i = 0; status == B_OK && i < partition.operator_count; i++)
		status = plan.AddOperator(partition.operators[i]);
	if (status == B_OK)
		status = plan.Finish(partition);
	if (status != B_OK)
		return status;

	VulkanProgram* program = new(std::nothrow) VulkanProgram(device);
	if (program == NULL)
		return B_NO_MEMORY;

	status = program->Init(plan);
	if (status != B_OK) {
		delete program;
		return status;
	}

	*_program = program;
	return B_OK;
}


BNeuralBackend*
instantiate_neural_backend(image_id image)
{
	return new(std::nothrow) VulkanBackend(image);
}
