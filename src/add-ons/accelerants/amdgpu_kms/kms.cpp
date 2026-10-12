/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "kms.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


namespace Kms {


status_t
Ioctl(int fd, unsigned long request, void* argument)
{
	while (ioctl(fd, request, argument, 0) != 0) {
		if (errno == EINTR || errno == EAGAIN)
			continue;
		return errno;
	}
	return B_OK;
}


status_t
SetClientCap(int fd, uint64 capability, uint64 value)
{
	drm_set_client_cap cap = {};
	cap.capability = capability;
	cap.value = value;
	return Ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
}


status_t
GetResources(int fd, Resources& resources)
{
	memset(&resources, 0, sizeof(resources));
	drm_mode_card_res counts = {};
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &counts);
	if (status != B_OK)
		return status;

	drm_mode_card_res res = {};
	res.count_crtcs = min_c(counts.count_crtcs, kMaxObjects);
	res.count_connectors = min_c(counts.count_connectors, kMaxObjects);
	res.count_encoders = min_c(counts.count_encoders, kMaxObjects);
	res.crtc_id_ptr = (uintptr_t)resources.crtcs;
	res.connector_id_ptr = (uintptr_t)resources.connectors;
	res.encoder_id_ptr = (uintptr_t)resources.encoders;
	status = Ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	if (status != B_OK)
		return status;
	resources.crtcCount = res.count_crtcs;
	resources.connectorCount = res.count_connectors;
	resources.encoderCount = res.count_encoders;
	resources.maxWidth = res.max_width;
	resources.maxHeight = res.max_height;
	return B_OK;
}


static status_t
GetBlob(int fd, uint32 id, void* data, uint32& length)
{
	drm_mode_get_blob blob = {};
	blob.blob_id = id;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob);
	if (status != B_OK)
		return status;
	uint32 size = blob.length;
	void* copy = malloc(size > 0 ? size : 1);
	if (copy == NULL)
		return B_NO_MEMORY;
	blob.data = (uintptr_t)copy;
	status = Ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob);
	if (status == B_OK) {
		length = min_c(length, size);
		memcpy(data, copy, length);
	}
	free(copy);
	return status;
}


static status_t
GetPropertyName(int fd, uint32 property, char* name, size_t size)
{
	drm_mode_get_property get = {};
	get.prop_id = property;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &get);
	if (status == B_OK)
		strlcpy(name, get.name, size);
	return status;
}


status_t
GetConnector(int fd, uint32 id, Connector& connector)
{
	memset(&connector, 0, sizeof(connector));
	// the first call probes and counts
	drm_mode_get_connector counts = {};
	counts.connector_id = id;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &counts);
	if (status != B_OK)
		return status;

	uint32 propertyCount = counts.count_props;
	uint32* properties = (uint32*)calloc(propertyCount + 1, sizeof(uint32));
	uint64* values = (uint64*)calloc(propertyCount + 1, sizeof(uint64));
	if (properties == NULL || values == NULL) {
		free(properties);
		free(values);
		return B_NO_MEMORY;
	}

	drm_mode_get_connector get = {};
	get.connector_id = id;
	get.count_modes = min_c(counts.count_modes, kMaxModes);
	get.count_encoders = min_c(counts.count_encoders, kMaxObjects);
	get.count_props = propertyCount;
	get.modes_ptr = (uintptr_t)connector.modes;
	get.encoders_ptr = (uintptr_t)connector.encoders;
	get.props_ptr = (uintptr_t)properties;
	get.prop_values_ptr = (uintptr_t)values;
	status = Ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get);
	if (status == B_OK) {
		connector.id = id;
		connector.type = get.connector_type;
		connector.typeId = get.connector_type_id;
		connector.connected = get.connection == 1;
		connector.widthMM = get.mm_width;
		connector.heightMM = get.mm_height;
		connector.modeCount = min_c(get.count_modes, kMaxModes);
		connector.encoderCount = min_c(get.count_encoders, kMaxObjects);
		for (uint32 i = 0; i < min_c(get.count_props, propertyCount); i++) {
			char name[DRM_PROP_NAME_LEN];
			if (GetPropertyName(fd, properties[i], name, sizeof(name)) != B_OK
				|| strcmp(name, "EDID") != 0 || values[i] == 0)
				continue;
			uint32 length = kEdidLength;
			if (GetBlob(fd, values[i], connector.edid, length) == B_OK)
				connector.edidLength = length;
		}
	}
	free(properties);
	free(values);
	return status;
}


const char*
ConnectorTypeName(uint32 type)
{
	switch (type) {
		case DRM_MODE_CONNECTOR_VGA: return "VGA";
		case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
		case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
		case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
		case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
		case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
		case DRM_MODE_CONNECTOR_eDP: return "eDP";
		case DRM_MODE_CONNECTOR_USB: return "USB";
		default: return "Unknown";
	}
}


status_t
GetEncoderCrtcs(int fd, uint32 encoder, uint32& possibleCrtcs)
{
	drm_mode_get_encoder get = {};
	get.encoder_id = encoder;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &get);
	if (status == B_OK)
		possibleCrtcs = get.possible_crtcs;
	return status;
}


uint32
PropertyId(int fd, uint32 object, uint32 objectType, const char* name)
{
	drm_mode_obj_get_properties counts = {};
	counts.obj_id = object;
	counts.obj_type = objectType;
	if (Ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &counts) != B_OK)
		return 0;
	uint32 count = counts.count_props;
	uint32* properties = (uint32*)calloc(count + 1, sizeof(uint32));
	uint64* values = (uint64*)calloc(count + 1, sizeof(uint64));
	uint32 id = 0;
	if (properties != NULL && values != NULL) {
		drm_mode_obj_get_properties get = {};
		get.obj_id = object;
		get.obj_type = objectType;
		get.count_props = count;
		get.props_ptr = (uintptr_t)properties;
		get.prop_values_ptr = (uintptr_t)values;
		if (Ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &get) == B_OK) {
			for (uint32 i = 0; i < min_c(get.count_props, count); i++) {
				char propertyName[DRM_PROP_NAME_LEN];
				if (GetPropertyName(fd, properties[i], propertyName,
						sizeof(propertyName)) == B_OK
					&& strcmp(propertyName, name) == 0) {
					id = properties[i];
					break;
				}
			}
		}
	}
	free(properties);
	free(values);
	return id;
}


static uint64
PropertyValue(int fd, uint32 object, uint32 objectType, uint32 property)
{
	drm_mode_obj_get_properties counts = {};
	counts.obj_id = object;
	counts.obj_type = objectType;
	if (Ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &counts) != B_OK)
		return 0;
	uint32 count = counts.count_props;
	uint32* properties = (uint32*)calloc(count + 1, sizeof(uint32));
	uint64* values = (uint64*)calloc(count + 1, sizeof(uint64));
	uint64 value = 0;
	if (properties != NULL && values != NULL) {
		drm_mode_obj_get_properties get = {};
		get.obj_id = object;
		get.obj_type = objectType;
		get.count_props = count;
		get.props_ptr = (uintptr_t)properties;
		get.prop_values_ptr = (uintptr_t)values;
		if (Ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &get) == B_OK) {
			for (uint32 i = 0; i < min_c(get.count_props, count); i++) {
				if (properties[i] == property) {
					value = values[i];
					break;
				}
			}
		}
	}
	free(properties);
	free(values);
	return value;
}


status_t
GetPlanes(int fd, Plane* planes, uint32& count)
{
	uint32 ids[kMaxObjects];
	drm_mode_get_plane_res res = {};
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res);
	if (status != B_OK)
		return status;
	res.count_planes = min_c(res.count_planes, kMaxObjects);
	res.plane_id_ptr = (uintptr_t)ids;
	status = Ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res);
	if (status != B_OK)
		return status;

	uint32 found = 0;
	for (uint32 i = 0; i < res.count_planes && found < count; i++) {
		drm_mode_get_plane get = {};
		get.plane_id = ids[i];
		if (Ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &get) != B_OK)
			continue;
		Plane& plane = planes[found++];
		plane.id = ids[i];
		plane.possibleCrtcs = get.possible_crtcs;
		uint32 typeProperty = PropertyId(fd, ids[i], DRM_MODE_OBJECT_PLANE,
			"type");
		plane.type = typeProperty != 0 ? (uint32)PropertyValue(fd, ids[i],
			DRM_MODE_OBJECT_PLANE, typeProperty) : kPlaneOverlay;
	}
	count = found;
	return B_OK;
}


status_t
CreateBlob(int fd, const void* data, uint32 length, uint32& id)
{
	drm_mode_create_blob create = {};
	create.data = (uintptr_t)data;
	create.length = length;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &create);
	if (status == B_OK)
		id = create.blob_id;
	return status;
}


void
DestroyBlob(int fd, uint32 id)
{
	if (id == 0)
		return;
	drm_mode_destroy_blob destroy = {};
	destroy.blob_id = id;
	Ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy);
}


AtomicRequest::AtomicRequest()
	:
	fCount(0),
	fOverflow(false)
{
}


void
AtomicRequest::Add(uint32 object, uint32 property, uint64 value)
{
	if (property == 0)
		return;
	if (fCount == kMaxProperties) {
		fOverflow = true;
		return;
	}
	fObjects[fCount] = object;
	fProperties[fCount] = property;
	fValues[fCount] = value;
	fCount++;
}


status_t
AtomicRequest::Commit(int fd, uint32 flags)
{
	if (fOverflow)
		return B_NO_MEMORY;
	// the kernel wants the changes grouped by object
	uint32 objects[kMaxProperties];
	uint32 counts[kMaxProperties];
	uint32 properties[kMaxProperties];
	uint64 values[kMaxProperties];
	uint32 objectCount = 0, n = 0;
	bool done[kMaxProperties] = {};
	for (uint32 i = 0; i < fCount; i++) {
		if (done[i])
			continue;
		objects[objectCount] = fObjects[i];
		counts[objectCount] = 0;
		for (uint32 j = i; j < fCount; j++) {
			if (done[j] || fObjects[j] != fObjects[i])
				continue;
			properties[n] = fProperties[j];
			values[n] = fValues[j];
			n++;
			counts[objectCount]++;
			done[j] = true;
		}
		objectCount++;
	}

	drm_mode_atomic atomic = {};
	atomic.flags = flags;
	atomic.count_objs = objectCount;
	atomic.objs_ptr = (uintptr_t)objects;
	atomic.count_props_ptr = (uintptr_t)counts;
	atomic.props_ptr = (uintptr_t)properties;
	atomic.prop_values_ptr = (uintptr_t)values;
	return Ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}


status_t
CreateBuffer(int fd, uint32 width, uint32 height, uint32 format,
	Buffer& buffer, bool addFramebuffer)
{
	memset(&buffer, 0, sizeof(buffer));
	buffer.area = -1;

	drm_mode_create_dumb create = {};
	create.width = width;
	create.height = height;
	create.bpp = 32;
	status_t status = Ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (status != B_OK)
		return status;
	buffer.handle = create.handle;
	buffer.width = width;
	buffer.height = height;
	buffer.pitch = create.pitch;
	buffer.size = create.size;

	drm_mode_map_dumb map = {};
	map.handle = buffer.handle;
	status = Ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (status == B_OK) {
		drm_haiku_map haikuMap = {};
		haikuMap.offset = map.offset;
		haikuMap.size = (buffer.size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
		// cloneable: direct windows and clones of the accelerant use it
		haikuMap.flags = DRM_HAIKU_MAP_WRITE | DRM_HAIKU_MAP_CLONEABLE;
		status = Ioctl(fd, DRM_HAIKU_IOCTL_MAP, &haikuMap);
		if (status == B_OK) {
			buffer.address = (void*)(addr_t)haikuMap.address;
			buffer.area = haikuMap.area;
		}
	}

	if (status == B_OK && addFramebuffer) {
		drm_mode_fb_cmd2 framebuffer = {};
		framebuffer.width = width;
		framebuffer.height = height;
		framebuffer.pixel_format = format;
		framebuffer.handles[0] = buffer.handle;
		framebuffer.pitches[0] = buffer.pitch;
		status = Ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &framebuffer);
		if (status == B_OK)
			buffer.framebuffer = framebuffer.fb_id;
	}

	if (status != B_OK)
		DestroyBuffer(fd, buffer);
	return status;
}


void
DestroyBuffer(int fd, Buffer& buffer)
{
	if (buffer.framebuffer != 0) {
		uint32 id = buffer.framebuffer;
		Ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
	}
	if (buffer.address != NULL) {
		drm_haiku_unmap unmap = {};
		unmap.address = (addr_t)buffer.address;
		Ioctl(fd, DRM_HAIKU_IOCTL_UNMAP, &unmap);
	}
	if (buffer.handle != 0) {
		drm_mode_destroy_dumb destroy = {};
		destroy.handle = buffer.handle;
		Ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	memset(&buffer, 0, sizeof(buffer));
	buffer.area = -1;
}


status_t
WaitVBlank(int fd, uint32 crtcIndex, uint32* sequence)
{
	drm_wait_vblank wait = {};
	wait.request.type = (drm_vblank_seq_type)(_DRM_VBLANK_RELATIVE
		| ((crtcIndex << _DRM_VBLANK_HIGH_CRTC_SHIFT)
			& _DRM_VBLANK_HIGH_CRTC_MASK));
	wait.request.sequence = 1;
	status_t status = Ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &wait);
	if (status == B_OK && sequence != NULL)
		*sequence = wait.reply.sequence;
	return status;
}


status_t
SetCursor(int fd, uint32 crtc, uint32 handle, uint32 width, uint32 height,
	int32 hotX, int32 hotY)
{
	drm_mode_cursor2 cursor = {};
	cursor.flags = DRM_MODE_CURSOR_BO;
	cursor.crtc_id = crtc;
	cursor.handle = handle;
	cursor.width = width;
	cursor.height = height;
	cursor.hot_x = hotX;
	cursor.hot_y = hotY;
	return Ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &cursor);
}


status_t
MoveCursor(int fd, uint32 crtc, int32 x, int32 y)
{
	drm_mode_cursor cursor = {};
	cursor.flags = DRM_MODE_CURSOR_MOVE;
	cursor.crtc_id = crtc;
	cursor.x = x;
	cursor.y = y;
	return Ioctl(fd, DRM_IOCTL_MODE_CURSOR, &cursor);
}


}	// namespace Kms
