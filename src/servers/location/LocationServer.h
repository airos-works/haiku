/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LOCATION_SERVER_H
#define LOCATION_SERVER_H


#include <Locker.h>
#include <Server.h>
#include <String.h>


struct Position {
	double		latitude;
	double		longitude;
	double		accuracy;
	BString		source;
	bigtime_t	time;
		// real_time_clock_usecs(); 0: none
};


class LocationServer : public BServer {
public:
								LocationServer(status_t* _error);

	virtual	void				MessageReceived(BMessage* message);

private:
	static	status_t			_RequestEntry(void* message);
			void				_HandleRequest(BMessage* request);
			status_t			_Position(bigtime_t maxAge,
									Position& _position);
			status_t			_LookUp(Position& _position);
			void				_LoadSettings();

private:
			BLocker				fLock;
				// held during a lookup: requests meanwhile wait for its result
			Position			fCached;
			bool				fEnabled;
			BString				fService;
			bool				fHasFixed;
			Position			fFixed;
};


#endif	// LOCATION_SERVER_H
