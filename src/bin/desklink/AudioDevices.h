/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */
#ifndef AUDIO_DEVICES_H
#define AUDIO_DEVICES_H


#include <vector>

#include <MediaDefs.h>
#include <String.h>


/*!	The physical audio inputs and outputs the media server knows (sound
	cards, HDMI audio, USB headsets and webcam microphones), and which of
	them are the defaults that applications use.
*/
class AudioDevices {
public:
			struct Device {
				media_node_id	node;
				int32			inputID;
					// destination ID of the node's input to use, or -1 for
					// the node as a whole
				BString			name;
				bool			isDefault;
			};

								AudioDevices();

			status_t			GetOutputs(std::vector<Device>& devices);
			status_t			GetInputs(std::vector<Device>& devices);

			//! The names of the default output and input, if any
			bool				GetDefaultOutputName(BString& name);
			bool				GetDefaultInputName(BString& name);

			status_t			SetOutput(media_node_id node, int32 inputID);
			status_t			SetInput(media_node_id node);

private:
			status_t			_GetDevices(bool outputs,
									std::vector<Device>& devices);
};


#endif	// AUDIO_DEVICES_H
