/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERM_FILE_TRANSFER_H
#define TERM_FILE_TRANSFER_H


#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <OS.h>


class BHandler;
struct z_stream_s;


//! The answer of the alert that asks the user ("id", "which").
static const uint32 kFileTransferAnswer = 'ftAn';
//! Sends more of the files a program asked for.
static const uint32 kFileTransferPump = 'ftPm';


/*!	kitty's file transfer protocol (OSC 5113) for one terminal: a program
	sends files to this computer (a send session) or gets files from it (a
	receive session), each session once the user allowed it (or with the
	password of the settings' "FileTransferPassword"). Files, directories,
	symbolic and hard links, with their times and permissions, zlib
	compressed or not; rsync deltas are not supported. Runs on the window
	thread; answers go to the program through the writer, and files sent
	to the program go in batches while the writer's backlog is small.
*/
class TermFileTransfer {
public:
	typedef std::function<void(const std::string&)> Writer;
	typedef std::function<size_t()> Backlog;

								TermFileTransfer(BHandler* target,
									Writer writer, Backlog backlog);
								~TermFileTransfer();

			//! An OSC 5113 command from the program (what follows "5113;").
			void				Command(const std::string& data);
			//! The user answered the question for session \a id.
			void				Answer(const char* id, bool allowed);
			//! A kFileTransferPump message arrived.
			void				Pump();

private:
			typedef std::map<std::string, std::string> Fields;

			struct File {
								File();

				std::string		id;
				std::string		path;
				std::string		temporaryPath;
				std::string		type;		// regular, directory, symlink, link
				int64			modified;	// nanoseconds, -1 unknown
				int32			permissions;	// -1 unknown
				int				fd;
				z_stream_s*		inflater;
				int64			written;
				std::string		linkData;
				bool			failed;
				bool			done;
			};

			struct Outgoing {
				std::string		requestID;
				std::string		path;
				bool			symlink;
				bool			compress;
			};

			struct Session {
								Session();

				std::string		id;
				bool			sending;	// from the program to here
				bool			accepted;
				bool			asked;
				bool			listed;
				int32			quiet;
				int32			expected;	// paths of a receive session
				std::vector<std::pair<std::string, std::string> > requests;
				std::vector<File> files;
				bigtime_t		lastActive;

				// the file being sent to the program, and the ones after it
				std::deque<Outgoing> outgoing;
				int				readFD;
				z_stream_s*		deflater;
				std::string		pending;
				bool			readDone;
			};

			void				_Send(Session& session, const Fields& fields);
			void				_Status(Session& session,
									const std::string& fileID,
									const std::string& status,
									int64 size = -1, bool error = false);
			void				_Ask(Session& session);
			bool				_CheckPassword(const Session& session,
									const std::string& field) const;
			void				_End(const std::string& id);
			void				_Expire();

			// sending to here
			void				_SendFile(Session& session,
									const Fields& fields);
			void				_SendData(Session& session,
									const Fields& fields, bool end);
			void				_Commit(Session& session);

			// receiving from here
			void				_List(Session& session);
			void				_ListPath(Session& session,
									const std::string& requestID,
									const std::string& path,
									const std::string& parent, int32& next,
									std::map<std::pair<dev_t, ino_t>,
										std::string>& seen,
									std::map<std::string, std::string>& ids);
			void				_ReceiveFile(Session& session,
									const Fields& fields);
			bool				_PumpSession(Session& session, size_t& budget);
			void				_EndOutgoing(Session& session);
			void				_SchedulePump(bool later);

private:
			BHandler*			fTarget;
			Writer				fWriter;
			Backlog				fBacklog;
			std::map<std::string, Session> fSessions;
			bool				fPumpScheduled;
};


#endif	// TERM_FILE_TRANSFER_H
