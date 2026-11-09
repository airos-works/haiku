/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <neural/NeuralRoster.h>

#include <new>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Handler.h>
#include <Looper.h>
#include <Message.h>
#include <MessageRunner.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <PathFinder.h>
#include <StringList.h>
#include <driver_settings.h>
#include <image.h>

#include <neural/NeuralModel.h>

#include <NeuralGraphData.h>
#include <NeuralInterpreter.h>
#include <NeuralRosterPrivate.h>
#include <PathMonitor.h>


namespace BPrivate {


const char* const kNeuralCPUDeviceID = "cpu";

//! Where the drivers of neural processors publish their devices.
static const char* const kDeviceDirectory = "/dev/npu";

static const uint32 kMsgDevicesChanged = 'NRch';

static pthread_once_t sRosterOnce = PTHREAD_ONCE_INIT;
static NeuralRosterData* sRosterData;


/*!	Looks for devices again when something changes under /dev/npu, a
	moment later: a driver publishes several nodes in a row.
*/
class NeuralWatchHandler : public BHandler {
public:
	NeuralWatchHandler()
		:
		BHandler("neural devices"),
		fPending(false)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_PATH_MONITOR:
				if (!fPending) {
					fPending = true;
					BMessage changed(kMsgDevicesChanged);
					BMessageRunner::StartSending(this, &changed, 500000, 1);
				}
				break;

			case kMsgDevicesChanged:
				fPending = false;
				NeuralRosterData::Default()->Rescan();
				break;

			default:
				BHandler::MessageReceived(message);
				break;
		}
	}

private:
	bool	fPending;
};


// #pragma mark - NeuralRosterData


NeuralRosterData::NeuralRosterData()
	:
	fLock("neural roster"),
	fLoaded(false),
	fBackends(4),
	fDevices(4),
	fWatchers(4),
	fWatchLooper(NULL)
{
}


/*static*/ void
NeuralRosterData::_Create()
{
	sRosterData = new NeuralRosterData();
}


/*static*/ NeuralRosterData*
NeuralRosterData::Default()
{
	pthread_once(&sRosterOnce, &_Create);

	BAutolock locker(sRosterData->fLock);
	if (!sRosterData->fLoaded) {
		sRosterData->fLoaded = true;
		sRosterData->_ReadSettings();
		sRosterData->_LoadBackends();
		sRosterData->_CollectDevices(sRosterData->fDevices);
	}
	return sRosterData;
}


/*static*/ void
NeuralRosterData::InitDevice(BNeuralDevice& device, const char* id,
	const char* name, const char* vendor, const char* backend,
	neural_device_kind kind, uint32 flags)
{
	device.fID = id;
	device.fName = name;
	device.fVendor = vendor;
	device.fBackend = backend;
	device.fKind = kind;
	device.fFlags = flags;
}


/*!	The settings file "neural" (driver settings syntax) and the
	environment: which device to prefer, which back ends to leave alone.

		preferred_device rk3588_npu/0
		disable_backend vulkan
*/
void
NeuralRosterData::_ReadSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("neural") == B_OK) {
		void* settings = load_driver_settings(path.Path());
		if (settings != NULL) {
			const char* device = get_driver_parameter(settings,
				"preferred_device", NULL, NULL);
			if (device != NULL)
				fPreferredDevice = device;

			const driver_settings* all = get_driver_settings(settings);
			for (int32 i = 0; all != NULL && i < all->parameter_count; i++) {
				const driver_parameter& parameter = all->parameters[i];
				if (strcmp(parameter.name, "disable_backend") == 0
					&& parameter.value_count > 0) {
					fDisabledBackends << " " << parameter.values[0] << " ";
				}
			}
			unload_driver_settings(settings);
		}
	}

	// for tests and for trying things out
	const char* device = getenv("NEURAL_DEVICE");
	if (device != NULL && device[0] != '\0')
		fPreferredDevice = device;
	const char* disabled = getenv("NEURAL_DISABLE_BACKENDS");
	if (disabled != NULL)
		fDisabledBackends << " " << disabled << " ";
}


void
NeuralRosterData::_LoadBackends()
{
	// The first add-on of a name wins: non-packaged before packaged, the
	// user's before the system's.
	BStringList directories;
	if (BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY, "neural",
			B_FIND_PATH_EXISTING_ONLY, directories) != B_OK) {
		return;
	}

	BStringList seen;
	for (int32 i = 0; i < directories.CountStrings(); i++) {
		BDirectory directory(directories.StringAt(i));
		BEntry entry;
		while (directory.GetNextEntry(&entry, true) == B_OK) {
			BPath path;
			if (!entry.IsFile() || entry.GetPath(&path) != B_OK)
				continue;
			if (seen.HasString(path.Leaf()))
				continue;
			seen.Add(path.Leaf());

			// "disable_backend vulkan" keeps vulkan.neural from being
			// loaded at all: it may be the reason for the setting.
			BString name(path.Leaf());
			if (name.EndsWith(".neural"))
				name.Truncate(name.Length() - 7);
			BString pattern;
			pattern << " " << name << " ";
			if (fDisabledBackends.FindFirst(pattern) >= 0
				|| fDisabledBackends.FindFirst(" all ") >= 0) {
				continue;
			}

			_LoadBackend(path.Path());
		}
	}
}


void
NeuralRosterData::_LoadBackend(const char* path)
{
	image_id image = load_add_on(path);
	if (image < 0)
		return;

	BNeuralBackend* (*instantiate)(image_id);
	if (get_image_symbol(image, "instantiate_neural_backend",
			B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK) {
		unload_add_on(image);
		return;
	}

	BNeuralBackend* backend = instantiate(image);
	if (backend == NULL) {
		unload_add_on(image);
		return;
	}

	Backend* entry = new(std::nothrow) Backend;
	if (entry == NULL || !fBackends.AddItem(entry)) {
		delete entry;
		delete backend;
		unload_add_on(image);
		return;
	}

	entry->name = backend->Name();
	entry->path = path;
	entry->image = image;
	entry->backend = backend;
}


/*!	Asks every back end for its devices. The CPU comes first. */
void
NeuralRosterData::_CollectDevices(BObjectList<Device, true>& devices)
{
	Device* cpu = new(std::nothrow) Device;
	if (cpu != NULL) {
		InitDevice(cpu->device, kNeuralCPUDeviceID, "CPU", "", "cpu",
			B_NEURAL_DEVICE_CPU, B_NEURAL_DEVICE_QUANTIZED
				| B_NEURAL_DEVICE_FLOAT | B_NEURAL_DEVICE_REFERENCE);
		cpu->backend = NULL;
		cpu->index = 0;
		cpu->priority = 0;
		if (!devices.AddItem(cpu))
			delete cpu;
	}

	for (int32 i = 0; i < fBackends.CountItems(); i++) {
		Backend* backend = fBackends.ItemAt(i);
		int32 count = backend->backend->CountDevices();
		for (int32 j = 0; j < count; j++) {
			neural_device_info info;
			memset(&info, 0, sizeof(info));
			if (backend->backend->GetDeviceInfo(j, &info, NULL) != B_OK)
				continue;
			info.id[sizeof(info.id) - 1] = '\0';
			info.name[sizeof(info.name) - 1] = '\0';
			info.vendor[sizeof(info.vendor) - 1] = '\0';

			Device* device = new(std::nothrow) Device;
			if (device == NULL)
				return;

			BString id;
			id << backend->name << "/" << info.id;
			InitDevice(device->device, id, info.name, info.vendor,
				backend->name, info.kind, info.flags);
			device->backend = backend;
			device->index = j;
			device->priority = info.priority;
			if (!devices.AddItem(device))
				delete device;
		}
	}
}


NeuralRosterData::Device*
NeuralRosterData::_FindDevice(const char* id)
{
	if (id == NULL)
		return NULL;

	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		if (strcmp(fDevices.ItemAt(i)->device.ID(), id) == 0)
			return fDevices.ItemAt(i);
	}
	return NULL;
}


void
NeuralRosterData::_Notify(uint32 what, const Device& device)
{
	BMessage message(what);
	message.AddString("device", device.device.ID());
	message.AddString("name", device.device.Name());
	message.AddInt32("kind", device.device.Kind());

	for (int32 i = fWatchers.CountItems() - 1; i >= 0; i--) {
		BMessenger* watcher = fWatchers.ItemAt(i);
		if (!watcher->IsValid()) {
			delete fWatchers.RemoveItemAt(i);
			continue;
		}
		watcher->SendMessage(&message, (BHandler*)NULL, 0);
	}
}


status_t
NeuralRosterData::Rescan()
{
	BAutolock locker(fLock);

	BObjectList<Device, true> devices(4);
	_CollectDevices(devices);

	// tell the watchers what went and what came
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		const Device* old = fDevices.ItemAt(i);
		bool found = false;
		for (int32 j = 0; !found && j < devices.CountItems(); j++)
			found = devices.ItemAt(j)->device == old->device;
		if (!found)
			_Notify(B_NEURAL_DEVICE_REMOVED, *old);
	}

	for (int32 j = 0; j < devices.CountItems(); j++) {
		if (_FindDevice(devices.ItemAt(j)->device.ID()) == NULL)
			_Notify(B_NEURAL_DEVICE_ADDED, *devices.ItemAt(j));
	}

	fDevices.MakeEmpty();
	while (devices.CountItems() > 0)
		fDevices.AddItem(devices.RemoveItemAt(0));

	return B_OK;
}


int32
NeuralRosterData::CountDevices()
{
	BAutolock locker(fLock);
	return fDevices.CountItems();
}


status_t
NeuralRosterData::GetDeviceAt(int32 index, BNeuralDevice* device)
{
	BAutolock locker(fLock);

	Device* entry = fDevices.ItemAt(index);
	if (entry == NULL)
		return B_BAD_INDEX;

	*device = entry->device;
	return B_OK;
}


status_t
NeuralRosterData::FindDevice(const char* id, BNeuralDevice* device)
{
	BAutolock locker(fLock);

	Device* entry = _FindDevice(id);
	if (entry == NULL)
		return B_NAME_NOT_FOUND;

	*device = entry->device;
	return B_OK;
}


bool
NeuralRosterData::IsAvailable(const char* id)
{
	BAutolock locker(fLock);
	return _FindDevice(id) != NULL;
}


status_t
NeuralRosterData::GetBackend(const char* id, BNeuralBackend** _backend,
	int32* _index)
{
	BAutolock locker(fLock);

	Device* entry = _FindDevice(id);
	if (entry == NULL)
		return B_NEURAL_NO_DEVICE;

	*_backend = entry->backend != NULL ? entry->backend->backend : NULL;
	*_index = entry->index;
	return B_OK;
}


status_t
NeuralRosterData::GetDeviceDetails(const char* id, BMessage* details)
{
	BNeuralBackend* backend;
	int32 index;
	status_t status = GetBackend(id, &backend, &index);
	if (status != B_OK)
		return status;

	details->MakeEmpty();
	if (backend == NULL) {
		system_info info;
		get_system_info(&info);
		details->AddInt32("cores", info.cpu_count);
		details->AddInt32("threads",
			NeuralThreadPool::Default()->CountThreads());
		details->AddString("driver", "Neural Kit");
		return B_OK;
	}

	neural_device_info info;
	memset(&info, 0, sizeof(info));
	return backend->GetDeviceInfo(index, &info, details);
}


int32
NeuralRosterData::GetSupportedOperators(const char* id,
	const NeuralGraphData* graph, bool* supported)
{
	BNeuralBackend* backend;
	int32 index;
	status_t status = GetBackend(id, &backend, &index);
	if (status != B_OK)
		return status;

	int32 count = graph->CountOperators();
	if (backend == NULL) {
		for (int32 i = 0; i < count; i++)
			supported[i] = NeuralInterpreter::Supports(graph, i);
	} else {
		memset(supported, 0, count * sizeof(bool));
		status = backend->GetSupportedOperators(index, graph->Public(),
			supported);
		if (status != B_OK)
			return status;
	}

	int32 total = 0;
	for (int32 i = 0; i < count; i++) {
		if (supported[i])
			total++;
	}
	return total;
}


struct DeviceOrder {
	uint32	flags;
};


static int
device_rank(const NeuralRosterData::Device* device, uint32 flags)
{
	// Lower is better. A neural processor is what the machine has for
	// this; a graphics processor is fast but takes power and is shared
	// with everything on screen.
	static const int kSpeedOrder[] = { 2, 1, 0 };
		// by neural_device_kind: CPU, GPU, NPU
	static const int kPowerOrder[] = { 1, 2, 0 };

	int kind = device->device.Kind();
	if (kind < 0 || kind > 2)
		kind = 0;
	const int* order = (flags & B_NEURAL_PREFER_LOW_POWER) != 0
		? kPowerOrder : kSpeedOrder;
	return order[kind] * 1000 - device->priority;
}


status_t
NeuralRosterData::GetDefaultDevice(BNeuralDevice* device, uint32 flags,
	const NeuralGraphData* graph)
{
	BAutolock locker(fLock);

	if ((flags & B_NEURAL_CPU_ONLY) != 0)
		return FindDevice(kNeuralCPUDeviceID, device);

	int32 opCount = graph != NULL ? graph->CountOperators() : 0;
	bool* supported = NULL;
	if (graph != NULL) {
		supported = (bool*)malloc(opCount > 0 ? opCount : 1);
		if (supported == NULL)
			return B_NO_MEMORY;
	}

	Device* best = NULL;
	int bestRank = 0;
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		Device* candidate = fDevices.ItemAt(i);

		if (graph != NULL) {
			int32 count = GetSupportedOperators(candidate->device.ID(),
				graph, supported);
			if (count <= 0)
				continue;
			if ((flags & B_NEURAL_NO_FALLBACK) != 0 && count < opCount)
				continue;
		}

		// what the user chose comes first when it can do the job
		if (fPreferredDevice.Length() > 0
			&& fPreferredDevice == candidate->device.ID()) {
			best = candidate;
			break;
		}

		int rank = device_rank(candidate, flags);
		if (best == NULL || rank < bestRank) {
			best = candidate;
			bestRank = rank;
		}
	}

	free(supported);

	if (best == NULL)
		return B_NEURAL_NO_DEVICE;

	*device = best->device;
	return B_OK;
}


status_t
NeuralRosterData::StartWatching(const BMessenger& target)
{
	if (!target.IsValid())
		return B_BAD_VALUE;

	BAutolock locker(fLock);

	for (int32 i = 0; i < fWatchers.CountItems(); i++) {
		if (*fWatchers.ItemAt(i) == target)
			return B_OK;
	}

	BMessenger* watcher = new(std::nothrow) BMessenger(target);
	if (watcher == NULL || !fWatchers.AddItem(watcher)) {
		delete watcher;
		return B_NO_MEMORY;
	}

	if (fWatchLooper == NULL) {
		BLooper* looper = new(std::nothrow) BLooper("neural roster");
		NeuralWatchHandler* handler = new(std::nothrow) NeuralWatchHandler();
		if (looper == NULL || handler == NULL) {
			delete looper;
			delete handler;
			return B_OK;
				// the watcher still hears of Rescan()s
		}

		looper->AddHandler(handler);
		if (looper->Run() < 0)
			return B_OK;
		fWatchLooper = looper;

		BPathMonitor::StartWatching(kDeviceDirectory,
			B_WATCH_FILES_ONLY | B_WATCH_RECURSIVELY, BMessenger(handler));
	}

	return B_OK;
}


status_t
NeuralRosterData::StopWatching(const BMessenger& target)
{
	BAutolock locker(fLock);

	for (int32 i = 0; i < fWatchers.CountItems(); i++) {
		if (*fWatchers.ItemAt(i) == target) {
			delete fWatchers.RemoveItemAt(i);
			return B_OK;
		}
	}
	return B_NAME_NOT_FOUND;
}


}	// namespace BPrivate


using BPrivate::NeuralRosterData;


// #pragma mark - BNeuralDevice


BNeuralDevice::BNeuralDevice()
	:
	fKind(B_NEURAL_DEVICE_CPU),
	fFlags(0)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralDevice::BNeuralDevice(const BNeuralDevice& other)
{
	memset(_reserved, 0, sizeof(_reserved));
	*this = other;
}


BNeuralDevice::~BNeuralDevice()
{
}


BNeuralDevice&
BNeuralDevice::operator=(const BNeuralDevice& other)
{
	fID = other.fID;
	fName = other.fName;
	fVendor = other.fVendor;
	fBackend = other.fBackend;
	fKind = other.fKind;
	fFlags = other.fFlags;
	return *this;
}


bool
BNeuralDevice::operator==(const BNeuralDevice& other) const
{
	return fID == other.fID;
}


bool
BNeuralDevice::operator!=(const BNeuralDevice& other) const
{
	return fID != other.fID;
}


bool
BNeuralDevice::IsValid() const
{
	return fID.Length() > 0;
}


bool
BNeuralDevice::IsAvailable() const
{
	return IsValid() && NeuralRosterData::Default()->IsAvailable(fID);
}


const char*
BNeuralDevice::ID() const
{
	return fID.String();
}


const char*
BNeuralDevice::Name() const
{
	return fName.String();
}


const char*
BNeuralDevice::Vendor() const
{
	return fVendor.String();
}


neural_device_kind
BNeuralDevice::Kind() const
{
	return fKind;
}


uint32
BNeuralDevice::Flags() const
{
	return fFlags;
}


const char*
BNeuralDevice::Backend() const
{
	return fBackend.String();
}


status_t
BNeuralDevice::GetInfo(BMessage* info) const
{
	if (info == NULL)
		return B_BAD_VALUE;
	if (!IsValid())
		return B_NO_INIT;
	return NeuralRosterData::Default()->GetDeviceDetails(fID, info);
}


int32
BNeuralDevice::CountSupportedOperators(const BNeuralModel& model) const
{
	if (!IsValid())
		return B_NO_INIT;
	if (model.InitCheck() != B_OK)
		return model.InitCheck();

	const BPrivate::NeuralGraphData* graph = model.fGraph;
	int32 count = graph->CountOperators();
	bool* supported = (bool*)malloc(count > 0 ? count : 1);
	if (supported == NULL)
		return B_NO_MEMORY;

	int32 result = NeuralRosterData::Default()->GetSupportedOperators(fID,
		graph, supported);
	free(supported);
	return result;
}


// #pragma mark - BNeuralRoster


BNeuralRoster::BNeuralRoster()
	:
	fData(NULL)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralRoster::~BNeuralRoster()
{
}


/*static*/ BNeuralRoster*
BNeuralRoster::Default()
{
	static BNeuralRoster sRoster;
	return &sRoster;
}


int32
BNeuralRoster::CountDevices()
{
	return NeuralRosterData::Default()->CountDevices();
}


status_t
BNeuralRoster::GetDeviceAt(int32 index, BNeuralDevice* device)
{
	if (device == NULL)
		return B_BAD_VALUE;
	return NeuralRosterData::Default()->GetDeviceAt(index, device);
}


status_t
BNeuralRoster::FindDevice(const char* id, BNeuralDevice* device)
{
	if (id == NULL || device == NULL)
		return B_BAD_VALUE;
	return NeuralRosterData::Default()->FindDevice(id, device);
}


status_t
BNeuralRoster::GetDefaultDevice(BNeuralDevice* device, uint32 flags,
	const BNeuralModel* model)
{
	if (device == NULL)
		return B_BAD_VALUE;
	if (model != NULL && model->InitCheck() != B_OK)
		return model->InitCheck();

	return NeuralRosterData::Default()->GetDefaultDevice(device, flags,
		model != NULL ? model->fGraph : NULL);
}


status_t
BNeuralRoster::Rescan()
{
	return NeuralRosterData::Default()->Rescan();
}


status_t
BNeuralRoster::StartWatching(const BMessenger& target)
{
	return NeuralRosterData::Default()->StartWatching(target);
}


status_t
BNeuralRoster::StopWatching(const BMessenger& target)
{
	return NeuralRosterData::Default()->StopWatching(target);
}


// #pragma mark - BNeuralBackend, BNeuralProgram


BNeuralBackend::BNeuralBackend(image_id image)
	:
	fImage(image)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralBackend::~BNeuralBackend()
{
}


void BNeuralBackend::_ReservedNeuralBackend1() {}
void BNeuralBackend::_ReservedNeuralBackend2() {}
void BNeuralBackend::_ReservedNeuralBackend3() {}
void BNeuralBackend::_ReservedNeuralBackend4() {}
void BNeuralBackend::_ReservedNeuralBackend5() {}
void BNeuralBackend::_ReservedNeuralBackend6() {}


BNeuralProgram::BNeuralProgram()
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralProgram::~BNeuralProgram()
{
}


bigtime_t
BNeuralProgram::LastDeviceTime() const
{
	return 0;
}


void BNeuralProgram::_ReservedNeuralProgram1() {}
void BNeuralProgram::_ReservedNeuralProgram2() {}
void BNeuralProgram::_ReservedNeuralProgram3() {}
void BNeuralProgram::_ReservedNeuralProgram4() {}
