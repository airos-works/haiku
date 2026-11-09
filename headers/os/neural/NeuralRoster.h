/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_ROSTER_H
#define _NEURAL_ROSTER_H


#include <Messenger.h>
#include <neural/NeuralDefs.h>
#include <String.h>

class BMessage;
class BNeuralModel;


/*!	One processor that can run networks: an NPU, a graphics card, or the
	CPU. Devices are values: they name a device, and stay valid (but stop
	being IsAvailable()) when it is unplugged.

	The identifier is stable across restarts and is what programs should
	keep in their settings: "cpu", "rk3588_npu/0", "vulkan/0".
*/
class BNeuralDevice {
public:
								BNeuralDevice();
								BNeuralDevice(const BNeuralDevice& other);
								~BNeuralDevice();

			BNeuralDevice&		operator=(const BNeuralDevice& other);
			bool				operator==(const BNeuralDevice& other) const;
			bool				operator!=(const BNeuralDevice& other) const;

			bool				IsValid() const;
	//! Whether the device is there right now.
			bool				IsAvailable() const;

			const char*			ID() const;
	//! "Rockchip RK3588 NPU"
			const char*			Name() const;
			const char*			Vendor() const;
			neural_device_kind	Kind() const;
	//! B_NEURAL_DEVICE_QUANTIZED, ...
			uint32				Flags() const;
	//! The name of the back-end add-on that drives it.
			const char*			Backend() const;

	/*!	Everything the back end says about the device: "driver",
		"version", "cores" (int32), "memory" (int64), "path" and whatever
		else it knows; none of the fields is promised. */
			status_t			GetInfo(BMessage* info) const;

	/*!	How many of the model's operators the device would run itself;
		the others would run on the CPU. Negative: an error. This loads
		nothing onto the device. */
			int32				CountSupportedOperators(
									const BNeuralModel& model) const;

private:
	friend class BNeuralRoster;
	friend class BNeuralSession;

			BString				fID;
			BString				fName;
			BString				fVendor;
			BString				fBackend;
			neural_device_kind	fKind;
			uint32				fFlags;
			uint32				_reserved[6];
};


/*!	The devices of this machine. There is one roster per team,
	BNeuralRoster::Default(); it loads the back-end add-ons the first time
	it is asked something.
*/
class BNeuralRoster {
public:
	static	BNeuralRoster*		Default();

			int32				CountDevices();
			status_t			GetDeviceAt(int32 index,
									BNeuralDevice* device);
			status_t			FindDevice(const char* id,
									BNeuralDevice* device);

	/*!	The device a program should use when it has no reason to pick
		one: an NPU if there is one, else a graphics processor, else the
		CPU; B_NEURAL_PREFER_LOW_POWER and B_NEURAL_CPU_ONLY change the
		choice. With a model, devices that would run none of it are
		skipped, and so are those that cannot run all of it when
		B_NEURAL_NO_FALLBACK is given. The user's choice in the settings
		(the "preferred device") comes first when it qualifies. */
			status_t			GetDefaultDevice(BNeuralDevice* device,
									uint32 flags = 0,
									const BNeuralModel* model = NULL);

	//! Looks for devices again (the add-ons are asked anew).
			status_t			Rescan();

	//! B_NEURAL_DEVICE_ADDED and _REMOVED are sent to the target.
			status_t			StartWatching(const BMessenger& target);
			status_t			StopWatching(const BMessenger& target);

private:
								BNeuralRoster();
								~BNeuralRoster();
								BNeuralRoster(const BNeuralRoster&);
			BNeuralRoster&		operator=(const BNeuralRoster&);

			void*				fData;
			uint32				_reserved[6];
};


#endif	// _NEURAL_ROSTER_H
