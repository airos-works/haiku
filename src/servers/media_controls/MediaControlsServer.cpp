/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	media_controls_server: the system's "now playing" service. Players
	register (see <MediaControls.h>), the server keeps one of them active,
	routes actions (media keys, the Deskbar replicant) to it, and shows its
	replicant in the Deskbar while there is a player.
*/


#include "MediaControlsServer.h"

#include <stdio.h>
#include <string.h>

#include <Deskbar.h>
#include <Entry.h>
#include <Looper.h>
#include <MessageRunner.h>
#include <Roster.h>


//#define TRACE_MEDIA_CONTROLS
#ifdef TRACE_MEDIA_CONTROLS
#	define TRACE(x...) printf("media_controls_server: " x)
#else
#	define TRACE(x...) ;
#endif


static const bigtime_t kSendTimeout = 100000;
static const bigtime_t kReplicantRemoveDelay = 1500000;

enum {
	kMsgSyncReplicant	= 'syRp',
	kMsgRemoveReplicant	= 'rmRp'
};


/*!	Talks to the Deskbar in its own thread: the Deskbar's replicant talks
	to the server, and the server must not wait for the Deskbar meanwhile.
*/
class DeskbarLooper : public BLooper {
public:
	DeskbarLooper()
		:
		BLooper("deskbar")
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kMsgSyncReplicant) {
			BLooper::MessageReceived(message);
			return;
		}

		bool show = message->GetBool("show", false);
		BDeskbar deskbar;
		if (!deskbar.IsRunning())
			return;

		bool installed = deskbar.HasItem(kMediaControlsReplicantName);
		if (show && !installed) {
			app_info info;
			if (be_app->GetAppInfo(&info) != B_OK)
				return;
			status_t status = deskbar.AddItem(&info.ref);
			TRACE("add replicant: %s\n", strerror(status));
			if (status != B_OK) {
				fprintf(stderr, "media_controls_server: cannot add the "
					"Deskbar replicant: %s\n", strerror(status));
			}
		} else if (!show && installed) {
			TRACE("remove replicant\n");
			deskbar.RemoveItem(kMediaControlsReplicantName);
		}
	}
};


//	#pragma mark -


MediaControlsServer::MediaControlsServer(status_t& error)
	:
	BServer(kMediaControlsServerSignature, false, &error),
	fSequence(0),
	fPinnedID(-1),
	fDeskbarLooper(NULL),
	fRemoveRunner(NULL),
	fReplicantWanted(false)
{
	if (error != B_OK)
		return;

	be_roster->StartWatching(be_app_messenger, B_REQUEST_QUIT);

	fDeskbarLooper = new DeskbarLooper();
	fDeskbarLooper->Run();

	// A replicant left over from before (the Deskbar keeps it across
	// reboots) goes away when no player registers in time.
	_UpdateReplicant();
}


MediaControlsServer::~MediaControlsServer()
{
	be_roster->StopWatching(be_app_messenger);
	delete fRemoveRunner;
}


bool
MediaControlsServer::QuitRequested()
{
	if (fDeskbarLooper != NULL && fDeskbarLooper->Lock())
		fDeskbarLooper->Quit();
	fDeskbarLooper = NULL;

	return BServer::QuitRequested();
}


void
MediaControlsServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgMediaSessionRegister:
			_Register(message);
			break;
		case kMsgMediaSessionUpdate:
			_Update(message);
			break;
		case kMsgMediaSessionUnregister:
			_Unregister(message);
			break;

		case kMsgMediaControlsSendAction:
			_SendAction(message);
			break;

		case kMsgMediaControlsGetState:
		{
			BMessage state(kMsgMediaControlsState);
			_GetState(state);
			message->SendReply(&state);
			break;
		}

		case kMsgMediaControlsStartWatching:
		{
			BMessenger target;
			if (message->FindMessenger("target", &target) != B_OK
				|| !target.IsValid())
				break;
			for (int32 i = 0; i < fWatchers.CountItems(); i++) {
				if (*fWatchers.ItemAt(i) == target)
					return;
			}
			fWatchers.AddItem(new BMessenger(target));

			BMessage state(kMsgMediaControlsState);
			_GetState(state);
			target.SendMessage(&state, (BHandler*)NULL, kSendTimeout);
			break;
		}

		case kMsgMediaControlsStopWatching:
		{
			BMessenger target;
			if (message->FindMessenger("target", &target) != B_OK)
				break;
			for (int32 i = fWatchers.CountItems() - 1; i >= 0; i--) {
				if (*fWatchers.ItemAt(i) == target)
					delete fWatchers.RemoveItemAt(i);
			}
			break;
		}

		case kMsgMediaControlsActivate:
		{
			Player* player = _FindPlayer(message->GetInt64("session", -1));
			if (player != NULL && player != _ActivePlayer()) {
				fPinnedID = player->id;
				_StateChanged();
			}
			break;
		}

		case B_SOME_APP_QUIT:
		{
			team_id team;
			if (message->FindInt32("be:team", &team) == B_OK)
				_TeamQuit(team);
			break;
		}

		case kMsgRemoveReplicant:
			delete fRemoveRunner;
			fRemoveRunner = NULL;
			if (fPlayers.IsEmpty() && fDeskbarLooper != NULL) {
				fReplicantWanted = false;
				BMessage sync(kMsgSyncReplicant);
				sync.AddBool("show", false);
				fDeskbarLooper->PostMessage(&sync);
			}
			break;

		default:
			BServer::MessageReceived(message);
			break;
	}
}


void
MediaControlsServer::_Register(BMessage* message)
{
	int64 id = message->GetInt64("session", -1);
	BMessenger target;
	if (id < 0 || message->FindMessenger("target", &target) != B_OK
		|| !target.IsValid())
		return;

	Player* player = _FindPlayer(id);
	if (player == NULL) {
		player = new Player;
		player->id = id;
		player->actions = B_MEDIA_DEFAULT_ACTIONS;
		player->state = B_MEDIA_SESSION_STOPPED;
		player->registerSequence = ++fSequence;
		player->playStartSequence = 0;
		player->lastPlayedSequence = 0;
		fPlayers.AddItem(player);
	}

	player->target = target;
	player->team = target.Team();

	app_info info;
	if (be_roster->GetRunningAppInfo(player->team, &info) == B_OK) {
		player->signature = info.signature;
		player->name = info.ref.name;
	}
	const char* name;
	if (message->FindString("name", &name) == B_OK && name[0] != '\0')
		player->name = name;

	TRACE("register %" B_PRIx64 " (%s, team %" B_PRId32 ")\n", id,
		player->name.String(), player->team);

	_ApplyUpdate(player, message);
	_StateChanged();
	_UpdateReplicant();
}


void
MediaControlsServer::_Update(BMessage* message)
{
	Player* player = _FindPlayer(message->GetInt64("session", -1));
	if (player == NULL)
		return;

	if (_ApplyUpdate(player, message))
		_StateChanged();
}


void
MediaControlsServer::_Unregister(BMessage* message)
{
	Player* player = _FindPlayer(message->GetInt64("session", -1));
	if (player == NULL)
		return;

	TRACE("unregister %" B_PRIx64 "\n", player->id);
	_RemovePlayer(player);
}


void
MediaControlsServer::_SendAction(BMessage* message)
{
	int32 action = message->GetInt32("action", -1);
	int64 id = message->GetInt64("session", -1);
	Player* player = id >= 0 ? _FindPlayer(id) : _ActivePlayer();

	bool handled = false;
	if (player != NULL && action >= 0 && action < B_MEDIA_ACTION_COUNT) {
		// Players need not take all of play, pause and toggle.
		uint32 actions = player->actions;
		bool playing = player->state == B_MEDIA_SESSION_PLAYING;
		if (action == B_MEDIA_ACTION_TOGGLE
			&& (actions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_TOGGLE)) == 0) {
			action = playing ? B_MEDIA_ACTION_PAUSE : B_MEDIA_ACTION_PLAY;
		} else if ((action == B_MEDIA_ACTION_PLAY && !playing)
				|| (action == B_MEDIA_ACTION_PAUSE && playing)) {
			if ((actions & B_MEDIA_ACTION_FLAG(action)) == 0)
				action = B_MEDIA_ACTION_TOGGLE;
		}

		bigtime_t position = message->GetInt64("position", -1);
		if ((actions & B_MEDIA_ACTION_FLAG(action)) != 0
			&& (action != B_MEDIA_ACTION_SEEK || position >= 0)) {
			BMessage forward(kMsgMediaSessionAction);
			forward.AddInt64("session", player->id);
			forward.AddInt32("action", action);
			if (action == B_MEDIA_ACTION_SEEK)
				forward.AddInt64("position", position);

			status_t status = player->target.SendMessage(&forward,
				(BHandler*)NULL, kSendTimeout);
			TRACE("action %s -> %" B_PRIx64 ": %s\n",
				media_session_action_name(action), player->id,
				strerror(status));
			if (status == B_OK)
				handled = true;
			else if (status == B_BAD_PORT_ID) {
				// gone without saying good bye
				_RemovePlayer(player);
				player = NULL;
			}
		}
	}

	BMessage reply(kMsgMediaControlsReply);
	reply.AddBool("handled", handled);
	if (player != NULL)
		reply.AddInt64("session", player->id);
	message->SendReply(&reply);
}


void
MediaControlsServer::_TeamQuit(team_id team)
{
	bool changed = false;
	for (int32 i = fPlayers.CountItems() - 1; i >= 0; i--) {
		Player* player = fPlayers.ItemAt(i);
		if (player->team != team)
			continue;
		TRACE("team %" B_PRId32 " quit: drop %" B_PRIx64 "\n", team,
			player->id);
		if (fPinnedID == player->id)
			fPinnedID = -1;
		fPlayers.RemoveItemAt(i);
		delete player;
		changed = true;
	}

	for (int32 i = fWatchers.CountItems() - 1; i >= 0; i--) {
		if (fWatchers.ItemAt(i)->Team() == team)
			delete fWatchers.RemoveItemAt(i);
	}

	if (changed) {
		_StateChanged();
		_UpdateReplicant();
	}
}


/*!	Merges the field groups of an update (see kMsgMediaSessionUpdate)
	into the player. Returns whether anything was there.
*/
bool
MediaControlsServer::_ApplyUpdate(Player* player, const BMessage* message)
{
	bool changed = false;

	if (message->HasString("title")) {
		player->metadata.MakeEmpty();
		static const char* kStrings[] = { "title", "artist", "album" };
		for (size_t i = 0; i < B_COUNT_OF(kStrings); i++) {
			const char* value;
			if (message->FindString(kStrings[i], &value) == B_OK)
				player->metadata.AddString(kStrings[i], value);
		}
		BMessage artwork;
		if (message->FindMessage("artwork", &artwork) == B_OK)
			player->metadata.AddMessage("artwork", &artwork);
		changed = true;
	}

	int32 state;
	if (message->FindInt32("state", &state) == B_OK) {
		media_session_state oldState = player->state;
		if (state < B_MEDIA_SESSION_STOPPED || state > B_MEDIA_SESSION_PAUSED)
			state = B_MEDIA_SESSION_STOPPED;
		player->state = (media_session_state)state;

		player->playback.MakeEmpty();
		player->playback.AddInt32("state", state);
		int64 value;
		if (message->FindInt64("position", &value) == B_OK) {
			player->playback.AddInt64("position", value);
			player->playback.AddInt64("position_time",
				message->GetInt64("position_time", system_time()));
		}
		if (message->FindInt64("duration", &value) == B_OK && value > 0)
			player->playback.AddInt64("duration", value);
		player->playback.AddFloat("rate", message->GetFloat("rate", 1.0f));

		bool playing = state == B_MEDIA_SESSION_PLAYING;
		bool wasPlaying = oldState == B_MEDIA_SESSION_PLAYING;
		if (playing != wasPlaying) {
			// The player that started playing last is the active one; one
			// that stopped playing last is next in line.
			player->lastPlayedSequence = ++fSequence;
			if (playing) {
				player->playStartSequence = fSequence;
				if (fPinnedID != player->id)
					fPinnedID = -1;
			}
		}
		changed = true;
	}

	uint32 actions;
	if (message->FindUInt32("actions", &actions) == B_OK) {
		player->actions = actions;
		changed = true;
	}

	return changed;
}


Player*
MediaControlsServer::_FindPlayer(int64 id) const
{
	if (id < 0)
		return NULL;

	for (int32 i = 0; i < fPlayers.CountItems(); i++) {
		Player* player = fPlayers.ItemAt(i);
		if (player->id == id)
			return player;
	}
	return NULL;
}


/*!	The player chosen by the user, else the one that started playing last
	among those playing, else the one that played last, else the one that
	registered last.
*/
Player*
MediaControlsServer::_ActivePlayer() const
{
	Player* pinned = _FindPlayer(fPinnedID);
	if (pinned != NULL)
		return pinned;

	Player* best = NULL;
	for (int32 i = 0; i < fPlayers.CountItems(); i++) {
		Player* player = fPlayers.ItemAt(i);
		if (player->state == B_MEDIA_SESSION_PLAYING
			&& (best == NULL
				|| player->playStartSequence > best->playStartSequence))
			best = player;
	}
	if (best != NULL)
		return best;

	for (int32 i = 0; i < fPlayers.CountItems(); i++) {
		Player* player = fPlayers.ItemAt(i);
		if (player->lastPlayedSequence > 0 && (best == NULL
				|| player->lastPlayedSequence > best->lastPlayedSequence))
			best = player;
	}
	if (best != NULL)
		return best;

	for (int32 i = 0; i < fPlayers.CountItems(); i++) {
		Player* player = fPlayers.ItemAt(i);
		if (best == NULL || player->registerSequence > best->registerSequence)
			best = player;
	}
	return best;
}


void
MediaControlsServer::_RemovePlayer(Player* player)
{
	if (fPinnedID == player->id)
		fPinnedID = -1;
	fPlayers.RemoveItem(player);
	delete player;

	_StateChanged();
	_UpdateReplicant();
}


void
MediaControlsServer::_GetState(BMessage& state) const
{
	Player* active = _ActivePlayer();
	if (active == NULL)
		return;

	state.AddInt64("active", active->id);
	_AddPlayer(state, active);

	// the others, the most recent first
	BObjectList<Player> others(fPlayers.CountItems());
	for (int32 i = 0; i < fPlayers.CountItems(); i++) {
		if (fPlayers.ItemAt(i) != active)
			others.AddItem(fPlayers.ItemAt(i));
	}
	others.SortItems([](const Player* a, const Player* b) -> int {
		int64 aSequence = a->lastPlayedSequence > 0
			? a->lastPlayedSequence : a->registerSequence;
		int64 bSequence = b->lastPlayedSequence > 0
			? b->lastPlayedSequence : b->registerSequence;
		return aSequence > bSequence ? -1 : aSequence < bSequence ? 1 : 0;
	});
	for (int32 i = 0; i < others.CountItems(); i++)
		_AddPlayer(state, others.ItemAt(i));
}


void
MediaControlsServer::_AddPlayer(BMessage& state, const Player* player) const
{
	BMessage info(player->metadata);
	info.AddInt64("session", player->id);
	info.AddString("name", player->name);
	info.AddString("signature", player->signature);
	info.AddInt32("team", player->team);
	info.AddUInt32("actions", player->actions);

	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; player->playback.GetInfo(B_ANY_TYPE, i, &name, &type,
			&count) == B_OK; i++) {
		const void* data;
		ssize_t size;
		if (player->playback.FindData(name, type, &data, &size) == B_OK)
			info.AddData(name, type, data, size);
	}
	if (!info.HasString("title"))
		info.AddString("title", "");

	state.AddMessage("player", &info);
}


void
MediaControlsServer::_StateChanged()
{
	if (fWatchers.IsEmpty())
		return;

	BMessage state(kMsgMediaControlsState);
	_GetState(state);

	for (int32 i = fWatchers.CountItems() - 1; i >= 0; i--) {
		status_t status = fWatchers.ItemAt(i)->SendMessage(&state,
			(BHandler*)NULL, kSendTimeout);
		if (status == B_BAD_PORT_ID)
			delete fWatchers.RemoveItemAt(i);
	}
}


/*!	The replicant is there while there is a player; it goes a moment after
	the last one, so that a player that registers again right away does not
	make it flicker.
*/
void
MediaControlsServer::_UpdateReplicant()
{
	if (fDeskbarLooper == NULL)
		return;

	if (!fPlayers.IsEmpty()) {
		delete fRemoveRunner;
		fRemoveRunner = NULL;
		if (!fReplicantWanted) {
			fReplicantWanted = true;
			BMessage sync(kMsgSyncReplicant);
			sync.AddBool("show", true);
			fDeskbarLooper->PostMessage(&sync);
		}
	} else if (fRemoveRunner == NULL) {
		BMessage remove(kMsgRemoveReplicant);
		fRemoveRunner = new BMessageRunner(be_app_messenger, &remove,
			kReplicantRemoveDelay, 1);
	}
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	status_t error;
	MediaControlsServer server(error);
	if (error != B_OK)
		return 1;

	server.Run();
	return 0;
}
