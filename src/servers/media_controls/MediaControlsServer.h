/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_CONTROLS_SERVER_H
#define MEDIA_CONTROLS_SERVER_H


#include <Server.h>

#include <ObjectList.h>
#include <String.h>

#include <MediaControls.h>


class BLooper;
class BMessageRunner;


struct Player {
			int64				id;
			team_id				team;
			BMessenger			target;
			BString				name;
			BString				signature;

			BMessage			metadata;
			BMessage			playback;
			uint32				actions;
			media_session_state	state;

			// for choosing the active player
			int64				registerSequence;
			int64				playStartSequence;
			int64				lastPlayedSequence;
};


class MediaControlsServer : public BServer {
public:
								MediaControlsServer(status_t& error);
	virtual						~MediaControlsServer();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_Register(BMessage* message);
			void				_Update(BMessage* message);
			void				_Unregister(BMessage* message);
			void				_SendAction(BMessage* message);
			void				_TeamQuit(team_id team);

			bool				_ApplyUpdate(Player* player,
									const BMessage* message);
			Player*				_FindPlayer(int64 id) const;
			Player*				_ActivePlayer() const;
			void				_RemovePlayer(Player* player);

			void				_GetState(BMessage& state) const;
			void				_AddPlayer(BMessage& state,
									const Player* player) const;
			void				_StateChanged();
			void				_UpdateReplicant();

private:
			BObjectList<Player, true> fPlayers;
			BObjectList<BMessenger, true> fWatchers;
			int64				fSequence;
			int64				fPinnedID;

			BLooper*			fDeskbarLooper;
			BMessageRunner*		fRemoveRunner;
			bool				fReplicantWanted;
};


#endif	// MEDIA_CONTROLS_SERVER_H
