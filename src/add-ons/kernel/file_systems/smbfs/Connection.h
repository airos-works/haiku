/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SMBFS_CONNECTION_H
#define SMBFS_CONNECTION_H


#include <pthread.h>

#include <string>

#include <OS.h>


struct smb2_context;


struct Options {
	std::string	server;
	std::string	share;
	std::string	path;
		// directory of the share that is the root of the volume, no slash at
		// either end
	std::string	user;
	std::string	password;
	std::string	domain;
	std::string	name;
	bool		readOnly;
	bool		encrypt;
	int			timeout;
		// seconds a request may take
	int			connections;

	Options()
		:
		readOnly(false),
		encrypt(false),
		timeout(20),
		connections(3)
	{
	}
};


/*!	What a request to the server came to. libsmb2 calls Callback() when it
	knows.
*/
struct Request {
	bool	finished;
	int		status;
	void*	data;

	Request()
		:
		finished(false),
		status(0),
		data(NULL)
	{
	}

	static void Callback(smb2_context* context, int status, void* data,
		void* cookie);
};


/*!	One session with the server. libsmb2 does one thing at a time with a
	session, so whoever uses one locks it for as long as that takes.

	The session is made when it is first needed, and again after it was lost.
	What the server handed out for the old session (file handles) is void
	then, which is what Generation() is for: it changes with every new
	session.
*/
class Connection {
public:
								Connection();
								~Connection();

			void				Init(const Options* options, int index);
			void				SetCancelFD(int fd)
									{ fCancelFD = fd; }
									// when it can be read, Wait() gives up

			void				Lock();
			bool				TryLock();
			void				Unlock();

	// all of these with the lock held

			bool				IsConnected() const
									{ return fContext != NULL; }
			int					EnsureConnected();
			void				Disconnect(bool sayGoodbye = false);

			smb2_context*		Context() const
									{ return fContext; }
			uint32				Generation() const
									{ return fGeneration; }

			bool				Wait(Request& request);

	static	bool				IsLost(int status);

private:
			const Options*		fOptions;
			int					fIndex;
			pthread_mutex_t		fLock;
			smb2_context*		fContext;
			uint32				fGeneration;
			bigtime_t			fLastFailure;
			int					fLastError;
			int					fCancelFD;
};


class ConnectionPool {
public:
								ConnectionPool();
								~ConnectionPool();

			void				Init(const Options* options);
			void				DisconnectAll();

			Connection*			Acquire();
									// returns it locked

private:
			Connection*			fConnections;
			int					fCount;
			int32				fNext;
};


#endif	// SMBFS_CONNECTION_H
