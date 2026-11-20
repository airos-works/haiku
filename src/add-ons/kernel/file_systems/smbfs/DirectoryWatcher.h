/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SMBFS_DIRECTORY_WATCHER_H
#define SMBFS_DIRECTORY_WATCHER_H


#include <pthread.h>

#include <deque>
#include <list>
#include <string>
#include <vector>

#include <OS.h>

#include "Connection.h"


/*!	Hears from the server when something changes in the directories that
	were read last, the way Windows Explorer does: SMB2 CHANGE_NOTIFY. The
	server answers such a request when there is something to tell, and not
	before, so nothing is done while nothing changes.

	This has a session of its own, and a thread that waits on it, as the
	answers come whenever they come and the sessions of the pool are only
	listened to while somebody waits for an answer there.
*/
class DirectoryWatcher {
public:
			enum {
				kEntryCreated = 1,
				kEntryRemoved,
				kEntryChanged,
				kDirectoryChanged
					// anything may have changed in it
			};

			typedef void (*ChangeHook)(int what, const std::string& directory,
				const std::string& serverDirectory, const std::string& name);

								DirectoryWatcher();
								~DirectoryWatcher();

			void				Init(const Options* options, ChangeHook hook);
			void				Shutdown();

			void				WatchDirectory(const std::string& directory,
									const std::string& serverDirectory);
									// the directory as the volume has it, and
									// as the server does

private:
			struct Watch;
			struct Event {
				int				what;
				std::string		directory;
				std::string		serverDirectory;
				std::string		name;
			};
			struct Command {
				std::string		directory;
				std::string		serverDirectory;
			};

	static	status_t			_ThreadEntry(void* data);
			void				_Thread();

			void				_TakeCommands(std::deque<Command>& commands);
			Watch*				_FindWatch(const std::string& directory);
			void				_Prune();
			bool				_Connect();
			void				_Disconnect();

			void				_Open(Watch* watch);
			void				_Arm(Watch* watch);
			void				_Close(Watch* watch);

	static	void				_Opened(struct smb2_context* context,
									int status, void* data, void* cookie);
	static	void				_Notified(struct smb2_context* context,
									int status, void* data, void* cookie);
	static	void				_Closed(struct smb2_context* context,
									int status, void* data, void* cookie);

			void				_AddEvents(Watch* watch, const uint8* buffer,
									uint32 length);
			void				_AddEvent(int what, Watch* watch,
									const std::string& name);
			void				_DeliverEvents();

			bool				_IsLost(int status) const;

private:
			const Options*		fOptions;
			ChangeHook			fHook;
			Connection			fConnection;
			thread_id			fThread;

			pthread_mutex_t		fLock;
			std::deque<Command>	fCommands;
			bool				fQuitting;
			int					fWakePipe[2];
			int					fQuitPipe[2];

			// only the thread uses these
			std::list<Watch*>	fWatches;
				// the latest wish first
			std::vector<Event>	fEvents;
			bigtime_t			fFirstEventTime;
			bool				fLost;
			bigtime_t			fNextConnect;
			bigtime_t			fConnectDelay;
			int					fBusy;
				// requests that are not CHANGE_NOTIFY on their way
};


#endif	// SMBFS_DIRECTORY_WATCHER_H
