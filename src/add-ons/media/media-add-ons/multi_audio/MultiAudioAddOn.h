/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Distributed under the terms of the MIT License.
 */
#ifndef MULTI_AUDIO_ADDON_H
#define MULTI_AUDIO_ADDON_H


#include <vector>

#include <Locker.h>
#include <MediaDefs.h>
#include <MediaAddOn.h>
#include <Message.h>
#include <String.h>

class BEntry;
class BLooper;
class MultiAudioDevice;


class MultiAudioAddOn : public BMediaAddOn {
public:
						MultiAudioAddOn(image_id image);
	virtual				~MultiAudioAddOn();

	virtual	status_t	InitCheck(const char** _failureText);
	virtual	int32		CountFlavors();
	virtual	status_t	GetFlavorAt(int32 i, const flavor_info** _info);
	virtual	BMediaNode*	InstantiateNodeFor(const flavor_info* info,
							BMessage* config, status_t* _error);
	virtual	status_t	GetConfigurationFor(BMediaNode* node,
							BMessage* message);
	virtual	bool		WantsAutoStart();
	virtual	status_t	AutoStart(int count, BMediaNode** _node,
							int32* _internalID, bool* _hasMore);

			// called by the device watcher when /dev/audio/hmulti changed
			void		DevicesChanged();
			// called by a node when it lets go of its device
			void		DeviceReleased(MultiAudioDevice* device);

private:
			struct device_entry {
				MultiAudioDevice*	device;
				int32				id;
				BString				path;
			};

			bool		_UpdateDevices();
			void		_ScanDevicePaths(const char* rootPath,
							BEntry* rootEntry, uint32 depth,
							std::vector<BString>& paths);
			void		_WatchDevices();
			void		_WatchDirectory(const char* path, bool recursive);
			MultiAudioDevice* _DeviceFor(int32 id);
			void		_SaveSettings();
			void		_LoadSettings();

private:
	status_t 		fInitStatus;
	BLocker			fLock;
	std::vector<device_entry> fDevices;
	std::vector<MultiAudioDevice*> fRetiredDevices;
		// unplugged, but their node may still hold them
	int32			fNextID;
	BLooper*		fWatcher;

	BMessage		fSettings;
		// loaded from settings directory
};

extern "C" BMediaAddOn* make_media_addon(image_id you);

#endif	// MULTI_AUDIO_ADDON_H
