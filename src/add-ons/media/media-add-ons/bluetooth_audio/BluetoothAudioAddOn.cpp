/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "BluetoothAudioAddOn.h"

#include <new>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <Autolock.h>
#include <Entry.h>
#include <Looper.h>
#include <MediaRoster.h>
#include <NodeMonitor.h>
#include <Path.h>

#include <bluetooth/bdaddrUtils.h>

#include <A2dpSource.h>
#include <DormantNodeManager.h>

#include "BluetoothAudioNode.h"


using namespace Bluetooth;


/*!	Watches the directory of the settings file (it may not exist yet, and
	writers replace it) and tells the add-on when it changes.
*/
// Makes the speaker the system's audio output, from the watcher's thread:
// the media roster calls wait for the node to show up.
static const uint32 kMsgMakeDefaultOutput = 'btdo';


class SettingsWatcher : public BLooper {
public:
	SettingsWatcher(BluetoothAudioAddOn* addOn)
		:
		BLooper("bluetooth audio settings"),
		fAddOn(addOn)
	{
	}

	status_t Watch()
	{
		BPath path;
		status_t status = GetAudioSinkSettingsPath(path);
		if (status != B_OK)
			return status;
		fName = path.Leaf();
		BPath directory;
		path.GetParent(&directory);
		BEntry entry(directory.Path());
		node_ref nodeRef;
		status = entry.GetNodeRef(&nodeRef);
		if (status != B_OK)
			return status;
		status = watch_node(&nodeRef, B_WATCH_DIRECTORY, this);
		if (status != B_OK)
			return status;

		// The file itself, for changes in place.
		BEntry file(path.Path());
		if (file.GetNodeRef(&fFileRef) == B_OK)
			watch_node(&fFileRef, B_WATCH_STAT, this);
		return B_OK;
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kMsgMakeDefaultOutput) {
			fAddOn->MakeDefaultOutput();
			return;
		}
		if (message->what != B_NODE_MONITOR) {
			BLooper::MessageReceived(message);
			return;
		}

		const char* name;
		int32 opcode = message->GetInt32("opcode", -1);
		if ((opcode == B_ENTRY_CREATED || opcode == B_ENTRY_REMOVED
				|| opcode == B_ENTRY_MOVED)
			&& (message->FindString("name", &name) != B_OK
				|| fName != name)) {
			return;
		}

		if (opcode == B_ENTRY_CREATED || opcode == B_ENTRY_MOVED) {
			BPath path;
			if (GetAudioSinkSettingsPath(path) == B_OK) {
				watch_node(&fFileRef, B_STOP_WATCHING, this);
				BEntry file(path.Path());
				if (file.GetNodeRef(&fFileRef) == B_OK)
					watch_node(&fFileRef, B_WATCH_STAT, this);
			}
		}
		fAddOn->SettingsChanged();
	}

private:
	BluetoothAudioAddOn*	fAddOn;
	BString					fName;
	node_ref				fFileRef;
};


extern "C" _EXPORT BMediaAddOn*
make_media_addon(image_id image)
{
	return new(std::nothrow) BluetoothAudioAddOn(image);
}


BluetoothAudioAddOn::BluetoothAudioAddOn(image_id image)
	:
	BMediaAddOn(image),
	fLock("bluetooth audio add-on"),
	fHasSink(false),
	fWatcher(NULL),
	fNode(NULL),
	fPreviousOutput(media_node::null),
	fStarted(false),
	fPinned(false),
	fConnectOnCreate(false)
{
	memset(&fSink, 0, sizeof(fSink));
	BluetoothAudioNode::GetFormat(fFormat);

	fFlavor.name = (char*)"";
	fFlavor.info = (char*)"Plays to a Bluetooth speaker or headset (A2DP)";
	fFlavor.kinds = B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT;
	fFlavor.flavor_flags = 0;
	fFlavor.internal_id = 0;
	fFlavor.possible_count = 1;
	fFlavor.in_format_count = 1;
	fFlavor.in_formats = &fFormat;
	fFlavor.out_format_count = 0;
	fFlavor.out_formats = NULL;

	SettingsChanged();
	fStarted = true;

	fWatcher = new(std::nothrow) SettingsWatcher(this);
	if (fWatcher != NULL) {
		fWatcher->Run();
		BAutolock _(fWatcher);
		fWatcher->Watch();
		// The media server does not bring a Bluetooth output back as the
		// default after a restart (the node comes later than its check),
		// and without one the mixer plays to nothing.
		if (fHasSink)
			fWatcher->PostMessage(kMsgMakeDefaultOutput);
	}
}


BluetoothAudioAddOn::~BluetoothAudioAddOn()
{
	if (fWatcher != NULL && fWatcher->Lock()) {
		stop_watching(fWatcher);
		fWatcher->Quit();
	}
}


status_t
BluetoothAudioAddOn::InitCheck(const char** _failureText)
{
	return B_OK;
}


bool
BluetoothAudioAddOn::_ReadSettings(bdaddr_t& address, BString& name)
{
	if (GetAudioSink(address, name) != B_OK)
		return false;
	if (name.IsEmpty())
		name = bdaddrUtils::ToString(address);
	return true;
}


void
BluetoothAudioAddOn::SettingsChanged()
{
	bdaddr_t address;
	BString name;
	const bool hasSink = _ReadSettings(address, name);

	bool hadSink;
	{
		BAutolock _(fLock);
		BString flavorName;
		if (hasSink)
			flavorName.SetToFormat("Bluetooth: %s", name.String());
		const bool sameDevice = hasSink == fHasSink
			&& (!hasSink || bdaddrUtils::Compare(address, fSink));
		if (sameDevice && flavorName == fFlavorName) {
			// The same speaker written again: the user connected it in the
			// Bluetooth preferences, and expects it to be connected now,
			// not when sound next plays.
			if (hasSink && fNode != NULL)
				fNode->ConnectNow();
			if (hasSink && fWatcher != NULL)
				fWatcher->PostMessage(kMsgMakeDefaultOutput);
			return;
		}
		hadSink = fHasSink;
		fHasSink = hasSink;
		// A speaker just chosen is connected at once, so that it shows it
		// is in use; one remembered from before (at boot) waits for sound.
		fConnectOnCreate = hasSink && fStarted;
		fSink = address;
		fFlavorName = flavorName;
		fFlavor.name = (char*)fFlavorName.String();
		if (!sameDevice && fNode != NULL)
			fNode->SinkChanged();
	}

	if (!fStarted)
		return;

	// Sound follows the speaker: switch the system output to it when one
	// is chosen, and back when it is given up.
	if (hadSink && !hasSink)
		_RestoreOutput();
	NotifyFlavorChange();
	if (hasSink && !hadSink)
		_MakeDefaultOutput();
}


void
BluetoothAudioAddOn::NodeDeleted(BluetoothAudioNode* node)
{
	BAutolock _(fLock);
	if (fNode == node)
		fNode = NULL;
}


void
BluetoothAudioAddOn::MakeDefaultOutput()
{
	{
		BAutolock _(fLock);
		if (!fHasSink)
			return;
	}
	_MakeDefaultOutput();
}


void
BluetoothAudioAddOn::_MakeDefaultOutput()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL)
		return;

	media_node current;
	if (roster->GetAudioOutput(&current) == B_OK) {
		BAutolock _(fLock);
		if (fNode != NULL && current.node == fNode->ID())
			return;
		fPreviousOutput = current;
	}

	dormant_node_info info;
	info.addon = AddonID();
	info.flavor_id = 0;
	strlcpy(info.name, fFlavorName.String(), sizeof(info.name));

	// The media add-on server creates the node a moment after the flavor
	// shows up (at boot, after the media server is up).
	status_t status = B_ERROR;
	for (int32 tries = 0; tries < 150 && status != B_OK; tries++) {
		status = roster->SetAudioOutput(info);
		if (status != B_OK)
			snooze(100000);
	}
	if (status != B_OK) {
		syslog(LOG_ERR, "bluetooth audio: could not become the audio output: "
			"%s\n", strerror(status));
	} else
		syslog(LOG_INFO, "bluetooth audio: %s is the audio output\n",
			fFlavorName.String());
}


void
BluetoothAudioAddOn::_RestoreOutput()
{
	media_node previous;
	{
		BAutolock _(fLock);
		previous = fPreviousOutput;
		fPreviousOutput = media_node::null;
	}
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster != NULL && previous != media_node::null)
		roster->SetAudioOutput(previous);
}


int32
BluetoothAudioAddOn::CountFlavors()
{
	// Without a speaker we have no flavor and no node, and the media
	// add-on server would unload us; keep a reference to ourselves, so that
	// we are still here to notice when one is chosen.
	if (!fPinned && AddonID() > 0 && gDormantNodeManager != NULL) {
		fPinned = true;
		gDormantNodeManager->GetAddOn(AddonID());
	}

	BAutolock _(fLock);
	return fHasSink ? 1 : 0;
}


status_t
BluetoothAudioAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	BAutolock _(fLock);
	if (!fHasSink || index != 0)
		return B_BAD_INDEX;
	*_info = &fFlavor;
	return B_OK;
}


BMediaNode*
BluetoothAudioAddOn::InstantiateNodeFor(const flavor_info* info,
	BMessage* config, status_t* _error)
{
	BluetoothAudioNode* node = new(std::nothrow) BluetoothAudioNode(this,
		"Bluetooth audio");
	if (node == NULL) {
		*_error = B_NO_MEMORY;
		return NULL;
	}
	{
		BAutolock _(fLock);
		fNode = node;
		if (fConnectOnCreate)
			node->ConnectNow();
		fConnectOnCreate = false;
	}
	*_error = B_OK;
	return node;
}


status_t
BluetoothAudioAddOn::GetConfigurationFor(BMediaNode* node, BMessage* message)
{
	return B_OK;
}


bool
BluetoothAudioAddOn::WantsAutoStart()
{
	return false;
}


status_t
BluetoothAudioAddOn::AutoStart(int index, BMediaNode** _node,
	int32* _internalID, bool* _hasMore)
{
	return B_ERROR;
}
