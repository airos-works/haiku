/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "L2capEndpointManager.h"

#include <AutoDeleter.h>

#include <string.h>

L2capEndpointManager gL2capEndpointManager;


L2capEndpointManager::L2capEndpointManager()
	:
	fNextChannelID(L2CAP_FIRST_CID),
	fFixedEndpoints(NULL)
{
	rw_lock_init(&fBoundEndpointsLock, "l2cap bound endpoints");
	rw_lock_init(&fChannelEndpointsLock, "l2cap channel endpoints");
	rw_lock_init(&fFixedEndpointsLock, "l2cap LE fixed endpoints");
}


L2capEndpointManager::~L2capEndpointManager()
{
	rw_lock_destroy(&fBoundEndpointsLock);
	rw_lock_destroy(&fChannelEndpointsLock);
	rw_lock_destroy(&fFixedEndpointsLock);
}


status_t
L2capEndpointManager::Bind(L2capEndpoint* endpoint, const sockaddr_l2cap& address)
{
	// TODO: Support binding to specific addresses?
	const bdaddr_t anyAddr = BDADDR_ANY;
	if (memcmp(&address.l2cap_bdaddr, &anyAddr, sizeof(bdaddr_t)) != 0)
		return EINVAL;

	// PSM values must be odd.
	if ((address.l2cap_psm & 1) == 0)
		return EINVAL;

	WriteLocker _(fBoundEndpointsLock);

	if (fBoundEndpoints.Find(address.l2cap_psm) != NULL)
		return EADDRINUSE;

	memcpy(*endpoint->LocalAddress(), &address, sizeof(struct sockaddr_l2cap));
	fBoundEndpoints.Insert(endpoint);
	gSocketModule->acquire_socket(endpoint->socket);

	return B_OK;
}


status_t
L2capEndpointManager::Unbind(L2capEndpoint* endpoint)
{
	WriteLocker _(fBoundEndpointsLock);

	fBoundEndpoints.Remove(endpoint);
	(*endpoint->LocalAddress())->sa_len = 0;
	gSocketModule->release_socket(endpoint->socket);

	return B_OK;
}


L2capEndpoint*
L2capEndpointManager::GetForPSM(uint16 psm)
{
	ReadLocker _(fBoundEndpointsLock);
	L2capEndpoint* endpoint = fBoundEndpoints.Find(psm);
	if (endpoint != NULL)
		gSocketModule->acquire_socket(endpoint->socket);
	return endpoint;
}


status_t
L2capEndpointManager::BindToChannel(L2capEndpoint* endpoint)
{
	WriteLocker _(fChannelEndpointsLock);

	for (uint16 i = 0; i < (L2CAP_LAST_CID - L2CAP_FIRST_CID); i++) {
		const uint16 cid = fNextChannelID;
		fNextChannelID++;
		if (fNextChannelID < L2CAP_FIRST_CID)
			fNextChannelID = L2CAP_FIRST_CID;

		if (fChannelEndpoints.Find(cid) != NULL)
			continue;

		endpoint->fChannelID = cid;
		fChannelEndpoints.Insert(endpoint);
		gSocketModule->acquire_socket(endpoint->socket);
		return B_OK;
	}

	return EADDRINUSE;
}


status_t
L2capEndpointManager::UnbindFromChannel(L2capEndpoint* endpoint)
{
	WriteLocker _(fChannelEndpointsLock);

	fChannelEndpoints.Remove(endpoint);
	endpoint->fChannelID = 0;
	gSocketModule->release_socket(endpoint->socket);

	return B_OK;
}


L2capEndpoint*
L2capEndpointManager::GetForChannel(uint16 cid)
{
	ReadLocker _(fChannelEndpointsLock);
	L2capEndpoint* endpoint = fChannelEndpoints.Find(cid);
	if (endpoint != NULL)
		gSocketModule->acquire_socket(endpoint->socket);
	return endpoint;
}


status_t
L2capEndpointManager::BindToFixedChannel(L2capEndpoint* endpoint)
{
	WriteLocker _(fFixedEndpointsLock);
	for (L2capEndpoint* current = fFixedEndpoints; current != NULL;
			current = current->fNextFixed) {
		if (current->fConnection == endpoint->fConnection
			&& current->fChannelID == endpoint->fChannelID)
			return EADDRINUSE;
	}

	endpoint->fNextFixed = fFixedEndpoints;
	fFixedEndpoints = endpoint;
	gSocketModule->acquire_socket(endpoint->socket);
	return B_OK;
}


void
L2capEndpointManager::UnbindFromFixedChannel(L2capEndpoint* endpoint)
{
	WriteLocker _(fFixedEndpointsLock);
	for (L2capEndpoint** current = &fFixedEndpoints; *current != NULL;
			current = &(*current)->fNextFixed) {
		if (*current != endpoint)
			continue;
		*current = endpoint->fNextFixed;
		endpoint->fNextFixed = NULL;
		gSocketModule->release_socket(endpoint->socket);
		break;
	}
}


L2capEndpoint*
L2capEndpointManager::GetForFixedChannel(HciConnection* connection,
	uint16 cid)
{
	ReadLocker _(fFixedEndpointsLock);
	for (L2capEndpoint* endpoint = fFixedEndpoints; endpoint != NULL;
			endpoint = endpoint->fNextFixed) {
		if (endpoint->fConnection == connection
			&& endpoint->fChannelID == cid) {
			gSocketModule->acquire_socket(endpoint->socket);
			return endpoint;
		}
	}
	return NULL;
}


void
L2capEndpointManager::Disconnected(HciConnection* connection)
{
	// Collect the fixed-channel sockets first: Shutdown() holds an endpoint's
	// lock while it takes fFixedEndpointsLock, so the reverse order here would
	// deadlock.
	L2capEndpoint* fixed[8];
	int32 fixedCount = 0;
	{
		ReadLocker _(fFixedEndpointsLock);
		for (L2capEndpoint* endpoint = fFixedEndpoints; endpoint != NULL
				&& fixedCount < (int32)B_COUNT_OF(fixed);
				endpoint = endpoint->fNextFixed) {
			if (endpoint->fConnection != connection)
				continue;
			gSocketModule->acquire_socket(endpoint->socket);
			fixed[fixedCount++] = endpoint;
		}
	}
	for (int32 i = 0; i < fixedCount; i++) {
		L2capEndpoint* endpoint = fixed[i];
		{
			MutexLocker locker(endpoint->fLock);
			if (endpoint->fConnection == connection) {
				dprintf("l2cap-le: link handle %#x closed under cid %#x\n",
					connection->handle, endpoint->fChannelID);
				endpoint->fConnection = NULL;
				endpoint->fState = L2capEndpoint::CLOSED;
				endpoint->socket->error = ENOTCONN;
			}
		}
		gSocketModule->notify(endpoint->socket, B_SELECT_ERROR, ENOTCONN);
		gSocketModule->notify(endpoint->socket, B_SELECT_READ, ENOTCONN);
		gSocketModule->release_socket(endpoint->socket);
	}

	// Close the dynamic channels too: wake anyone waiting in connect() or
	// close(), give back the channel IDs, and drop the idents of requests
	// the peer will now never answer. The connection, and its ident table,
	// is deleted after this returns.
	while (true) {
		L2capEndpoint* channels[16];
		int32 count = 0;
		{
			ReadLocker _(fChannelEndpointsLock);
			auto iter = fChannelEndpoints.GetIterator();
			while (iter.HasNext() && count < (int32)B_COUNT_OF(channels)) {
				L2capEndpoint* endpoint = iter.Next();
				if (endpoint->fConnection != connection)
					continue;
				gSocketModule->acquire_socket(endpoint->socket);
				channels[count++] = endpoint;
			}
		}
		if (count == 0)
			break;

		for (int32 i = 0; i < count; i++) {
			L2capEndpoint* endpoint = channels[i];
			bool releaseIdent = false;
			{
				MutexLocker locker(endpoint->fLock);
				if (endpoint->fConnection == connection) {
					releaseIdent = endpoint->_DropCommandIdent();
					endpoint->fConnection = NULL;
					endpoint->fState = L2capEndpoint::CLOSED;
					endpoint->fConnectError = EHOSTUNREACH;
					endpoint->socket->error = ENOTCONN;
					endpoint->fCommandWait.NotifyAll();
					UnbindFromChannel(endpoint);
				}
			}
			gSocketModule->notify(endpoint->socket, B_SELECT_ERROR, ENOTCONN);
			gSocketModule->notify(endpoint->socket, B_SELECT_READ, ENOTCONN);
			if (releaseIdent)
				gSocketModule->release_socket(endpoint->socket);
			endpoint->_AbortIfNeverAccepted();
			gSocketModule->release_socket(endpoint->socket);
		}
	}
}
