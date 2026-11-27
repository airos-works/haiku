/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ClientMessageQueue.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Message.h>
#include <Messenger.h>

#include <MessagePrivate.h>


// Messages one port may have waiting. A window that is stuck for good keeps
// at most this many; keys and clicks typed ahead of a busy one fit easily.
static const int32 kMaxQueuedPerPort = 1024;

// How often a port that had no room is tried again: soon at first, then
// less often while none of the waiting clients reads anything.
static const bigtime_t kMinRetryInterval = 5000;
static const bigtime_t kMaxRetryInterval = 100000;


struct ClientMessageQueue::Pending {
	Pending*	next;
	BMessage*	message;
	team_id		team;
	int32		token;
	bool		droppable;
};


struct ClientMessageQueue::PortQueue {
	PortQueue*	next;
	port_id		port;
	Pending*	head;
	Pending*	tail;
	int32		count;
};


ClientMessageQueue&
ClientMessageQueue::Default()
{
	static ClientMessageQueue sDefault;
	return sDefault;
}


ClientMessageQueue::ClientMessageQueue()
	:
	fLock("client message queue"),
	fQueues(NULL),
	fQueuedPorts(0),
	fWakeUp(-1),
	fThread(-1),
	fDropped(0)
{
}


status_t
ClientMessageQueue::Send(port_id port, team_id team, int32 token,
	const BMessage* message, Kind kind)
{
	if (atomic_get(&fQueuedPorts) == 0) {
		// Nothing is waiting anywhere, so nothing can be overtaken.
		status_t status = _SendNow(port, team, token, message);
		if (status != B_WOULD_BLOCK)
			return status;
	}

	BAutolock _(fLock);

	PortQueue* queue = _FindQueue(port);
	if (queue == NULL || queue->count == 0) {
		// Writing never waits, so it can be done with the lock held; that
		// keeps the retry thread from getting in between.
		status_t status = _SendNow(port, team, token, message);
		if (status != B_WOULD_BLOCK)
			return status;

		if (queue == NULL) {
			queue = new(std::nothrow) PortQueue;
			if (queue == NULL)
				return B_NO_MEMORY;
			queue->port = port;
			queue->head = queue->tail = NULL;
			queue->count = 0;
			queue->next = fQueues;
			fQueues = queue;
		}
	}

	return _Enqueue(queue, team, token, message, kind);
}


status_t
ClientMessageQueue::_SendNow(port_id port, team_id team, int32 token,
	const BMessage* message)
{
	BMessenger replyTo;
	BMessage::Private messagePrivate(const_cast<BMessage*>(message));
	status_t status = messagePrivate.SendMessage(port, team, token, 0, false,
		replyTo);
	if (status == B_TIMED_OUT)
		status = B_WOULD_BLOCK;
	return status;
}


ClientMessageQueue::PortQueue*
ClientMessageQueue::_FindQueue(port_id port)
{
	for (PortQueue* queue = fQueues; queue != NULL; queue = queue->next) {
		if (queue->port == port)
			return queue;
	}
	return NULL;
}


status_t
ClientMessageQueue::_Enqueue(PortQueue* queue, team_id team, int32 token,
	const BMessage* message, Kind kind)
{
	const bool droppable = kind == kDroppable;
	if (droppable && queue->tail != NULL && queue->tail->droppable
		&& queue->tail->token == token
		&& queue->tail->message->what == message->what) {
		// The client has not seen the move waiting at the end yet: this
		// newer one says all it would have said.
		*queue->tail->message = *message;
		atomic_add(&fDropped, 1);
		return B_OK;
	}

	if (queue->count >= kMaxQueuedPerPort && kind != kKeep) {
		// Make room by dropping the oldest move.
		Pending* previous = NULL;
		Pending* pending = queue->head;
		while (pending != NULL && !pending->droppable) {
			previous = pending;
			pending = pending->next;
		}
		if (pending == NULL) {
			if (atomic_add(&fDropped, 1) % 100 == 0) {
				fprintf(stderr, "app_server: port %" B_PRId32 " has %" B_PRId32
					" messages waiting, dropping '%.4s'\n", queue->port,
					queue->count, (const char*)&message->what);
			}
			return B_WOULD_BLOCK;
		}
		if (previous != NULL)
			previous->next = pending->next;
		else
			queue->head = pending->next;
		if (queue->tail == pending)
			queue->tail = previous;
		queue->count--;
		delete pending->message;
		delete pending;
		atomic_add(&fDropped, 1);
	}

	Pending* pending = new(std::nothrow) Pending;
	BMessage* copy = new(std::nothrow) BMessage(*message);
	if (pending == NULL || copy == NULL) {
		delete pending;
		delete copy;
		return B_NO_MEMORY;
	}
	pending->next = NULL;
	pending->message = copy;
	pending->team = team;
	pending->token = token;
	pending->droppable = droppable;

	if (queue->tail != NULL)
		queue->tail->next = pending;
	else
		queue->head = pending;
	queue->tail = pending;
	if (queue->count++ == 0)
		atomic_add(&fQueuedPorts, 1);

	if (fThread < 0) {
		fWakeUp = create_sem(0, "client message retry");
		fThread = spawn_thread(_ThreadEntry, "client message retry",
			B_URGENT_DISPLAY_PRIORITY, this);
		if (fWakeUp < 0 || fThread < 0) {
			fprintf(stderr, "app_server: no thread to deliver queued messages "
				"to clients\n");
			delete_sem(fWakeUp);
			fWakeUp = -1;
			fThread = -1;
		} else
			resume_thread(fThread);
	}
	if (queue->count == 1)
		release_sem_etc(fWakeUp, 1, B_DO_NOT_RESCHEDULE);

	return B_OK;
}


/*!	Sends what the port has room for, in order. Returns false when the port
	is gone. Called with the lock held.
*/
bool
ClientMessageQueue::_Flush(PortQueue* queue, bool& progress)
{
	while (Pending* pending = queue->head) {
		status_t status = _SendNow(queue->port, pending->team, pending->token,
			pending->message);
		if (status == B_WOULD_BLOCK)
			return true;
		if (status == B_BAD_PORT_ID)
			return false;
		if (status != B_OK) {
			fprintf(stderr, "app_server: could not deliver '%.4s' to port %"
				B_PRId32 ": %s\n", (const char*)&pending->message->what,
				queue->port, strerror(status));
		}

		queue->head = pending->next;
		if (queue->head == NULL)
			queue->tail = NULL;
		if (--queue->count == 0)
			atomic_add(&fQueuedPorts, -1);
		delete pending->message;
		delete pending;
		progress = true;
	}
	return true;
}


void
ClientMessageQueue::_DeleteQueue(PortQueue* queue)
{
	PortQueue** link = &fQueues;
	while (*link != NULL && *link != queue)
		link = &(*link)->next;
	if (*link == NULL)
		return;
	*link = queue->next;

	if (queue->count > 0)
		atomic_add(&fQueuedPorts, -1);
	while (Pending* pending = queue->head) {
		queue->head = pending->next;
		delete pending->message;
		delete pending;
	}
	delete queue;
}


status_t
ClientMessageQueue::_ThreadEntry(void* data)
{
	((ClientMessageQueue*)data)->_Thread();
	return B_OK;
}


void
ClientMessageQueue::_Thread()
{
	bigtime_t interval = kMinRetryInterval;

	while (true) {
		bigtime_t timeout = atomic_get(&fQueuedPorts) > 0
			? interval : B_INFINITE_TIMEOUT;
		status_t status = acquire_sem_etc(fWakeUp, 1, B_RELATIVE_TIMEOUT,
			timeout);
		if (status == B_BAD_SEM_ID)
			return;
		if (status == B_OK) {
			// something new is waiting: start trying it soon
			interval = kMinRetryInterval;
		}

		BAutolock _(fLock);

		bool progress = false;
		PortQueue* queue = fQueues;
		while (queue != NULL) {
			PortQueue* next = queue->next;
			if (!_Flush(queue, progress)) {
				// The client is gone, and with it what it had not read.
				_DeleteQueue(queue);
			} else if (queue->count == 0) {
				// Most queues are needed once, for a short while.
				_DeleteQueue(queue);
			}
			queue = next;
		}

		if (progress)
			interval = kMinRetryInterval;
		else if (status == B_TIMED_OUT)
			interval = min_c(interval * 2, kMaxRetryInterval);
	}
}
