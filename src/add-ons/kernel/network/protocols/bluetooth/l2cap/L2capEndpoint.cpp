/*
 * Copyright 2008, Oliver Ruiz Dorantes. All rights reserved.
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "L2capEndpoint.h"

#include <stdio.h>
#include <string.h>

#include <Errors.h>
#include <NetBufferUtilities.h>

#include "L2capEndpointManager.h"
#include "SupportDefs.h"
#include "l2cap_signal.h"
#include "l2cap_le.h"
#include <btDebug.h>


static l2cap_qos sDefaultQOS = {
	.flags = 0x0,
	.service_type = 1,
	.token_rate = 0xffffffff, /* maximum */
	.token_bucket_size = 0xffffffff, /* maximum */
	.peak_bandwidth = 0x00000000, /* maximum */
	.access_latency = 0xffffffff, /* don't care */
	.delay_variation = 0xffffffff /* don't care */
};


// Signaling timeouts (Core Vol 3 Part A 6.2): how long a request may go
// unanswered, and how long a peer may keep a connection pending once it has
// said so. ACL links are reliable, so requests are not retransmitted.
static const bigtime_t kResponseTimeout = 20000000;
static const bigtime_t kPendingTimeout = 60000000;


static inline status_t
posix_error(status_t error)
{
	if (error == B_TIMED_OUT)
		return B_WOULD_BLOCK;

	return error;
}


// #pragma mark - endpoint


L2capEndpoint::L2capEndpoint(net_socket* socket)
	:
	ProtocolSocket(socket),
	fAcceptSemaphore(-1),
	fState(CLOSED),
	fConnection(NULL),
	fChannelID(L2CAP_NULL_CID),
	fDestinationChannelID(L2CAP_NULL_CID),
	fFixedChannel(false),
	fNextFixed(NULL),
	fConnectError(ECONNREFUSED),
	fCommandIdent(L2CAP_NULL_IDENT),
	fCommandDeadline(B_INFINITE_TIMEOUT)
{
	CALLED();

	mutex_init(&fLock, "l2cap endpoint");
	fCommandWait.Init(this, "l2cap endpoint command");

	// Set MTU and flow control settings to defaults
	fChannelConfig.incoming_mtu = L2CAP_MTU_DEFAULT;
	memcpy(&fChannelConfig.incoming_flow, &sDefaultQOS, sizeof(l2cap_qos));

	fChannelConfig.outgoing_mtu = L2CAP_MTU_DEFAULT;
	memcpy(&fChannelConfig.outgoing_flow, &sDefaultQOS, sizeof(l2cap_qos));

	fChannelConfig.flush_timeout = L2CAP_FLUSH_TIMEOUT_DEFAULT;
	fChannelConfig.link_timeout  = L2CAP_LINK_TIMEOUT_DEFAULT;

	fConfigState = {};

	gStackModule->init_fifo(&fReceiveQueue, "l2cap recv", L2CAP_MTU_MAXIMUM);
	gStackModule->init_fifo(&fSendQueue, "l2cap send", L2CAP_MTU_MAXIMUM);
	gStackModule->init_timer(&fSendTimer, L2capEndpoint::_SendTimer, this);
}


L2capEndpoint::~L2capEndpoint()
{
	CALLED();

	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	ASSERT(fState == CLOSED);

	fCommandWait.NotifyAll(B_ERROR);

	gStackModule->uninit_fifo(&fReceiveQueue);
	gStackModule->uninit_fifo(&fSendQueue);
	gStackModule->wait_for_timer(&fSendTimer);
}


status_t
L2capEndpoint::_WaitForStateChange(bigtime_t absoluteTimeout)
{
	channel_status state = fState;
	while (fState == state) {
		const bool expiring = fState == WAIT_FOR_CONNECTION_RSP
			|| fState == CONFIGURATION || fState == WAIT_FOR_DISCONNECTION_RSP;
		bigtime_t until = absoluteTimeout;
		if (expiring && fCommandDeadline < until)
			until = fCommandDeadline;

		status_t status = fCommandWait.Wait(&fLock,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, until);
		if (status == B_TIMED_OUT && fState == state && expiring
			&& system_time() >= fCommandDeadline) {
			_ExpireCommand();
			continue;
		}
		if (status != B_OK)
			return posix_error(status);
	}

	return B_OK;
}


/*!	Forgets the request this endpoint is waiting on, so that a late answer is
	ignored. Returns the socket reference the ident held; the caller must
	release it, after unlocking if it may be the last one.
*/
bool
L2capEndpoint::_DropCommandIdent()
{
	ASSERT_LOCKED_MUTEX(&fLock);

	const uint8 ident = fCommandIdent;
	fCommandIdent = L2CAP_NULL_IDENT;
	if (ident == L2CAP_NULL_IDENT || fConnection == NULL)
		return false;
	if (btCoreData->lookup_command_ident(fConnection, ident) != this)
		return false;

	btCoreData->free_command_ident(fConnection, ident);
	return true;
}


void
L2capEndpoint::_ExpireCommand()
{
	ASSERT_LOCKED_MUTEX(&fLock);

	dprintf("l2cap: cid %#x: no answer from the peer in state %d, giving "
		"up\n", fChannelID, fState);

	// The ident's socket reference is never the last one here: the thread
	// waiting in Connect() or Shutdown() is inside a call on the socket.
	if (_DropCommandIdent())
		gSocketModule->release_socket(socket);

	if (fState == WAIT_FOR_DISCONNECTION_RSP) {
		_MarkClosed();
		return;
	}

	if (fState == CONFIGURATION && fConnection != NULL) {
		// The peer has a channel for us; ask it to go away, but do not wait.
		uint8 ident = btCoreData->allocate_command_ident(fConnection, NULL);
		if (ident != L2CAP_NULL_IDENT) {
			send_l2cap_disconnection_req(fConnection, ident,
				fDestinationChannelID, fChannelID);
			btCoreData->free_command_ident(fConnection, ident);
		}
	}

	_MarkRefused(ETIMEDOUT);
}


status_t
L2capEndpoint::Open()
{
	CALLED();
	return ProtocolSocket::Open();
}


status_t
L2capEndpoint::Shutdown()
{
	CALLED();
	MutexLocker locker(fLock);
	if (fFixedChannel) {
		L2CAP_LE_TRACE("socket %p closing cid %#x\n", this, fChannelID);
		gL2capEndpointManager.UnbindFromFixedChannel(this);
		fFixedChannel = false;
		fConnection = NULL;
		fState = CLOSED;
		return B_OK;
	}

	if (fState == CLOSED) {
		// Nothing to do.
		return B_OK;
	}
	if (fState == LISTEN) {
		delete_sem(fAcceptSemaphore);
		fAcceptSemaphore = -1;
		gSocketModule->set_max_backlog(socket, 0);
		fState = BOUND;
		locker.Unlock();
		return Unbind();
	}

	status_t status;
	bigtime_t timeout;
	if (gStackModule->is_restarted_syscall())
		timeout = gStackModule->restore_syscall_restart_timeout();
	else
		timeout = gStackModule->set_syscall_restart_timeout(socket->receive.timeout);

	if (fState == WAIT_FOR_CONNECTION_RSP) {
		// The peer has not told us its channel yet, so there is nothing to
		// disconnect. Forget the request; a late answer is ignored.
		if (_DropCommandIdent())
			gSocketModule->release_socket(socket);
		_MarkRefused(ECONNABORTED);
		return B_OK;
	}

	while (fState > OPEN) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}
	if (fState == CLOSED)
		return B_OK;

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT)
		return ENOBUFS;
	gSocketModule->acquire_socket(socket);

	status = send_l2cap_disconnection_req(fConnection, ident,
		fDestinationChannelID, fChannelID);
	if (status != B_OK) {
		btCoreData->free_command_ident(fConnection, ident);
		gSocketModule->release_socket(socket);
		return status;
	}

	if (_DropCommandIdent())
		gSocketModule->release_socket(socket);
	fCommandIdent = ident;
	fCommandDeadline = system_time() + kResponseTimeout;
	fState = WAIT_FOR_DISCONNECTION_RSP;

	while (fState != CLOSED) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


status_t
L2capEndpoint::Close()
{
	return Shutdown();
}


status_t
L2capEndpoint::Free()
{
	CALLED();

	return B_OK;
}


status_t
L2capEndpoint::Bind(const struct sockaddr* _address)
{
	const sockaddr_l2cap* address
		= reinterpret_cast<const sockaddr_l2cap*>(_address);
	if (AddressModule()->is_empty_address(_address, true))
		return B_OK; // We don't need to bind to empty.
	if (!AddressModule()->is_same_family(_address))
		return EAFNOSUPPORT;
	if (address->l2cap_len != sizeof(struct sockaddr_l2cap))
		return EAFNOSUPPORT;

	CALLED();
	MutexLocker _(fLock);

	if (fState != CLOSED)
		return EISCONN;

	status_t status = gL2capEndpointManager.Bind(this, *address);
	if (status != B_OK)
		return status;

	fState = BOUND;
	return B_OK;
}


status_t
L2capEndpoint::Unbind()
{
	CALLED();
	MutexLocker _(fLock);

	if (LocalAddress().IsEmpty(true))
		return EINVAL;

	status_t status = gL2capEndpointManager.Unbind(this);
	if (status != B_OK)
		return status;

	if (fState == BOUND)
		fState = CLOSED;
	return B_OK;
}


status_t
L2capEndpoint::Listen(int backlog)
{
	CALLED();
	MutexLocker _(fLock);

	if (fState != BOUND)
		return B_BAD_VALUE;

	fAcceptSemaphore = create_sem(0, "l2cap accept");
	if (fAcceptSemaphore < B_OK) {
		ERROR("%s: Semaphore could not be created\n", __func__);
		return ENOBUFS;
	}

	gSocketModule->set_max_backlog(socket, backlog);

	fState = LISTEN;
	return B_OK;
}


status_t
L2capEndpoint::Connect(const struct sockaddr* _address)
{
	const sockaddr_l2cap* address
		= reinterpret_cast<const sockaddr_l2cap*>(_address);
	if (!AddressModule()->is_same_family(_address))
		return EAFNOSUPPORT;
	if (address->l2cap_len != sizeof(struct sockaddr_l2cap))
		return EAFNOSUPPORT;

	TRACE("l2cap: connect(\"%s\")\n",
		ConstSocketAddress(AddressModule(), _address).AsString().Data());
	MutexLocker _(fLock);

	status_t status;
	bigtime_t timeout;
	if (gStackModule->is_restarted_syscall()) {
		timeout = gStackModule->restore_syscall_restart_timeout();

		while (fState != CLOSED && fState != OPEN) {
			status = _WaitForStateChange(timeout);
			if (status != B_OK)
				return status;
		}
		return (fState == OPEN) ? B_OK : fConnectError;
	} else {
		timeout = gStackModule->set_syscall_restart_timeout(socket->send.timeout);
	}

	if (fState == LISTEN)
		return EINVAL;
	if (fState == OPEN)
		return EISCONN;
	if (fState != CLOSED)
		return EALREADY;

	const bool fixedChannel = address->l2cap_psm == L2CAP_ATT_CID
		|| address->l2cap_psm == L2CAP_SMP_CID;
	if (fixedChannel)
		l2cap_le_reload_settings();

	// Set up route.
	hci_id hid = btCoreData->RouteConnection(address->l2cap_bdaddr);
	if (hid <= 0) {
		if (fixedChannel) {
			dprintf("l2cap-le: no ACL link to %02X:%02X:%02X:%02X:%02X:%02X "
				"for cid %#x\n", address->l2cap_bdaddr.b[5],
				address->l2cap_bdaddr.b[4], address->l2cap_bdaddr.b[3],
				address->l2cap_bdaddr.b[2], address->l2cap_bdaddr.b[1],
				address->l2cap_bdaddr.b[0], address->l2cap_psm);
		}
		return ENETUNREACH;
	}

	TRACE("l2cap: %" B_PRId32 " for route %02X:%02X:%02X:%02X:%02X:%02X\n", hid,
		address->l2cap_bdaddr.b[5], address->l2cap_bdaddr.b[4], address->l2cap_bdaddr.b[3],
		address->l2cap_bdaddr.b[2], address->l2cap_bdaddr.b[1], address->l2cap_bdaddr.b[0]);

	fConnection = btCoreData->ConnectionByDestination(
		address->l2cap_bdaddr, hid);
	if (fConnection == NULL) {
		if (fixedChannel)
			dprintf("l2cap-le: route found but no connection for cid %#x\n",
				address->l2cap_psm);
		return EHOSTUNREACH;
	}

	memcpy(&socket->peer, _address, sizeof(struct sockaddr_l2cap));
	if (fixedChannel) {
		if (!fConnection->isLE) {
			dprintf("l2cap-le: link handle %#x to the peer is not LE; cannot "
				"open cid %#x\n", fConnection->handle, address->l2cap_psm);
			fConnection = NULL;
			return EPROTONOSUPPORT;
		}
		fChannelID = address->l2cap_psm;
		fDestinationChannelID = fChannelID;
		// ATT starts with a 23-byte MTU. SMP can carry a 65-byte public key.
		fChannelConfig.outgoing_mtu = fChannelID == L2CAP_ATT_CID ? 23 : 65;
		status = gL2capEndpointManager.BindToFixedChannel(this);
		L2CAP_LE_TRACE("socket %p %s cid %#x on handle %#x: %s\n", this,
			status == B_OK ? "bound" : "could not bind", fChannelID,
			fConnection->handle, strerror(status));
		if (status != B_OK) {
			dprintf("l2cap-le: cid %#x on handle %#x is already open by "
				"another socket (%s)\n", fChannelID, fConnection->handle,
				strerror(status));
			fChannelID = L2CAP_NULL_CID;
			fDestinationChannelID = L2CAP_NULL_CID;
			fConnection = NULL;
			return status;
		}
		fFixedChannel = true;
		fState = OPEN;
		return B_OK;
	}

	status = gL2capEndpointManager.BindToChannel(this);
	if (status != B_OK)
		return status;

	fConfigState = {};

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT)
		return ENOBUFS;
	gSocketModule->acquire_socket(socket);

	status = send_l2cap_connection_req(fConnection, ident,
		address->l2cap_psm, fChannelID);
	if (status != B_OK) {
		btCoreData->free_command_ident(fConnection, ident);
		gSocketModule->release_socket(socket);
		return status;
	}

	fState = WAIT_FOR_CONNECTION_RSP;
	fConnectError = ECONNREFUSED;
	fCommandIdent = ident;
	fCommandDeadline = system_time() + kResponseTimeout;

	while (fState != CLOSED && fState != OPEN) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}
	return (fState == OPEN) ? B_OK : fConnectError;
}


status_t
L2capEndpoint::Accept(net_socket** _acceptedSocket)
{
	CALLED();
	MutexLocker locker(fLock);

	status_t status;
	bigtime_t timeout;
	if (gStackModule->is_restarted_syscall())
		timeout = gStackModule->restore_syscall_restart_timeout();
	else
		timeout = gStackModule->set_syscall_restart_timeout(socket->receive.timeout);

	do {
		locker.Unlock();

		status = acquire_sem_etc(fAcceptSemaphore, 1, B_ABSOLUTE_TIMEOUT
			| B_CAN_INTERRUPT, timeout);
		if (status != B_OK) {
			if (status == B_TIMED_OUT && socket->receive.timeout == 0)
				return B_WOULD_BLOCK;

			return status;
		}

		locker.Lock();
		status = gSocketModule->dequeue_connected(socket, _acceptedSocket);
	} while (status != B_OK);

	return status;
}


ssize_t
L2capEndpoint::ReadData(size_t numBytes, uint32 flags, net_buffer** _buffer)
{
	CALLED();
	MutexLocker locker(fLock);

	*_buffer = NULL;

	bigtime_t timeout = 0;
	if ((flags & MSG_DONTWAIT) == 0) {
		if (gStackModule->is_restarted_syscall())
			timeout = gStackModule->restore_syscall_restart_timeout();
		else
			timeout = gStackModule->set_syscall_restart_timeout(socket->receive.timeout);
	}

	if (fState == CLOSED)
		flags |= MSG_DONTWAIT;

	locker.Unlock();
	ssize_t bytes = gStackModule->fifo_dequeue_buffer(&fReceiveQueue, flags, timeout, _buffer);
	if (bytes == B_WOULD_BLOCK && fState == CLOSED)
		return ENOTCONN;
	return bytes;
}


status_t
L2capEndpoint::SendData(net_buffer* buffer)
{
	CALLED();
	MutexLocker locker(fLock);

	if (buffer == NULL)
		return ENOBUFS;

	if (fState != OPEN)
		return ENOTCONN;

	if (buffer->size > fChannelConfig.outgoing_mtu)
		return EMSGSIZE;

	status_t status = gStackModule->fifo_enqueue_buffer(&fSendQueue, buffer);
	if (status != B_OK)
		return status;

	if (!gStackModule->is_timer_active(&fSendTimer))
		gStackModule->set_timer(&fSendTimer, 0);

	return B_OK;
}


status_t
L2capEndpoint::ReceiveData(net_buffer* buffer)
{
	CALLED();
	// FIXME: Check address specified in net_buffer!
	status_t status = gStackModule->fifo_enqueue_buffer(&fReceiveQueue, buffer);

	if (status == B_OK)
		gSocketModule->notify(socket, B_SELECT_READ, Receivable());

	return status;
}


/*static*/ void
L2capEndpoint::_SendTimer(net_timer* timer, void* _endpoint)
{
	L2capEndpoint* endpoint = (L2capEndpoint*)_endpoint;

	MutexLocker locker(endpoint->fLock);
	if (!locker.IsLocked() || gStackModule->is_timer_active(timer))
		return;

	endpoint->_SendQueued();
}


void
L2capEndpoint::_SendQueued()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);

	if (fState != OPEN)
		return;

	net_buffer* buffer;
	while (gStackModule->fifo_dequeue_buffer(&fSendQueue, MSG_DONTWAIT, 0, &buffer) >= 0) {
		NetBufferPrepend<l2cap_basic_header> header(buffer);
		if (header.Status() != B_OK) {
			ERROR("%s: header could not be prepended!\n", __func__);
			gBufferModule->free(buffer);
			continue;
		}

		header->length = B_HOST_TO_LENDIAN_INT16(buffer->size - sizeof(l2cap_basic_header));
		header->dcid = B_HOST_TO_LENDIAN_INT16(fDestinationChannelID);

		if (fFixedChannel)
			l2cap_le_dump("tx", fConnection, fDestinationChannelID, buffer);
		buffer->type = fConnection->handle;
		if (btDevices->PostACL(fConnection->ndevice->index, buffer) != B_OK) {
			ERROR("%s: could not post ACL data on handle %#x\n", __func__,
				fConnection->handle);
			gBufferModule->free(buffer);
		}
	}
}


ssize_t
L2capEndpoint::Sendable()
{
	CALLED();
	MutexLocker locker(fLock);

	if (fState != OPEN) {
		if (_IsEstablishing())
			return 0;
		return EPIPE;
	}

	MutexLocker fifoLocker(fSendQueue.lock);
	return (fSendQueue.max_bytes - fSendQueue.current_bytes);
}


ssize_t
L2capEndpoint::Receivable()
{
	CALLED();
	MutexLocker locker(fLock);

	MutexLocker fifoLocker(fReceiveQueue.lock);
	return fReceiveQueue.current_bytes;
}


status_t
L2capEndpoint::GetOption(int option, void* value, int* _length)
{
	MutexLocker locker(fLock);

	uint16 result;
	switch (option) {
		case B_L2CAP_OUTGOING_MTU:
			result = fChannelConfig.outgoing_mtu;
			break;
		case B_L2CAP_INCOMING_MTU:
			result = fChannelConfig.incoming_mtu;
			break;
		default:
			return ENOPROTOOPT;
	}

	if (*_length < (int)sizeof(result))
		return B_BAD_VALUE;
	if (fState != OPEN)
		return ENOTCONN;

	memcpy(value, &result, sizeof(result));
	*_length = sizeof(result);
	return B_OK;
}


void
L2capEndpoint::_HandleCommandRejected(uint8 ident, uint16 reason,
	const l2cap_command_reject_data& data)
{
	CALLED();
	MutexLocker locker(fLock);

	if (ident == fCommandIdent)
		fCommandIdent = L2CAP_NULL_IDENT;

	switch (fState) {
		case WAIT_FOR_CONNECTION_RSP:
			// Connection request was rejected. Reset state.
			_MarkRefused(ECONNREFUSED);
		break;

		case CONFIGURATION:
			// TODO: Adjust and resend configuration request.
		break;

	default:
		ERROR("l2cap: unknown command unexpectedly rejected (ident %d)\n", ident);
		break;
	}

	fCommandWait.NotifyAll();
}


void
L2capEndpoint::_HandleConnectionReq(HciConnection* connection,
	uint8 ident, uint16 psm, uint16 scid)
{
	MutexLocker locker(fLock);
	if (fState != LISTEN) {
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_PSM_NOT_SUPPORTED, 0);
		return;
	}
	locker.Unlock();

	net_socket* newSocket;
	status_t status = gSocketModule->spawn_pending_socket(socket, &newSocket);
	if (status != B_OK) {
		ERROR("l2cap: could not spawn child for endpoint: %s\n", strerror(status));
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_NO_RESOURCES, 0);
		return;
	}

	L2capEndpoint* endpoint = (L2capEndpoint*)newSocket->first_protocol;
	MutexLocker newEndpointLocker(endpoint->fLock);

	status = gL2capEndpointManager.BindToChannel(endpoint);
	if (status != B_OK) {
		ERROR("l2cap: could not allocate channel for endpoint: %s\n", strerror(status));
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_NO_RESOURCES, 0);
		return;
	}

	endpoint->fAcceptSemaphore = fAcceptSemaphore;

	// accept() and getpeername() tell the server which device this is (a
	// HID device opens its channels itself when it reconnects).
	memcpy(&newSocket->address, &socket->address, sizeof(sockaddr_l2cap));
	sockaddr_l2cap* peer = (sockaddr_l2cap*)&newSocket->peer;
	memset(peer, 0, sizeof(sockaddr_l2cap));
	peer->l2cap_len = sizeof(sockaddr_l2cap);
	peer->l2cap_family = AF_BLUETOOTH;
	peer->l2cap_psm = psm;
	peer->l2cap_bdaddr = connection->destination;

	endpoint->fConnection = connection;
	endpoint->fState = CONFIGURATION;

	endpoint->fDestinationChannelID = scid;

	send_l2cap_connection_rsp(connection, ident, endpoint->fChannelID, scid,
		l2cap_connection_rsp::RESULT_SUCCESS, 0);

	// Our side of the configuration goes out now, as other stacks do: a
	// peer that waits for it before sending its own (the Keychron K4 seems
	// to) would otherwise leave the channel half open.
	endpoint->_SendChannelConfig();
}


/*!	A channel the peer opened and closed again (or lost with its link)
	before anyone accepted it. It still counted against the listener's
	backlog; a handful of those and every new channel on that PSM was
	refused for want of resources, until the listening server restarted.
	The caller must hold a reference to the socket and not fLock: taking
	the channel off the listener can drop the last but that reference.
*/
void
L2capEndpoint::_AbortIfNeverAccepted()
{
	{
		MutexLocker locker(fLock);
		if (fState != CLOSED)
			return;
	}
	if (gSocketModule->has_parent(socket)) {
		dprintf("l2cap: channel %#x closed before it was accepted; off the "
			"listener's backlog\n", fChannelID);
		gSocketModule->set_aborted(socket);
	}
}


void
L2capEndpoint::_HandleConnectionRsp(uint8 ident, const l2cap_connection_rsp& response)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != WAIT_FOR_CONNECTION_RSP) {
		ERROR("l2cap: unexpected connection response, scid=%d, state=%d\n",
			response.scid, fState);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, response.scid, response.dcid);
		return;
	}

	if (fChannelID != response.scid) {
		ERROR("l2cap: invalid connection response, mismatched SCIDs (%d, %d)\n",
			fChannelID, response.scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, response.scid, response.dcid);
		return;
	}

	if (response.result == l2cap_connection_rsp::RESULT_PENDING) {
		// The connection is still pending on the remote end, which is often
		// busy authenticating or authorizing the link. It answers again later
		// with the same identifier (Core Vol 3 Part A 4.3).
		fCommandDeadline = system_time() + kPendingTimeout;
		return;
	}

	if (ident == fCommandIdent)
		fCommandIdent = L2CAP_NULL_IDENT;

	if (response.result != l2cap_connection_rsp::RESULT_SUCCESS) {
		dprintf("l2cap: connection refused by the peer, result %#x status "
			"%#x\n", response.result, response.status);
		_MarkRefused(response.result
			== l2cap_connection_rsp::RESULT_SECURITY_BLOCK
				? EACCES : ECONNREFUSED);
		return;
	}

	// Success: channel is now open for configuration.
	fState = CONFIGURATION;
	fDestinationChannelID = response.dcid;
	fCommandDeadline = system_time() + kResponseTimeout;

	_SendChannelConfig();
}


void
L2capEndpoint::_SendChannelConfig()
{
	uint16* mtu = NULL;
	if (fChannelConfig.incoming_mtu != L2CAP_MTU_DEFAULT)
		mtu = &fChannelConfig.incoming_mtu;

	uint16* flush_timeout = NULL;
	if (fChannelConfig.flush_timeout != L2CAP_FLUSH_TIMEOUT_DEFAULT)
		flush_timeout = &fChannelConfig.flush_timeout;

	l2cap_qos* flow = NULL;
	if (memcmp(&sDefaultQOS, &fChannelConfig.outgoing_flow,
			sizeof(fChannelConfig.outgoing_flow)) != 0) {
		flow = &fChannelConfig.outgoing_flow;
	}

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT) {
		// TODO: Retry later?
		return;
	}
	gSocketModule->acquire_socket(socket);

	status_t status = send_l2cap_configuration_req(fConnection, ident,
		fDestinationChannelID, 0, flush_timeout, mtu, flow);
	if (status != B_OK) {
		btCoreData->free_command_ident(fConnection, ident);
		gSocketModule->release_socket(socket);
		socket->error = status;
		return;
	}

	if (_DropCommandIdent())
		gSocketModule->release_socket(socket);
	fCommandIdent = ident;

	fConfigState.out = ConfigState::SENT;
}


void
L2capEndpoint::_HandleConfigurationReq(uint8 ident, uint16 flags,
	uint16* mtu, uint16* flush_timeout, l2cap_qos* flow)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != CONFIGURATION && fState != OPEN) {
		ERROR("l2cap: unexpected configuration req: invalid channel state (cid=%d, state=%d)\n",
			fChannelID, fState);
		send_l2cap_configuration_rsp(fConnection, ident, fDestinationChannelID, 0,
			l2cap_configuration_rsp::RESULT_REJECTED, NULL);
		return;
	}

	if (fState == OPEN) {
		// Re-configuration.
		fConfigState = {};
		fState = CONFIGURATION;
	}

	// Process options.
	// TODO: Validate parameters!
	if (mtu != NULL && *mtu != fChannelConfig.outgoing_mtu)
		fChannelConfig.outgoing_mtu = *mtu;
	if (flush_timeout != NULL && *flush_timeout != fChannelConfig.flush_timeout)
		fChannelConfig.flush_timeout = *flush_timeout;
	if (flow != NULL)
		fChannelConfig.incoming_flow = *flow;

	send_l2cap_configuration_rsp(fConnection, ident, fDestinationChannelID, 0,
		l2cap_configuration_rsp::RESULT_SUCCESS, NULL);

	if ((flags & L2CAP_CFG_FLAG_CONTINUATION) != 0) {
		// More options are coming, just keep waiting.
		return;
	}

	// We now have all options.
	fConfigState.in = ConfigState::DONE;

	if (fConfigState.out < ConfigState::SENT)
		_SendChannelConfig();
	else if (fConfigState.out == ConfigState::DONE)
		_MarkEstablished();
}


void
L2capEndpoint::_HandleConfigurationRsp(uint8 ident, uint16 scid, uint16 flags,
	uint16 result, uint16* mtu, uint16* flush_timeout, l2cap_qos* flow)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != CONFIGURATION) {
		ERROR("l2cap: unexpected configuration rsp: invalid channel state (cid=%d, state=%d)\n",
			fChannelID, fState);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}
	if (scid != fChannelID) {
		ERROR("l2cap: unexpected configuration rsp: invalid source channel (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	// TODO: Validate parameters!
	if (result == l2cap_configuration_rsp::RESULT_PENDING) {
		// We will receive another CONFIGURATION_RSP later.
		return;
	} else if (result == l2cap_configuration_rsp::RESULT_UNACCEPTABLE_PARAMS) {
		// The acceptable parameters are specified in options.
		if (mtu != NULL && *mtu != fChannelConfig.incoming_mtu)
			fChannelConfig.incoming_mtu = *mtu;
		if (flush_timeout != NULL && *flush_timeout != fChannelConfig.flush_timeout)
			fChannelConfig.flush_timeout = *flush_timeout;
	} else if (result == l2cap_configuration_rsp::RESULT_FLOW_SPEC_REJECTED) {
		if (flow != NULL)
			fChannelConfig.outgoing_flow = *flow;
	} else if (result != l2cap_configuration_rsp::RESULT_SUCCESS) {
		ERROR("l2cap: unhandled configuration response! (result=%d)\n",
			result);
		return;
	}

	if ((flags & L2CAP_CFG_FLAG_CONTINUATION) != 0) {
		// More options are coming, just keep waiting.
		return;
	}

	if (ident == fCommandIdent)
		fCommandIdent = L2CAP_NULL_IDENT;

	if (result != l2cap_configuration_rsp::RESULT_SUCCESS) {
		// Resend configuration request to try again.
		_SendChannelConfig();
		return;
	}

	// We now have all options.
	fConfigState.out = ConfigState::DONE;

	if (fConfigState.in == ConfigState::DONE)
		_MarkEstablished();
}


status_t
L2capEndpoint::_MarkEstablished()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);

	fState = OPEN;
	fCommandWait.NotifyAll();

	status_t error = B_OK;
	if (gSocketModule->has_parent(socket)) {
		error = gSocketModule->set_connected(socket);
		if (error == B_OK) {
			release_sem(fAcceptSemaphore);
			fAcceptSemaphore = -1;
		} else {
			ERROR("%s: could not set endpoint %p connected: %s\n", __func__, this,
				strerror(error));
		}
	}

	return error;
}


void
L2capEndpoint::_HandleDisconnectionReq(uint8 ident, uint16 scid)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (scid != fDestinationChannelID) {
		ERROR("l2cap: unexpected disconnection req: invalid source channel (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	if (fState != WAIT_FOR_DISCONNECTION_RSP)
		fState = RECEIVED_DISCONNECTION_REQ;

	// The dcid/scid are the same as in the REQ command.
	status_t status = send_l2cap_disconnection_rsp(fConnection, ident, fChannelID, scid);
	if (status != B_OK) {
		// TODO?
		return;
	}

	// Our own request, if any, will not be answered now. The signal handler
	// holds a reference to the socket, so this one is not the last.
	if (_DropCommandIdent())
		gSocketModule->release_socket(socket);
	_MarkClosed();
}


void
L2capEndpoint::_HandleDisconnectionRsp(uint8 ident, uint16 dcid, uint16 scid)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != WAIT_FOR_DISCONNECTION_RSP) {
		ERROR("l2cap: unexpected disconnection rsp (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	if (dcid != fDestinationChannelID && scid != fChannelID) {
		ERROR("l2cap: unexpected disconnection rsp: mismatched CIDs (dcid=%d, scid=%d)\n",
			dcid, scid);
		return;
	}

	if (ident == fCommandIdent)
		fCommandIdent = L2CAP_NULL_IDENT;
	_MarkClosed();
}


void
L2capEndpoint::_MarkRefused(status_t error)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	fState = CLOSED;
	fConnectError = error;
	socket->error = error;
	fCommandWait.NotifyAll();
	gSocketModule->notify(socket, B_SELECT_ERROR, error);

	gL2capEndpointManager.UnbindFromChannel(this);
}


void
L2capEndpoint::_MarkClosed()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);

	fState = CLOSED;

	socket->error = ENOTCONN;
	gSocketModule->notify(socket, B_SELECT_ERROR, ENOTCONN);

	gL2capEndpointManager.UnbindFromChannel(this);
}
