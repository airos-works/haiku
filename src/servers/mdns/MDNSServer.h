/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MDNS_SERVER_H
#define MDNS_SERVER_H


#include <Locker.h>
#include <Messenger.h>
#include <ObjectList.h>
#include <Server.h>
#include <String.h>

#include <netinet/in.h>


struct MDNSRecord {
	BString		name;
		// lower case, without the final dot
	in_addr_t	address;
		// 0: the address of the interface the question came in on
	BMessenger	owner;
	int32		announcements;
	bigtime_t	nextAnnouncement;
};


class MDNSServer : public BServer {
public:
								MDNSServer(status_t* _error);
	virtual						~MDNSServer();

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
	static	status_t			_ListenerEntry(void* self);
			void				_Listen();
			void				_JoinGroups();
			void				_HandlePacket(const uint8* packet, size_t size,
									const sockaddr_in& from);
			void				_SendAnswers(MDNSRecord* record,
									const sockaddr_in* to, uint32 ttl,
									const uint8* question, size_t questionSize,
									uint16 queryID);
			void				_Announce(MDNSRecord* record, uint32 ttl);
			void				_Housekeeping();
			in_addr_t			_AddressFor(const MDNSRecord* record,
									in_addr_t querier);

			MDNSRecord*			_Find(const char* name);

private:
			BLocker				fLock;
			BObjectList<MDNSRecord, true> fRecords;
			int					fSocket;
			thread_id			fListener;
			bool				fQuitting;
			BObjectList<in_addr, true> fJoined;
			bigtime_t			fNextJoin;
};


#endif	// MDNS_SERVER_H
