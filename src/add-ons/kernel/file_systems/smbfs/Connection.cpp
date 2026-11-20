/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Connection.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>


// how long a session that could not be made is not tried again
static const bigtime_t kRetryDelay = 10 * 1000000LL;


/*static*/ void
Request::Callback(smb2_context* context, int status, void* data, void* cookie)
{
	Request* request = (Request*)cookie;
	request->finished = true;
	request->status = status;
	request->data = data;
}


//	#pragma mark - Connection


Connection::Connection()
	:
	fOptions(NULL),
	fIndex(0),
	fContext(NULL),
	fGeneration(0),
	fLastFailure(0),
	fLastError(0),
	fCancelFD(-1)
{
	pthread_mutex_init(&fLock, NULL);
}


Connection::~Connection()
{
	if (fContext != NULL)
		smb2_destroy_context(fContext);
	pthread_mutex_destroy(&fLock);
}


void
Connection::Init(const Options* options, int index)
{
	fOptions = options;
	fIndex = index;
}


void
Connection::Lock()
{
	pthread_mutex_lock(&fLock);
}


bool
Connection::TryLock()
{
	return pthread_mutex_trylock(&fLock) == 0;
}


void
Connection::Unlock()
{
	pthread_mutex_unlock(&fLock);
}


/*!	Returns 0 when there is a session with the server, making one if needed,
	and the error that prevented it otherwise.
*/
int
Connection::EnsureConnected()
{
	if (fContext != NULL)
		return 0;

	// When the server cannot be reached everything that is tried on the volume
	// ends up here, and should not wait for the same answer each time.
	if (fLastFailure != 0 && system_time() - fLastFailure < kRetryDelay)
		return fLastError;

	fContext = smb2_init_context();
	if (fContext == NULL)
		return -ENOMEM;

	smb2_set_timeout(fContext, fOptions->timeout);
	smb2_set_security_mode(fContext, SMB2_NEGOTIATE_SIGNING_ENABLED);
	if (fOptions->encrypt)
		smb2_set_seal(fContext, 1);

	if (!fOptions->user.empty())
		smb2_set_user(fContext, fOptions->user.c_str());
	if (!fOptions->password.empty())
		smb2_set_password(fContext, fOptions->password.c_str());
	if (!fOptions->domain.empty())
		smb2_set_domain(fContext, fOptions->domain.c_str());

	char host[256];
	if (gethostname(host, sizeof(host)) == 0 && host[0] != '\0') {
		host[sizeof(host) - 1] = '\0';
		smb2_set_workstation(fContext, host);
	}

	Request request;
	int status = smb2_connect_share_async(fContext, fOptions->server.c_str(),
		fOptions->share.c_str(),
		fOptions->user.empty() ? NULL : fOptions->user.c_str(),
		&Request::Callback, &request);
	if (status == 0) {
		if (Wait(request))
			status = request.status;
		else
			status = -ETIMEDOUT;
	}

	if (status != 0) {
		if (status > 0)
			status = -status;

		// libsmb2 has a refused connection for a refused logon; tell them
		// apart, as only one of them is worth trying again
		switch ((uint32)smb2_get_nterror(fContext)) {
			case SMB2_STATUS_LOGON_FAILURE:
			case SMB2_STATUS_ACCESS_DENIED:
			case SMB2_STATUS_ACCOUNT_DISABLED:
			case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
			case SMB2_STATUS_ACCOUNT_RESTRICTION:
			case SMB2_STATUS_PASSWORD_EXPIRED:
			case SMB2_STATUS_INVALID_LOGON_HOURS:
			case SMB2_STATUS_INVALID_WORKSTATION:
			case SMB2_STATUS_LOGON_TYPE_NOT_GRANTED:
				status = -EACCES;
				break;
			case SMB2_STATUS_BAD_NETWORK_NAME:
			case SMB2_STATUS_BAD_NETWORK_PATH:
				status = -ENOENT;
				break;
		}

		syslog(LOG_ERR, "smbfs: //%s/%s: connection %d failed: %s (%s)\n",
			fOptions->server.c_str(), fOptions->share.c_str(), fIndex,
			smb2_get_error(fContext), strerror(-status));

		// the request is on the stack, and libsmb2 still knows it
		smb2_destroy_context(fContext);
		fContext = NULL;

		fLastFailure = system_time();
		fLastError = status;
		return status;
	}

	fLastFailure = 0;
	fGeneration++;

	syslog(LOG_INFO, "smbfs: //%s/%s: connection %d established, SMB dialect "
		"%#x\n", fOptions->server.c_str(), fOptions->share.c_str(), fIndex,
		smb2_get_dialect(fContext));

	return 0;
}


/*!	Ends the session. Requests the server has not answered end with it, and
	do so before this returns, which is why a request can live on the stack
	of whoever waits for it.
*/
void
Connection::Disconnect(bool sayGoodbye)
{
	if (fContext == NULL)
		return;

	if (sayGoodbye) {
		Request request;
		if (smb2_disconnect_share_async(fContext, &Request::Callback,
				&request) == 0) {
			Wait(request);
		}
	}

	smb2_destroy_context(fContext);
	fContext = NULL;
}


/*!	Waits for the server's answer to \a request. Returns whether there is one;
	if not, the session is of no use anymore and the caller has to end it.
*/
bool
Connection::Wait(Request& request)
{
	const bigtime_t deadline = system_time()
		+ (fOptions->timeout + 5) * 1000000LL;

	while (!request.finished) {
		struct pollfd pollFDs[2];
		pollFDs[0].fd = smb2_get_fd(fContext);
		pollFDs[0].events = smb2_which_events(fContext);
		pollFDs[0].revents = 0;
		pollFDs[1].fd = fCancelFD;
		pollFDs[1].events = POLLIN;
		pollFDs[1].revents = 0;

		if (poll(pollFDs, fCancelFD >= 0 ? 2 : 1, 1000) < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}

		if ((pollFDs[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0
			&& fCancelFD >= 0) {
			break;
		}

		// without events too, that is where it finds requests that took
		// too long
		if (smb2_service(fContext, pollFDs[0].revents) < 0)
			break;

		if (system_time() > deadline)
			break;
	}

	return request.finished;
}


/*!	Whether \a status is what a request ends with when the session is lost,
	rather than something the server said about the request.
*/
/*static*/ bool
Connection::IsLost(int status)
{
	switch (status) {
		case -ETIMEDOUT:
		case -ENETRESET:
		case -ECONNRESET:
		case -ECONNABORTED:
		case -ENOTCONN:
		case -EPIPE:
		case -ENETDOWN:
		case -ENETUNREACH:
		case -EHOSTUNREACH:
		case -EHOSTDOWN:
			return true;
	}

	// what libsmb2 ends requests with that it gives up on
	return (uint32)status == SMB2_STATUS_SHUTDOWN
		|| (uint32)status == SMB2_STATUS_CANCELLED;
}


//	#pragma mark - ConnectionPool


ConnectionPool::ConnectionPool()
	:
	fConnections(NULL),
	fCount(0),
	fNext(0)
{
}


ConnectionPool::~ConnectionPool()
{
	delete[] fConnections;
}


void
ConnectionPool::Init(const Options* options)
{
	fCount = options->connections;
	fConnections = new Connection[fCount];
	for (int i = 0; i < fCount; i++)
		fConnections[i].Init(options, i);
}


void
ConnectionPool::DisconnectAll()
{
	for (int i = 0; i < fCount; i++) {
		fConnections[i].Lock();
		fConnections[i].Disconnect(true);
		fConnections[i].Unlock();
	}
}


/*!	Returns a session nobody is using, locked. One that exists is preferred
	over one that has to be made first, so there are only as many as were
	needed at the same time. When all are in use this waits for one.
*/
Connection*
ConnectionPool::Acquire()
{
	Connection* unconnected = NULL;

	for (int i = 0; i < fCount; i++) {
		Connection* connection = &fConnections[i];
		if (!connection->TryLock())
			continue;

		if (connection->IsConnected()) {
			if (unconnected != NULL)
				unconnected->Unlock();
			return connection;
		}

		if (unconnected == NULL)
			unconnected = connection;
		else
			connection->Unlock();
	}

	if (unconnected != NULL)
		return unconnected;

	Connection* connection
		= &fConnections[(uint32)atomic_add(&fNext, 1) % fCount];
	connection->Lock();
	return connection;
}
