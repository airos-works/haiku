/*
 * Copyright 2010, Haiku. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Marcus Overhagen
 *		Jérôme Duval
 */
/*
 * Copyright 2002, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DEFAULT_MANAGER_H
#define _DEFAULT_MANAGER_H


/*!	Manager for defaults (audio and video, input and output)
*/


#include "DataExchange.h"

#include <Autolock.h>
#include <MediaRoster.h>
#include <Message.h>

class NodeManager;


class DefaultManager {
public:
								DefaultManager();
								~DefaultManager();

			status_t 			LoadState();
			status_t			SaveState();
			status_t			RememberDefault(NodeManager *node_manager,
									node_type type);

			status_t			Set(media_node_id nodeid,
									const char *input_name, int32 input_id,
									node_type type);
			status_t			Get(media_node_id *nodeid, char *input_name,
									int32 *input_id, node_type type);
			status_t			Rescan();

			void				Dump();

			void 				CleanupTeam(team_id team);
			void				NodeUnregistered(media_node_id node);

private:
			static int32		rescan_thread(void *arg);
			void				_RescanThread();

			void				_ChoosePhysical(volatile media_node_id *id,
									volatile bool *standIn,
									uint32 default_type, bool isInput,
									bool isVideo);
			bool				_FindPhysical(volatile media_node_id *id,
									uint32 default_type, bool isInput,
									media_type type, bool savedOnly);
			void				_FindAudioMixer();
			void				_FindTimeSource();

			status_t			_ConnectMixerToOutput();

private:
			volatile bool 		fMixerConnected;
			volatile media_node_id fPhysicalVideoOut;
			volatile media_node_id fPhysicalVideoIn;
			volatile media_node_id fPhysicalAudioOut;
			volatile media_node_id fPhysicalAudioIn;
			volatile media_node_id fSystemTimeSource;
			volatile media_node_id fTimeSource;
			volatile media_node_id fAudioMixer;
			volatile int32 		fPhysicalAudioOutInputID;
			char fPhysicalAudioOutInputName[B_MEDIA_NAME_LENGTH];

			// whether the default of a type only stands in for the saved
			// one, which is not there (yet)
			volatile bool		fVideoOutStandIn;
			volatile bool		fVideoInStandIn;
			volatile bool		fAudioOutStandIn;
			volatile bool		fAudioInStandIn;

			media_output 		fMixerToPhysicalAudioOut;
			media_input 		fPhysicalAudioOutFromMixer;

			BList				fMsgList;
				// the saved defaults: those the user chose
			BLocker				fSettingsLock;

			uint32				fBeginHeader[3];
			uint32				fEndHeader[3];
			thread_id			fRescanThread;
			int32 				fRescanRequested;
			BLocker				fRescanLock;
			BMediaRoster*		fRoster;
};

#endif // _DEFAULT_MANAGER_H
