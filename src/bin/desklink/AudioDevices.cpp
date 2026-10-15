/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include "AudioDevices.h"

#include <string.h>

#include <MediaNode.h>
#include <MediaRoster.h>


static const int32 kMaxNodes = 32;
static const int32 kMaxInputs = 32;


AudioDevices::AudioDevices()
{
}


status_t
AudioDevices::GetOutputs(std::vector<Device>& devices)
{
	return _GetDevices(true, devices);
}


status_t
AudioDevices::GetInputs(std::vector<Device>& devices)
{
	return _GetDevices(false, devices);
}


bool
AudioDevices::GetDefaultOutputName(BString& name)
{
	std::vector<Device> devices;
	if (_GetDevices(true, devices) != B_OK)
		return false;

	for (size_t i = 0; i < devices.size(); i++) {
		if (devices[i].isDefault) {
			name = devices[i].name;
			return true;
		}
	}
	return false;
}


bool
AudioDevices::GetDefaultInputName(BString& name)
{
	std::vector<Device> devices;
	if (_GetDevices(false, devices) != B_OK)
		return false;

	for (size_t i = 0; i < devices.size(); i++) {
		if (devices[i].isDefault) {
			name = devices[i].name;
			return true;
		}
	}
	return false;
}


/*!	Makes the node (or, with \a inputID, one of its inputs) the default audio
	output. The media server reconnects the system mixer to it, so that what
	is playing moves over.
*/
status_t
AudioDevices::SetOutput(media_node_id nodeID, int32 inputID)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL)
		return B_NO_INIT;

	media_node node;
	status_t status = roster->GetNodeFor(nodeID, &node);
	if (status != B_OK)
		return status;

	if (inputID < 0)
		status = roster->SetAudioOutput(node);
	else {
		media_input inputs[kMaxInputs];
		int32 count = 0;
		status = roster->GetAllInputsFor(node, inputs, kMaxInputs, &count);
		if (status == B_OK) {
			status = B_NAME_NOT_FOUND;
			for (int32 i = 0; i < count; i++) {
				if (inputs[i].destination.id == inputID) {
					status = roster->SetAudioOutput(inputs[i]);
					break;
				}
			}
		}
	}

	roster->ReleaseNode(node);
	return status;
}


status_t
AudioDevices::SetInput(media_node_id nodeID)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL)
		return B_NO_INIT;

	media_node node;
	status_t status = roster->GetNodeFor(nodeID, &node);
	if (status != B_OK)
		return status;

	status = roster->SetAudioInput(node);
	roster->ReleaseNode(node);
	return status;
}


/*!	Lists the live physical audio output (or input) nodes. An output node
	with several inputs (a sound card with more than one stereo pair) is
	listed once per input, as the media server can send the mixer to any of
	them.
*/
status_t
AudioDevices::_GetDevices(bool outputs, std::vector<Device>& devices)
{
	devices.clear();

	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL || !BMediaRoster::IsRunning())
		return B_NO_INIT;

	media_node_id defaultNode = -1;
	int32 defaultInputID = -1;
	{
		media_node node;
		status_t status;
		if (outputs) {
			BString inputName;
			status = roster->GetAudioOutput(&node, &defaultInputID,
				&inputName);
		} else
			status = roster->GetAudioInput(&node);
		if (status == B_OK) {
			defaultNode = node.node;
			roster->ReleaseNode(node);
		}
	}

	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;

	live_node_info info[kMaxNodes];
	int32 count = kMaxNodes;
	status_t status = roster->GetLiveNodes(info, &count,
		outputs ? &format : NULL, outputs ? NULL : &format, NULL,
		outputs ? (B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT)
			: (B_BUFFER_PRODUCER | B_PHYSICAL_INPUT));
	if (status != B_OK)
		return status;

	for (int32 i = 0; i < count; i++) {
		Device device;
		device.node = info[i].node.node;
		device.inputID = -1;
		device.name = info[i].name;
		device.isDefault = device.node == defaultNode;

		if (!outputs) {
			devices.push_back(device);
			continue;
		}

		// the node's inputs, if it has more than one for raw audio
		media_input inputs[kMaxInputs];
		int32 inputCount = 0;
		if (roster->GetAllInputsFor(info[i].node, inputs, kMaxInputs,
				&inputCount) != B_OK) {
			inputCount = 0;
		}
		int32 audioInputs = 0;
		for (int32 j = 0; j < inputCount; j++) {
			if (inputs[j].format.type == B_MEDIA_RAW_AUDIO
				|| inputs[j].format.type == B_MEDIA_UNKNOWN_TYPE) {
				audioInputs++;
			}
		}
		if (audioInputs <= 1) {
			devices.push_back(device);
			continue;
		}

		for (int32 j = 0; j < inputCount; j++) {
			if (inputs[j].format.type != B_MEDIA_RAW_AUDIO
				&& inputs[j].format.type != B_MEDIA_UNKNOWN_TYPE) {
				continue;
			}
			Device channel = device;
			channel.inputID = inputs[j].destination.id;
			channel.name << ": " << inputs[j].name;
			channel.isDefault = device.node == defaultNode
				&& channel.inputID == defaultInputID;
			devices.push_back(channel);
		}
	}

	return B_OK;
}
