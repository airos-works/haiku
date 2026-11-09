/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_ROSTER_PRIVATE_H
#define _NEURAL_ROSTER_PRIVATE_H


#include <Locker.h>
#include <ObjectList.h>
#include <String.h>

#include <add-ons/neural/NeuralBackend.h>
#include <neural/NeuralRoster.h>


class BLooper;


namespace BPrivate {


class NeuralGraphData;


//! The identifier of the kit's own CPU device.
extern const char* const kNeuralCPUDeviceID;


/*!	What is behind BNeuralRoster: the loaded back ends and their devices.
*/
class NeuralRosterData {
public:
	struct Backend {
		BString				name;
		BString				path;
		image_id			image;
		BNeuralBackend*		backend;
	};

	struct Device {
		BNeuralDevice		device;
		Backend*			backend;
			// NULL: the CPU
		int32				index;
		int32				priority;
	};

	static	NeuralRosterData*	Default();

			status_t			Rescan();

			int32				CountDevices();
			status_t			GetDeviceAt(int32 index,
									BNeuralDevice* device);
			status_t			FindDevice(const char* id,
									BNeuralDevice* device);
			bool				IsAvailable(const char* id);

	/*!	The back end of a device and the device's number there; a NULL
		back end with B_OK is the CPU. */
			status_t			GetBackend(const char* id,
									BNeuralBackend** _backend,
									int32* _index);
			status_t			GetDeviceDetails(const char* id,
									BMessage* details);
	/*!	Which operators the device runs (supported has one entry per
		operator); returns how many, or an error. */
			int32				GetSupportedOperators(const char* id,
									const NeuralGraphData* graph,
									bool* supported);

			status_t			GetDefaultDevice(BNeuralDevice* device,
									uint32 flags,
									const NeuralGraphData* graph);

			status_t			StartWatching(const BMessenger& target);
			status_t			StopWatching(const BMessenger& target);

	static	void				InitDevice(BNeuralDevice& device,
									const char* id, const char* name,
									const char* vendor, const char* backend,
									neural_device_kind kind, uint32 flags);

private:
								NeuralRosterData();

	static	void				_Create();
			void				_LoadBackends();
			void				_LoadBackend(const char* path);
			void				_CollectDevices(
									BObjectList<Device, true>& devices);
			Device*				_FindDevice(const char* id);
			void				_ReadSettings();
			void				_Notify(uint32 what, const Device& device);

			BLocker				fLock;
			bool				fLoaded;
			BObjectList<Backend, true> fBackends;
			BObjectList<Device, true> fDevices;
			BObjectList<BMessenger, true> fWatchers;
			BLooper*			fWatchLooper;
			BString				fPreferredDevice;
			BString				fDisabledBackends;
};


}	// namespace BPrivate


#endif	// _NEURAL_ROSTER_PRIVATE_H
