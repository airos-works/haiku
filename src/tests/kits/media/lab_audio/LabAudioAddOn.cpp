/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


/*!	lab_audio.media_addon: a virtual speaker (a physical audio output that
	drops what it gets) and a virtual microphone (a physical audio input
	that produces a tone), for testing whatever lists and chooses audio
	devices on a machine with only one sound card, without a kernel driver.
	Copy it into a media-add-ons directory and the media_addon_server picks
	it up at once; remove it and the devices go away again.
*/


#include <MediaAddOn.h>
#include <MediaDefs.h>

#include "LabMicrophone.h"
#include "LabSpeaker.h"


class LabAudioAddOn : public BMediaAddOn {
public:
	LabAudioAddOn(image_id image)
		:
		BMediaAddOn(image)
	{
	}

	virtual status_t InitCheck(const char** _failureText)
	{
		return B_OK;
	}

	virtual int32 CountFlavors()
	{
		return 2;
	}

	virtual status_t GetFlavorAt(int32 index, const flavor_info** _info)
	{
		if (index < 0 || index > 1)
			return B_BAD_INDEX;

		flavor_info* info = new flavor_info;
		info->internal_id = index;
		info->flavor_flags = 0;
		info->possible_count = 1;
		info->in_format_count = 0;
		info->in_formats = NULL;
		info->out_format_count = 0;
		info->out_formats = NULL;

		media_format* format = new media_format;
		format->type = B_MEDIA_RAW_AUDIO;
		format->u.raw_audio = media_raw_audio_format::wildcard;

		if (index == 0) {
			info->name = (char*)"Virtual speaker (lab)";
			info->info = (char*)"A physical audio output that goes nowhere.";
			info->kinds = B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT;
			info->in_format_count = 1;
			info->in_formats = format;
		} else {
			info->name = (char*)"Virtual microphone (lab)";
			info->info = (char*)"A physical audio input that produces a tone.";
			info->kinds = B_BUFFER_PRODUCER | B_PHYSICAL_INPUT | B_CONTROLLABLE;
			info->out_format_count = 1;
			info->out_formats = format;
		}

		*_info = info;
		return B_OK;
	}

	virtual BMediaNode* InstantiateNodeFor(const flavor_info* info,
		BMessage* config, status_t* _error)
	{
		BMediaNode* node;
		if (info->internal_id == 0)
			node = new LabSpeaker(this, 0);
		else
			node = new LabMicrophone(this);
		*_error = B_OK;
		return node;
	}

	virtual status_t GetConfigurationFor(BMediaNode* node, BMessage* message)
	{
		return B_OK;
	}
};


extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	return new LabAudioAddOn(image);
}
