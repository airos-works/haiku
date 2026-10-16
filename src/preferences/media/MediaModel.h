/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_MODEL_H
#define MEDIA_MODEL_H


#include <MediaAddOn.h>
#include <MediaNode.h>
#include <String.h>

#include <vector>

#include "Glyphs.h"


enum media_role {
	ROLE_AUDIO_OUTPUT	= 0x01,
	ROLE_AUDIO_INPUT	= 0x02,
	ROLE_VIDEO_INPUT	= 0x04,
	ROLE_VIDEO_OUTPUT	= 0x08
};


struct NodeEntry {
	dormant_node_info	info;
	uint32				roles;

	bool Is(const dormant_node_info& other) const
	{
		return info.addon == other.addon
			&& info.flavor_id == other.flavor_id;
	}
};


/*!	A device as people see it: every physical node of one name, so a
	webcam's camera and its microphone are one device.
*/
struct DeviceEntry {
	BString				name;
	std::vector<NodeEntry> nodes;
	uint32				roles;

	const NodeEntry*	NodeFor(uint32 role) const;
	BString				Key() const;
	glyph_kind			Glyph() const;
	BString				Description() const;
};


class MediaModel {
public:
								MediaModel();

			status_t			Scan();
			bool				Equals(const MediaModel& other) const;

			int32				CountDevices() const
									{ return fDevices.size(); }
			const DeviceEntry*	DeviceAt(int32 index) const;
			const DeviceEntry*	DeviceFor(const char* key) const;
			const DeviceEntry*	DeviceFor(const dormant_node_info& info)
									const;

			//! All nodes that can take \a role, in a stable order.
			void				NodesFor(uint32 role,
									std::vector<const NodeEntry*>& nodes)
									const;

			const NodeEntry*	DefaultFor(uint32 role) const;
			bool				IsDefault(const dormant_node_info& info,
									uint32 role) const;
			BString				DefaultName(uint32 role) const;
			int32				DefaultOutputChannel() const
									{ return fOutputChannel; }

	static	int					RoleIndex(uint32 role);
	static	const char*			RoleName(uint32 role);

private:
			void				_Add(media_type type, uint64 kind,
									uint32 role);

			std::vector<DeviceEntry> fDevices;
			dormant_node_info	fDefaults[4];
			bool				fHasDefault[4];
			int32				fOutputChannel;
};


#endif	// MEDIA_MODEL_H
