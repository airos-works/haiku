/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_SESSION_H
#define IRIS_SESSION_H


#include <OS.h>
#include <String.h>

#include "IrisDevice.h"


/*	HFI generation 1 as Linux' iris and venus drivers speak it to SM8250's
	firmware (iris_hfi_gen1_defines.h, hfi_helper.h): what decoding and
	encoding share. */

#define HFI_CMD_SYS_SESSION_INIT			0x10007
#define HFI_CMD_SYS_SESSION_END				0x10008
#define HFI_CMD_SESSION_SET_PROPERTY		0x11001
#define HFI_CMD_SESSION_SET_BUFFERS			0x11002
#define HFI_CMD_SESSION_LOAD_RESOURCES		0x211001
#define HFI_CMD_SESSION_START				0x211002
#define HFI_CMD_SESSION_STOP				0x211003
#define HFI_CMD_SESSION_EMPTY_BUFFER		0x211004
#define HFI_CMD_SESSION_FILL_BUFFER			0x211005
#define HFI_CMD_SESSION_FLUSH				0x211008
#define HFI_CMD_SESSION_RELEASE_BUFFERS		0x21100b
#define HFI_CMD_SESSION_RELEASE_RESOURCES	0x21100c
#define HFI_CMD_SESSION_CONTINUE			0x21100d

#define HFI_MSG_SYS_SESSION_INIT			0x20006
#define HFI_MSG_SYS_SESSION_END				0x20007
#define HFI_MSG_EVENT_NOTIFY				0x21001
#define HFI_MSG_SESSION_LOAD_RESOURCES		0x221001
#define HFI_MSG_SESSION_START				0x221002
#define HFI_MSG_SESSION_STOP				0x221003
#define HFI_MSG_SESSION_FLUSH				0x221006
#define HFI_MSG_SESSION_EMPTY_BUFFER		0x221007
#define HFI_MSG_SESSION_FILL_BUFFER			0x221008
#define HFI_MSG_SESSION_RELEASE_RESOURCES	0x22100a
#define HFI_MSG_SESSION_RELEASE_BUFFERS		0x22100c

#define HFI_SESSION_TYPE_ENC				1
#define HFI_SESSION_TYPE_DEC				2
#define HFI_VIDEO_CODEC_H264				0x00000002
#define HFI_VIDEO_CODEC_HEVC				0x00002000

#define HFI_BUFFER_INPUT					0x1
#define HFI_BUFFER_OUTPUT					0x2
#define HFI_BUFFER_OUTPUT2					0x3
#define HFI_BUFFER_INTERNAL_PERSIST			0x4
#define HFI_BUFFER_INTERNAL_PERSIST_1		0x5
#define HFI_BUFFER_INTERNAL_SCRATCH			0x6		// "BIN"
#define HFI_BUFFER_INTERNAL_SCRATCH_1		0x7
#define HFI_BUFFER_INTERNAL_SCRATCH_2		0x8

#define HFI_PROPERTY_PARAM_FRAME_SIZE		0x1001
#define HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT 0x1003
#define HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT 0x1005
#define HFI_PROPERTY_PARAM_WORK_MODE		0x1015
#define HFI_PROPERTY_CONFIG_VIDEOCORES_USAGE 0x2002
#define HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL 0x201001

#define HFI_COLOR_FORMAT_NV12				0x0002
#define HFI_COLOR_FORMAT_NV12_UBWC			0x8002

#define HFI_FLUSH_OUTPUT					0x1000002
#define HFI_FLUSH_ALL						0x1000004

#define HFI_EVENT_SYS_ERROR					0x1
#define HFI_EVENT_SESSION_ERROR				0x2

#define HFI_BUFFERFLAG_EOS					0x00000001
#define HFI_BUFFERFLAG_DATACORRUPT			0x00000008
#define HFI_BUFFERFLAG_DROP_FRAME			0x20000000

#define HFI_ERR_SESSION_UNSUPPORTED_SETTING	0x1008
#define HFI_ERR_SESSION_UNSUPPORTED_STREAM	0x100d
#define HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE 0x1010
#define HFI_ERR_SESSION_INVALID_SCALE_FACTOR 0x1012
#define HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED 0x1013

#define HFI_PICTURE_I						0x1
#define HFI_PICTURE_P						0x2
#define HFI_PICTURE_B						0x4
#define HFI_PICTURE_IDR						0x8

// the end of stream packet's buffer, which nothing is at
#define HFI_END_OF_STREAM_ADDRESS			0xdeadb000

#define IRIS_ALIGN(x, a)		((((x) + (a) - 1) / (a)) * (a))
#define IRIS_DIV_ROUND_UP(x, a)	(((x) + (a) - 1) / (a))


bool		iris_debug_enabled();

#define IRIS_TRACE(...) \
	do { \
		if (iris_debug_enabled()) \
			fprintf(stderr, "iris: " __VA_ARGS__); \
	} while (false)


/*!	One HFI session on the codec (IRIS_DEBUG=1 traces it): the device, the
	command packets, waiting for the firmware's answers. A decoder or an
	encoder handles the messages of its kind in _Handle().
*/
class IrisSession {
public:
								IrisSession();
	virtual						~IrisSession();

			const char*			Error() const { return fError.String(); }
			const char*			FirmwareVersion() const
									{ return fDevice.FirmwareVersion(); }

protected:
			status_t			_OpenSession(uint32 type, uint32 codec);
			//!	STOP and RELEASE_RESOURCES when started, SESSION_END.
			void				_EndSession();

			status_t			_Fail(status_t status, const char* format,
									...);

			status_t			_Send(const uint32* packet);
			status_t			_SetProperty(uint32 property,
									const uint32* payload, uint32 words);
			status_t			_SetProperty(uint32 property, uint32 a);
			status_t			_SetProperty(uint32 property, uint32 a,
									uint32 b);
			status_t			_SetProperty(uint32 property, uint32 a,
									uint32 b, uint32 c);
			status_t			_Simple(uint32 command, uint32 answer);
			status_t			_Flush(uint32 type);

			//!	Handles messages until one of \a type came.
			status_t			_WaitFor(uint32 type);
			//!	One message, waiting up to \a timeout; B_TIMED_OUT when none.
			status_t			_Pump(bigtime_t timeout);
	virtual	status_t			_Handle(const uint32* packet, size_t size) = 0;
			//!	The error and system events, for _Handle().
			status_t			_HandleErrorEvent(const uint32* packet,
									size_t size);

			status_t			_SetInternal(uint32 type,
									IrisDevice::Buffer& buffer, size_t size);
			status_t			_ReleaseInternal(uint32 type,
									IrisDevice::Buffer& buffer);

			IrisDevice			fDevice;
			BString				fError;
			uint32				fSession;
			bool				fOpen;
			bool				fStarted;	// LOAD_RESOURCES and START done
			bool				fFailed;
};


#endif	// IRIS_SESSION_H
