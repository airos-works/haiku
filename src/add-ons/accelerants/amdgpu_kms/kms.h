/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDGPU_KMS_KMS_H
#define AMDGPU_KMS_KMS_H


#include <OS.h>

#include "drm.h"
#include "drm_fourcc.h"
#include "drm_haiku.h"
#include "drm_mode.h"


/*	A thin layer over the Linux KMS ioctls of amdgpu_drm (Linux's DRM driver
	as a Haiku add-on): no libdrm in app_server. All functions return B_OK
	or an error code from errno. */
namespace Kms {


static const uint32 kMaxObjects = 32;
static const uint32 kMaxModes = 64;
static const uint32 kEdidLength = 512;


status_t Ioctl(int fd, unsigned long request, void* argument);

status_t SetClientCap(int fd, uint64 capability, uint64 value);

// the IDs of the card's objects
struct Resources {
	uint32	crtcs[kMaxObjects];
	uint32	crtcCount;
	uint32	connectors[kMaxObjects];
	uint32	connectorCount;
	uint32	encoders[kMaxObjects];
	uint32	encoderCount;
	uint32	maxWidth;
	uint32	maxHeight;
};

status_t GetResources(int fd, Resources& resources);

struct Connector {
	uint32				id;
	uint32				type;
	uint32				typeId;
	bool				connected;
	uint32				widthMM;
	uint32				heightMM;
	uint32				encoders[kMaxObjects];
	uint32				encoderCount;
	drm_mode_modeinfo	modes[kMaxModes];
	uint32				modeCount;
	uint8				edid[kEdidLength];
	uint32				edidLength;
};

// probes the connector: what is attached, its modes and EDID
status_t GetConnector(int fd, uint32 id, Connector& connector);
const char* ConnectorTypeName(uint32 type);

status_t GetEncoderCrtcs(int fd, uint32 encoder, uint32& possibleCrtcs);

enum {
	kPlaneOverlay = 0,
	kPlanePrimary = 1,
	kPlaneCursor = 2,
};

struct Plane {
	uint32	id;
	uint32	possibleCrtcs;
	uint32	type;
};

status_t GetPlanes(int fd, Plane* planes, uint32& count);

// the ID of the property \a name of an object, 0 if it has none
uint32 PropertyId(int fd, uint32 object, uint32 objectType, const char* name);

status_t CreateBlob(int fd, const void* data, uint32 length, uint32& id);
void DestroyBlob(int fd, uint32 id);

// a list of property changes, committed as one
class AtomicRequest {
public:
								AtomicRequest();

			void				Add(uint32 object, uint32 property,
									uint64 value);
			status_t			Commit(int fd, uint32 flags);

private:
	static const uint32 kMaxProperties = 256;

			uint32				fObjects[kMaxProperties];
			uint32				fProperties[kMaxProperties];
			uint64				fValues[kMaxProperties];
			uint32				fCount;
			bool				fOverflow;
};

// a frame buffer in VRAM the CPU can write, mapped into the team
struct Buffer {
	uint32	handle;
	uint32	framebuffer;
	uint32	width;
	uint32	height;
	uint32	pitch;
	uint64	size;
	void*	address;
	area_id	area;
};

status_t CreateBuffer(int fd, uint32 width, uint32 height, uint32 format,
	Buffer& buffer, bool addFramebuffer = true);
void DestroyBuffer(int fd, Buffer& buffer);

// waits for the next vertical blank of the CRTC with the index \a crtcIndex
status_t WaitVBlank(int fd, uint32 crtcIndex, uint32* sequence = NULL);

status_t SetCursor(int fd, uint32 crtc, uint32 handle, uint32 width,
	uint32 height, int32 hotX, int32 hotY);
status_t MoveCursor(int fd, uint32 crtc, int32 x, int32 y);

// how often a GPU reset has lost the contents of video memory
// (AMDGPU_INFO_VRAM_LOST_COUNTER)
status_t VramLostCounter(int fd, uint32& counter);


}	// namespace Kms


#endif	// AMDGPU_KMS_KMS_H
