/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CLIENT_MESSAGE_QUEUE_H
#define CLIENT_MESSAGE_QUEUE_H


#include <Locker.h>
#include <OS.h>


class BMessage;


/*!	Delivers messages to client loopers without ever waiting for them.

	app_server sends input events and window messages (_UPDATE_,
	B_QUIT_REQUESTED, ...) with a timeout of 0: it must not wait on a client.
	A busy window does not read its port, though, and once that is full
	(200 messages, which pointer moves alone fill in a second or two) a send
	fails. Messages that fail that way are kept here instead, per port, and
	sent from a thread of their own as soon as the port has room again;
	whatever else is sent to that port meanwhile queues behind them, so the
	client sees everything in order.

	Pointer moves may be dropped: while a port has messages waiting, a new
	move replaces a move waiting at the end of its queue, and a queue that
	reaches its limit makes room by dropping moves first. A queue goes when
	its port does.
*/
class ClientMessageQueue {
public:
			enum Kind {
				kDroppable,
					// a pointer move: a newer one stands in for it
				kNormal,
					// kept unless the port's queue is full of these
				kKeep
					// kept even then: the window sends only one at a time
					// and does not send it again (_UPDATE_)
			};

	static	ClientMessageQueue&	Default();

			status_t			Send(port_id port, team_id team, int32 token,
									const BMessage* message,
									Kind kind = kNormal);
									// B_OK when sent or queued; else what
									// sending gave (B_BAD_PORT_ID: the
									// port is gone), or B_WOULD_BLOCK when
									// the queue is full and this message
									// may not push anything out

private:
								ClientMessageQueue();

			struct Pending;
			struct PortQueue;

			status_t			_SendNow(port_id port, team_id team,
									int32 token, const BMessage* message);
			PortQueue*			_FindQueue(port_id port);
			status_t			_Enqueue(PortQueue* queue, team_id team,
									int32 token, const BMessage* message,
									Kind kind);
			bool				_Flush(PortQueue* queue, bool& progress);
			void				_DeleteQueue(PortQueue* queue);

	static	status_t			_ThreadEntry(void* data);
			void				_Thread();

private:
			BLocker				fLock;
			PortQueue*			fQueues;
				// few at any time: those of clients that are busy right now
			int32				fQueuedPorts;
				// ports with messages waiting; read without the lock for
				// the common case of nothing waiting anywhere
			sem_id				fWakeUp;
			thread_id			fThread;
			int32				fDropped;
};


#endif	// CLIENT_MESSAGE_QUEUE_H
