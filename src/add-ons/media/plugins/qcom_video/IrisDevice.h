/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_DEVICE_H
#define IRIS_DEVICE_H


#include <OS.h>


/*!	/dev/video/qcom_iris/0 (qcom_iris.h): the SC8280XP's video codec, whose
	firmware the driver has started. HFI command packets go in, the
	firmware's messages come out, and buffers for it are allocated here.
	Each IrisDevice is an open of its own, with the sessions it began.
*/
class IrisDevice {
public:
	struct Buffer {
		void*		address = NULL;
		uint32		deviceAddress = 0;
		size_t		size = 0;
		area_id		area = -1;
	};

								IrisDevice();
								~IrisDevice();

			status_t			Open();
			void				Close();
			bool				IsOpen() const { return fDevice >= 0; }

			const char*			FirmwareVersion() const
									{ return fFirmware; }

			// \a packet's first word is its size in bytes
			status_t			Send(const void* packet);
			// the next message, up to \a size bytes, waiting up to
			// \a timeout; B_TIMED_OUT when none came
			status_t			Receive(void* packet, size_t size,
									bigtime_t timeout,
									size_t* _received = NULL);

			status_t			Allocate(size_t size, Buffer& buffer);
			void				Free(Buffer& buffer);

private:
			int					fDevice;
			char				fFirmware[128];
};


#endif	// IRIS_DEVICE_H
