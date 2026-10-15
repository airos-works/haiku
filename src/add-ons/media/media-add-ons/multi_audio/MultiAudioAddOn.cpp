/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Distributed under the terms of the MIT License.
 */


#include "MultiAudioAddOn.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <File.h>
#include <Looper.h>
#include <MessageQueue.h>
#include <Node.h>
#include <NodeMonitor.h>
#include <Path.h>

#include "debug.h"
#include "MultiAudioNode.h"
#include "MultiAudioDevice.h"


#define MULTI_SAVE

const char* kSettingsName = "Media/multi_audio_settings";
static const char* kDevicesRoot = "/dev/audio/hmulti/";


/*!	Hears the node monitor when a device comes or goes under
	/dev/audio/hmulti (a USB headset or webcam plugged in or pulled, for
	example) and has the add-on update its flavors.
*/
class DeviceWatcher : public BLooper {
public:
	DeviceWatcher(MultiAudioAddOn* addOn)
		:
		BLooper("multi_audio device watcher"),
		fAddOn(addOn)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_NODE_MONITOR) {
			// several entries usually turn up at once (a directory, then
			// the device in it): collect them for a moment
			snooze(200000);
			while (BMessage* next
					= MessageQueue()->FindMessage(B_NODE_MONITOR, 0)) {
				MessageQueue()->RemoveMessage(next);
				delete next;
			}
			fAddOn->DevicesChanged();
		} else
			BLooper::MessageReceived(message);
	}

private:
	MultiAudioAddOn*	fAddOn;
};


//! instantiation function
extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	CALLED();
	return new MultiAudioAddOn(image);
}


//	#pragma mark -


MultiAudioAddOn::MultiAudioAddOn(image_id image)
	: BMediaAddOn(image),
	fLock("multi_audio devices"),
	fDevices(),
	fRetiredDevices(),
	fNextID(0),
	fWatcher(NULL)
{
	CALLED();
	_UpdateDevices();
	_LoadSettings();

	fWatcher = new(std::nothrow) DeviceWatcher(this);
	if (fWatcher != NULL) {
		fWatcher->Run();
		_WatchDevices();
	}
	fInitStatus = B_OK;
}


MultiAudioAddOn::~MultiAudioAddOn()
{
	CALLED();

	if (fWatcher != NULL) {
		stop_watching(BMessenger(fWatcher));
		fWatcher->Lock();
		fWatcher->Quit();
	}

	BAutolock _(fLock);
	for (size_t i = 0; i < fDevices.size(); i++)
		delete fDevices[i].device;
	for (size_t i = 0; i < fRetiredDevices.size(); i++)
		delete fRetiredDevices[i];

	_SaveSettings();
}


status_t
MultiAudioAddOn::InitCheck(const char** _failureText)
{
	CALLED();
	return fInitStatus;
}


int32
MultiAudioAddOn::CountFlavors()
{
	CALLED();
	BAutolock _(fLock);
	return fDevices.size();
}


status_t
MultiAudioAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	CALLED();

	BAutolock _(fLock);
	if (index < 0 || (size_t)index >= fDevices.size())
		return B_BAD_INDEX;
	const device_entry& entry = fDevices[index];

	flavor_info* info = new (std::nothrow) flavor_info;
	if (info == NULL)
		return B_NO_MEMORY;

	MultiAudioNode::GetFlavor(info, entry.id);
	info->name = entry.device->Description().friendly_name;

	// Only what the device can do: a graphics card's HDMI audio has no
	// microphone, and must not be offered as an audio input.
	if (entry.device->Description().input_channel_count <= 0)
		info->kinds &= ~(uint64)B_PHYSICAL_INPUT;
	if (entry.device->Description().output_channel_count <= 0)
		info->kinds &= ~(uint64)B_PHYSICAL_OUTPUT;

	*_info = info;
	return B_OK;
}


BMediaNode*
MultiAudioAddOn::InstantiateNodeFor(const flavor_info* info, BMessage* config,
	status_t* _error)
{
	CALLED();

	BAutolock locker(fLock);
	MultiAudioDevice* device = _DeviceFor(info->internal_id);
	if (device == NULL) {
		*_error = B_ERROR;
		return NULL;
	}

#ifdef MULTI_SAVE
	if (fSettings.FindMessage(device->Description().friendly_name, config)
			== B_OK) {
		fSettings.RemoveData(device->Description().friendly_name);
	}
#endif
	locker.Unlock();

	MultiAudioNode* node = new (std::nothrow) MultiAudioNode(this,
		device->Description().friendly_name, device, info->internal_id, config);
	if (node == NULL)
		*_error = B_NO_MEMORY;
	else
		*_error = node->InitCheck();

	return node;
}


status_t
MultiAudioAddOn::GetConfigurationFor(BMediaNode* _node, BMessage* message)
{
	CALLED();
	MultiAudioNode* node = dynamic_cast<MultiAudioNode*>(_node);
	if (node == NULL)
		return B_BAD_TYPE;

#ifdef MULTI_SAVE
	if (message == NULL) {
		BMessage settings;
		if (node->GetConfigurationFor(&settings) == B_OK) {
			BAutolock _(fLock);
			fSettings.RemoveName(node->Name());
			fSettings.AddMessage(node->Name(), &settings);
		}
		return B_OK;
	}
#endif

	// currently never called by the media kit. Seems it is not implemented.

	return node->GetConfigurationFor(message);
}


bool
MultiAudioAddOn::WantsAutoStart()
{
	CALLED();
	return false;
}


status_t
MultiAudioAddOn::AutoStart(int count, BMediaNode** _node, int32* _internalID,
	bool* _hasMore)
{
	CALLED();
	return B_OK;
}


void
MultiAudioAddOn::DevicesChanged()
{
	CALLED();
	// Directories may have come or gone with the devices: watch anew.
	_WatchDevices();

	if (_UpdateDevices())
		NotifyFlavorChange();
}


/*!	The node of an unplugged device is destroyed by the media_addon_server
	after the flavor is gone; only then may the device object go, too.
*/
void
MultiAudioAddOn::DeviceReleased(MultiAudioDevice* device)
{
	CALLED();
	BAutolock _(fLock);
	std::vector<MultiAudioDevice*>::iterator found = std::find(
		fRetiredDevices.begin(), fRetiredDevices.end(), device);
	if (found == fRetiredDevices.end())
		return;

	fRetiredDevices.erase(found);
	delete device;
}


/*!	Makes the device list match /dev/audio/hmulti: new devices are opened
	and get a flavor ID of their own (never reused, so that a saved default
	cannot name the wrong device), devices that are gone are retired.
	Returns whether anything changed.
*/
bool
MultiAudioAddOn::_UpdateDevices()
{
	CALLED();
	std::vector<BString> paths;
	_ScanDevicePaths(kDevicesRoot, NULL, 0, paths);

	BAutolock _(fLock);
	bool changed = false;

	// retire the devices that are gone
	for (size_t i = 0; i < fDevices.size();) {
		if (std::find(paths.begin(), paths.end(), fDevices[i].path)
				!= paths.end()) {
			i++;
			continue;
		}
		PRINT(("MultiAudioAddOn: device %s is gone\n",
			fDevices[i].path.String()));
		fRetiredDevices.push_back(fDevices[i].device);
		fDevices.erase(fDevices.begin() + i);
		changed = true;
	}

	// and open the new ones
	for (size_t i = 0; i < paths.size(); i++) {
		bool known = false;
		for (size_t j = 0; j < fDevices.size(); j++) {
			if (fDevices[j].path == paths[i]) {
				known = true;
				break;
			}
		}
		if (known)
			continue;

		MultiAudioDevice* device = new(std::nothrow) MultiAudioDevice(
			paths[i].String() + strlen(kDevicesRoot), paths[i].String());
		if (device == NULL)
			continue;
		if (device->InitCheck() != B_OK) {
			delete device;
			continue;
		}

		device_entry entry;
		entry.device = device;
		entry.id = fNextID++;
		entry.path = paths[i];
		fDevices.push_back(entry);
		changed = true;
		PRINT(("MultiAudioAddOn: device %s, flavor %" B_PRId32 "\n",
			paths[i].String(), entry.id));
	}

	return changed;
}


void
MultiAudioAddOn::_ScanDevicePaths(const char* rootPath, BEntry* rootEntry,
	uint32 depth, std::vector<BString>& paths)
{
	CALLED();
	if (depth > 16)
		return;

	BDirectory root;
	if (rootEntry != NULL)
		root.SetTo(rootEntry);
	else
		root.SetTo(rootPath);

	BEntry entry;
	while (root.GetNextEntry(&entry) == B_OK) {
		if (entry.IsDirectory()) {
			_ScanDevicePaths(rootPath, &entry, depth + 1, paths);
		} else {
			BPath path;
			if (entry.GetPath(&path) == B_OK)
				paths.push_back(path.Path());
		}
	}
}


/*!	Watches the directories under /dev/audio/hmulti for devices that come
	and go. The directory of a kind of device (hmulti/usb) only exists while
	one is plugged in, as does hmulti itself on a machine without a sound
	card: when it is missing, the nearest directory that exists is watched
	instead.
*/
void
MultiAudioAddOn::_WatchDevices()
{
	CALLED();
	if (fWatcher == NULL)
		return;

	stop_watching(BMessenger(fWatcher));

	BPath path(kDevicesRoot);
	while (path.InitCheck() == B_OK) {
		BEntry entry(path.Path());
		if (entry.Exists() && entry.IsDirectory()) {
			_WatchDirectory(path.Path(), strcmp(path.Path(), "/dev") != 0
				&& strcmp(path.Path(), "/dev/audio") != 0);
			break;
		}
		BPath parent;
		if (path.GetParent(&parent) != B_OK)
			break;
		path = parent;
	}
}


void
MultiAudioAddOn::_WatchDirectory(const char* path, bool recursive)
{
	BDirectory directory(path);
	if (directory.InitCheck() != B_OK)
		return;

	node_ref nodeRef;
	if (directory.GetNodeRef(&nodeRef) == B_OK)
		watch_node(&nodeRef, B_WATCH_DIRECTORY, BMessenger(fWatcher));

	if (!recursive)
		return;

	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		if (!entry.IsDirectory())
			continue;
		BPath childPath;
		if (entry.GetPath(&childPath) == B_OK)
			_WatchDirectory(childPath.Path(), true);
	}
}


MultiAudioDevice*
MultiAudioAddOn::_DeviceFor(int32 id)
{
	for (size_t i = 0; i < fDevices.size(); i++) {
		if (fDevices[i].id == id)
			return fDevices[i].device;
	}
	return NULL;
}


void
MultiAudioAddOn::_SaveSettings()
{
	CALLED();
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		fSettings.Flatten(&file);
}


void
MultiAudioAddOn::_LoadSettings()
{
	CALLED();
	fSettings.MakeEmpty();

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK && fSettings.Unflatten(&file) == B_OK) {
		PRINT_OBJECT(fSettings);
	} else {
		PRINT(("Error unflattening settings file %s\n", path.Path()));
	}
}
