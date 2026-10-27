/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaModel.h"

#include <algorithm>
#include <string.h>

#include <Catalog.h>
#include <MediaRoster.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Media Window"


static const uint32 kRoles[] = {
	ROLE_AUDIO_OUTPUT, ROLE_AUDIO_INPUT, ROLE_VIDEO_INPUT, ROLE_VIDEO_OUTPUT
};


const NodeEntry*
DeviceEntry::NodeFor(uint32 role) const
{
	for (size_t i = 0; i < nodes.size(); i++) {
		if ((nodes[i].roles & role) != 0)
			return &nodes[i];
	}
	return NULL;
}


BString
DeviceEntry::Key() const
{
	BString key("device:");
	key << name;
	return key;
}


glyph_kind
DeviceEntry::Glyph() const
{
	if ((roles & ROLE_VIDEO_INPUT) != 0)
		return GLYPH_CAMERA;
	if ((roles & ROLE_VIDEO_OUTPUT) != 0 || name.IFindFirst("HDMI") >= 0
		|| name.IFindFirst("DisplayPort") >= 0) {
		return GLYPH_DISPLAY;
	}
	if ((roles & ROLE_AUDIO_OUTPUT) != 0)
		return GLYPH_SPEAKER;
	return GLYPH_MICROPHONE;
}


BString
DeviceEntry::Description() const
{
	if ((roles & ROLE_VIDEO_INPUT) != 0) {
		if ((roles & ROLE_AUDIO_INPUT) != 0)
			return B_TRANSLATE("Camera and microphone");
		return B_TRANSLATE("Camera");
	}
	if ((roles & ROLE_VIDEO_OUTPUT) != 0)
		return B_TRANSLATE("Video output");
	if ((roles & (ROLE_AUDIO_OUTPUT | ROLE_AUDIO_INPUT))
			== (ROLE_AUDIO_OUTPUT | ROLE_AUDIO_INPUT)) {
		return B_TRANSLATE("Sound output and input");
	}
	if ((roles & ROLE_AUDIO_OUTPUT) != 0)
		return B_TRANSLATE("Sound output");
	return B_TRANSLATE("Sound input");
}


// #pragma mark - MediaModel


MediaModel::MediaModel()
	:
	fOutputChannel(-1)
{
	for (int i = 0; i < 4; i++)
		fHasDefault[i] = false;
}


int
MediaModel::RoleIndex(uint32 role)
{
	for (int i = 0; i < 4; i++) {
		if (kRoles[i] == role)
			return i;
	}
	return 0;
}


const char*
MediaModel::RoleName(uint32 role)
{
	switch (role) {
		case ROLE_AUDIO_OUTPUT:
			return B_TRANSLATE("Sound output");
		case ROLE_AUDIO_INPUT:
			return B_TRANSLATE("Sound input");
		case ROLE_VIDEO_INPUT:
			return B_TRANSLATE("Camera");
		case ROLE_VIDEO_OUTPUT:
			return B_TRANSLATE("Video output");
	}
	return "";
}


status_t
MediaModel::Scan()
{
	fDevices.clear();
	for (int i = 0; i < 4; i++)
		fHasDefault[i] = false;
	fOutputChannel = -1;

	status_t status;
	BMediaRoster* roster = BMediaRoster::Roster(&status);
	if (roster == NULL || status != B_OK)
		return status != B_OK ? status : B_ERROR;

	_Add(B_MEDIA_RAW_AUDIO, B_PHYSICAL_OUTPUT, ROLE_AUDIO_OUTPUT);
	_Add(B_MEDIA_ENCODED_AUDIO, B_PHYSICAL_OUTPUT, ROLE_AUDIO_OUTPUT);
	_Add(B_MEDIA_RAW_AUDIO, B_PHYSICAL_INPUT, ROLE_AUDIO_INPUT);
	_Add(B_MEDIA_ENCODED_AUDIO, B_PHYSICAL_INPUT, ROLE_AUDIO_INPUT);
	_Add(B_MEDIA_RAW_VIDEO, B_PHYSICAL_INPUT, ROLE_VIDEO_INPUT);
	_Add(B_MEDIA_ENCODED_VIDEO, B_PHYSICAL_INPUT, ROLE_VIDEO_INPUT);
	_Add(B_MEDIA_RAW_VIDEO, B_PHYSICAL_OUTPUT, ROLE_VIDEO_OUTPUT);
	_Add(B_MEDIA_ENCODED_VIDEO, B_PHYSICAL_OUTPUT, ROLE_VIDEO_OUTPUT);

	// A node that is a camera is not also listed as a sound output just
	// because it takes a sound track.
	for (size_t i = 0; i < fDevices.size(); i++) {
		DeviceEntry& device = fDevices[i];
		device.roles = 0;
		for (size_t j = 0; j < device.nodes.size(); j++) {
			NodeEntry& node = device.nodes[j];
			if ((node.roles & ROLE_VIDEO_INPUT) != 0)
				node.roles &= ~(uint32)ROLE_AUDIO_INPUT;
			if ((node.roles & ROLE_VIDEO_OUTPUT) != 0)
				node.roles &= ~(uint32)ROLE_AUDIO_OUTPUT;
			device.roles |= node.roles;
		}
	}

	struct Order {
		static int Rank(const DeviceEntry& device)
		{
			if ((device.roles & ROLE_AUDIO_OUTPUT) != 0)
				return 0;
			if ((device.roles & ROLE_VIDEO_INPUT) != 0)
				return 2;
			if ((device.roles & ROLE_AUDIO_INPUT) != 0)
				return 1;
			return 3;
		}

		bool operator()(const DeviceEntry& a, const DeviceEntry& b) const
		{
			int rankA = Rank(a);
			int rankB = Rank(b);
			if (rankA != rankB)
				return rankA < rankB;
			return a.name.ICompare(b.name) < 0;
		}
	};
	std::stable_sort(fDevices.begin(), fDevices.end(), Order());

	media_node node;
	dormant_node_info info;
	if (roster->GetAudioOutput(&node, &fOutputChannel, NULL) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK) {
			fDefaults[0] = info;
			fHasDefault[0] = true;
		}
		roster->ReleaseNode(node);
	}
	if (roster->GetAudioInput(&node) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK) {
			fDefaults[1] = info;
			fHasDefault[1] = true;
		}
		roster->ReleaseNode(node);
	}
	if (roster->GetVideoInput(&node) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK) {
			fDefaults[2] = info;
			fHasDefault[2] = true;
		}
		roster->ReleaseNode(node);
	}
	if (roster->GetVideoOutput(&node) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK) {
			fDefaults[3] = info;
			fHasDefault[3] = true;
		}
		roster->ReleaseNode(node);
	}

	return B_OK;
}


void
MediaModel::_Add(media_type type, uint64 kind, uint32 role)
{
	dormant_node_info infos[64];
	int32 count = 64;

	media_format format;
	format.type = type;

	// outputs are consumers, so they have an input format; inputs are
	// producers with an output format
	media_format* inputFormat = (kind & B_PHYSICAL_OUTPUT) != 0
		? &format : NULL;
	media_format* outputFormat = (kind & B_PHYSICAL_INPUT) != 0
		? &format : NULL;

	if (BMediaRoster::Roster()->GetDormantNodes(infos, &count, inputFormat,
			outputFormat, NULL, kind) != B_OK) {
		return;
	}

	for (int32 i = 0; i < count; i++) {
		const dormant_node_info& info = infos[i];

		DeviceEntry* device = NULL;
		for (size_t j = 0; j < fDevices.size() && device == NULL; j++) {
			for (size_t k = 0; k < fDevices[j].nodes.size(); k++) {
				if (fDevices[j].nodes[k].Is(info)) {
					fDevices[j].nodes[k].roles |= role;
					device = &fDevices[j];
					break;
				}
			}
		}
		if (device != NULL)
			continue;

		NodeEntry node;
		node.info = info;
		node.roles = role;

		// A camera and a microphone of one name are one device; two sound
		// cards of one name are two, "HD Audio" and "HD Audio 2".
		const uint32 kVideo = ROLE_VIDEO_INPUT | ROLE_VIDEO_OUTPUT;
		BString base(info.name);
		base.Trim();
		BString name(base);
		for (int32 number = 2; device == NULL; number++) {
			DeviceEntry* sameName = NULL;
			for (size_t j = 0; j < fDevices.size(); j++) {
				if (fDevices[j].name == name) {
					sameName = &fDevices[j];
					break;
				}
			}
			if (sameName == NULL)
				break;

			uint32 roles = 0;
			for (size_t j = 0; j < sameName->nodes.size(); j++)
				roles |= sameName->nodes[j].roles;
			if (((roles & kVideo) != 0) != ((role & kVideo) != 0)) {
				device = sameName;
				break;
			}
			name = base;
			name << " " << number;
		}
		if (device == NULL) {
			DeviceEntry entry;
			entry.name = name;
			entry.roles = 0;
			fDevices.push_back(entry);
			device = &fDevices.back();
		}
		device->nodes.push_back(node);
	}
}


bool
MediaModel::Equals(const MediaModel& other) const
{
	if (fDevices.size() != other.fDevices.size()
		|| fOutputChannel != other.fOutputChannel) {
		return false;
	}

	for (int i = 0; i < 4; i++) {
		if (fHasDefault[i] != other.fHasDefault[i])
			return false;
		if (fHasDefault[i] && (fDefaults[i].addon != other.fDefaults[i].addon
				|| fDefaults[i].flavor_id != other.fDefaults[i].flavor_id)) {
			return false;
		}
	}

	for (size_t i = 0; i < fDevices.size(); i++) {
		const DeviceEntry& a = fDevices[i];
		const DeviceEntry& b = other.fDevices[i];
		if (a.name != b.name || a.roles != b.roles
			|| a.nodes.size() != b.nodes.size()) {
			return false;
		}
		for (size_t j = 0; j < a.nodes.size(); j++) {
			if (!a.nodes[j].Is(b.nodes[j].info)
				|| a.nodes[j].roles != b.nodes[j].roles) {
				return false;
			}
		}
	}
	return true;
}


const DeviceEntry*
MediaModel::DeviceAt(int32 index) const
{
	if (index < 0 || index >= (int32)fDevices.size())
		return NULL;
	return &fDevices[index];
}


const DeviceEntry*
MediaModel::DeviceFor(const char* key) const
{
	for (size_t i = 0; i < fDevices.size(); i++) {
		if (fDevices[i].Key() == key)
			return &fDevices[i];
	}
	return NULL;
}


const DeviceEntry*
MediaModel::DeviceFor(const dormant_node_info& info) const
{
	for (size_t i = 0; i < fDevices.size(); i++) {
		for (size_t j = 0; j < fDevices[i].nodes.size(); j++) {
			if (fDevices[i].nodes[j].Is(info))
				return &fDevices[i];
		}
	}
	return NULL;
}


void
MediaModel::NodesFor(uint32 role, std::vector<const NodeEntry*>& nodes) const
{
	nodes.clear();
	for (size_t i = 0; i < fDevices.size(); i++) {
		for (size_t j = 0; j < fDevices[i].nodes.size(); j++) {
			if ((fDevices[i].nodes[j].roles & role) != 0)
				nodes.push_back(&fDevices[i].nodes[j]);
		}
	}
}


const NodeEntry*
MediaModel::DefaultFor(uint32 role) const
{
	int index = RoleIndex(role);
	if (!fHasDefault[index])
		return NULL;

	for (size_t i = 0; i < fDevices.size(); i++) {
		for (size_t j = 0; j < fDevices[i].nodes.size(); j++) {
			if (fDevices[i].nodes[j].Is(fDefaults[index]))
				return &fDevices[i].nodes[j];
		}
	}
	return NULL;
}


bool
MediaModel::IsDefault(const dormant_node_info& info, uint32 role) const
{
	int index = RoleIndex(role);
	return fHasDefault[index] && fDefaults[index].addon == info.addon
		&& fDefaults[index].flavor_id == info.flavor_id;
}


BString
MediaModel::DefaultName(uint32 role) const
{
	const NodeEntry* node = DefaultFor(role);
	if (node == NULL)
		return BString();
	const DeviceEntry* device = DeviceFor(node->info);
	if (device != NULL)
		return device->name;
	BString name(node->info.name);
	name.Trim();
	return name;
}
