/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	An input_server device add-on for pointer_steps_test: it queues relative
	pointer moves when the test asks for them, back to back, as a mouse whose
	reports arrive together does (a Bluetooth LE mouse after a missed
	connection event, any device after a hiccup).

	The test writes to the port "pointer steps test": code 1 with an int32
	step queues two B_MOUSE_MOVED of that step in x.
*/


#include <stdio.h>
#include <string.h>

#include <InterfaceDefs.h>
#include <Message.h>
#include <OS.h>
#include <add-ons/input_server/InputServerDevice.h>


static const char* kPortName = "pointer steps test";


class PointerStepsDevice : public BInputServerDevice {
public:
								PointerStepsDevice();

	virtual	status_t			Start(const char* name, void* cookie);
	virtual	status_t			Stop(const char* name, void* cookie);

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Thread();
			void				_Move(int32 step);

			port_id				fPort;
			thread_id			fThread;
};


extern "C" _EXPORT BInputServerDevice*
instantiate_input_device()
{
	return new(std::nothrow) PointerStepsDevice();
}


PointerStepsDevice::PointerStepsDevice()
	:
	fPort(-1),
	fThread(-1)
{
	input_device_ref device = { (char*)"Pointer steps test",
		B_POINTING_DEVICE, NULL };
	input_device_ref* devices[2] = { &device, NULL };
	RegisterDevices(devices);
}


status_t
PointerStepsDevice::Start(const char* name, void* cookie)
{
	fPort = create_port(16, kPortName);
	if (fPort < 0)
		return fPort;

	// above the event loop of input_server, as the real mouse add-ons
	fThread = spawn_thread(_ThreadEntry, "pointer steps test",
		B_FIRST_REAL_TIME_PRIORITY + 4, this);
	if (fThread < 0) {
		delete_port(fPort);
		fPort = -1;
		return fThread;
	}
	resume_thread(fThread);
	return B_OK;
}


status_t
PointerStepsDevice::Stop(const char* name, void* cookie)
{
	delete_port(fPort);
	fPort = -1;
	status_t result;
	wait_for_thread(fThread, &result);
	fThread = -1;
	return B_OK;
}


status_t
PointerStepsDevice::_ThreadEntry(void* data)
{
	((PointerStepsDevice*)data)->_Thread();
	return B_OK;
}


void
PointerStepsDevice::_Thread()
{
	while (true) {
		int32 code;
		int32 step;
		ssize_t size = read_port(fPort, &code, &step, sizeof(step));
		if (size == B_INTERRUPTED)
			continue;
		if (size < 0)
			return;
		if (code == 1 && size == sizeof(step)) {
			// Build both first: the second must be queued right after the
			// first, before input_server's event loop takes it.
			BMessage* first = new(std::nothrow) BMessage(B_MOUSE_MOVED);
			BMessage* second = new(std::nothrow) BMessage(B_MOUSE_MOVED);
			if (first == NULL || second == NULL) {
				delete first;
				delete second;
				continue;
			}
			BMessage* messages[2] = { first, second };
			for (int32 i = 0; i < 2; i++) {
				messages[i]->AddInt64("when", system_time());
				messages[i]->AddInt32("be:device_subtype",
					B_MOUSE_POINTING_DEVICE);
				messages[i]->AddInt32("buttons", 0);
				messages[i]->AddInt32("x", step);
				messages[i]->AddInt32("y", 0);
			}
			if (EnqueueMessage(first) != B_OK)
				delete first;
			if (EnqueueMessage(second) != B_OK)
				delete second;
		}
	}
}
