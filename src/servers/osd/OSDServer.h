/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef OSD_SERVER_H
#define OSD_SERVER_H


#include <Server.h>


class LevelWindow;


class OSDServer : public BServer {
public:
								OSDServer(status_t& error);

	virtual	void				MessageReceived(BMessage* message);

private:
			LevelWindow*		fLevelWindow;
};


#endif	// OSD_SERVER_H
