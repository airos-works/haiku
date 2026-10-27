/*
** Copyright 2004, the Haiku project. All rights reserved.
** Distributed under the terms of the MIT License.
**
** Author : Jérôme Duval
** Original authors: Marcus Overhagen, Axel Dörfler
*/
#ifndef _DEVICE_MANAGER_H
#define _DEVICE_MANAGER_H

// Manager for devices monitoring
#include <Handler.h>
#include <Node.h>
#include <Looper.h>
#include <Locker.h>
#include <StringList.h>


class DeviceManager : public BLooper {
	public:
		DeviceManager();
		~DeviceManager();

		void		LoadState();
		void		SaveState();
		
		status_t StartMonitoringDevice(const char* device);
		status_t StopMonitoringDevice(const char* device);

		void MessageReceived(BMessage *msg);
		
	private:
		status_t AddDirectory(node_ref* nref);
		status_t RemoveDirectory(node_ref* nref);
		status_t AddDevice(entry_ref* nref);
		static status_t _PrepareDevice(void* message);

		BLocker fLock;
		BStringList fDevices;
			// handed to the server: the first look and the node monitor
			// may both see a radio
};

#endif // _DEVICE_MANAGER_H
