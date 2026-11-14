/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Andrew Galante, haiku.galante@gmail.com
 *		Axel Dörfler, axeld@pinc-software.de
 *		Hugo Santos, hugosantos@gmail.com
 */
#ifndef TCP_ENDPOINT_H
#define TCP_ENDPOINT_H


#include "BufferQueue.h"
#include "EndpointManager.h"
#include "tcp.h"

#include <ProtocolUtilities.h>
#include <net_protocol.h>
#include <net_stack.h>
#include <condition_variable.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/OpenHashTable.h>

#include <stddef.h>


class TCPEndpoint : public net_protocol, public ProtocolSocket {
public:
						TCPEndpoint(net_socket* socket);
						~TCPEndpoint();

			status_t	InitCheck() const;

			status_t	Open();
			status_t	Close();
			void		Free();
			status_t	Connect(const struct sockaddr* address);
			status_t	Accept(struct net_socket** _acceptedSocket);
			status_t	Bind(const sockaddr* address);
			status_t	Unbind(struct sockaddr* address);
			status_t	Listen(int count);
			status_t	Shutdown(int direction);
			status_t	SendData(net_buffer* buffer);
			ssize_t		SendAvailable();
			status_t	ReadData(size_t numBytes, uint32 flags,
							net_buffer** _buffer);
			ssize_t		ReadAvailable();

			status_t	FillStat(struct net_stat* stat);

			status_t	SetSendBufferSize(size_t length);
			status_t	SetReceiveBufferSize(size_t length);

			status_t	GetOption(int option, void* value, int* _length);
			status_t	SetOption(int option, const void* value, int length);

			tcp_state	State() const { return fState; }
			bool		EndTimeWaitForNewConnection(
							const tcp_segment_header& segment);
			bool		IsBound() const;
			bool		IsLocal() const;

			status_t	DelayedAcknowledge();

			int32		SegmentReceived(tcp_segment_header& segment,
							net_buffer* buffer);
			status_t	ErrorReceived(net_error error, net_error_data* errorData,
							net_buffer* data);

			void		AbortIfLocalAddressRemoved();
			void		KeepAliveChanged();

			void		Dump() const;

private:
			void		_StartPersistTimer();
			void		_EnterTimeWait();
			void		_UpdateTimeWait();
			void		_Close();
			void		_CancelConnectionTimers();
			void		_UpdateKeepAliveTimer();
			void		_KeepAlive();
			status_t	_SendKeepAlive();
			bool		_IsWaitingForPeer() const;
			void		_Drop(status_t error, const char* reason);

			tcp_segment_header _PrepareSendSegment();
			bool		_ShouldSendSegment(tcp_segment_header& segment,
							uint32 length, uint32 segmentMaxSize,
							uint32 flightSize);
			status_t	_PrepareAndSend(tcp_segment_header& segment, net_buffer* buffer,
							bool isRetransmit);
			status_t	_SendAcknowledge(bool force = false);
			status_t	_SendReset(bool force = false);
			status_t	_SendQueued(bool force = false);
			status_t	_RetransmitSynchronizeAcknowledge();

			status_t	_Disconnect(bool closing);
			ssize_t		_AvailableData() const;
			void		_NotifyReader();
			bool		_ShouldReceive() const;
			status_t	_ClosedSendStatus() const;
			void		_HandleReset(status_t error);
			int32		_Spawn(TCPEndpoint* parent, tcp_segment_header& segment,
							net_buffer* buffer);
			int32		_ListenReceive(tcp_segment_header& segment,
							net_buffer* buffer);
			int32		_SpawnFromCookie(TCPEndpoint* parent,
							tcp_segment_header& segment, net_buffer* buffer,
							uint16 maxSegmentSize);
			int32		_SynchronizeSentReceive(tcp_segment_header& segment,
							net_buffer* buffer);
			int32		_SegmentReceived(tcp_segment_header& segment,
							net_buffer* buffer);
			int32		_Receive(tcp_segment_header& segment,
							net_buffer* buffer);
			int32		_ChallengeAcknowledge();
			void		_UpdateTimestamps(tcp_segment_header& segment,
							size_t segmentLength);
			void		_UpdateReceiveBuffer();
			void		_UpdateSendBuffer();
			void		_MarkEstablished();
			status_t	_WaitForEstablished(MutexLocker& lock,
							bigtime_t timeout);
			bool		_AddData(tcp_segment_header& segment,
							net_buffer* buffer);
			int			_MaxSegmentSize(const struct sockaddr* address) const;
			void		_PrepareReceivePath(tcp_segment_header& segment);
			status_t	_PrepareSendPath(const sockaddr* peer);
			void		_Acknowledged(tcp_segment_header& segment);
			void		_Retransmit();
			void		_UpdateRoundTripTime(int32 roundTripTime, int32 expectedSamples);
			void		_ResetSlowStart();
			uint32		_CubicCongestionEvent(uint32 flightSize);
			void		_CubicIncrease(uint32 bytesAcknowledged);
			void		_DuplicateAcknowledge(tcp_segment_header& segment);
			void		_UpdateScoreboard(const tcp_segment_header& segment);
			void		_PruneScoreboard();
			bool		_NextHole(tcp_sequence& start, uint32& length) const;
			bool		_RetransmitNextHole();
			uint32		_Pipe() const;
			void		_RecoverySend();
			uint32		_SkipReported();

	static	void		_TimeWaitTimer(net_timer* timer, void* _endpoint);
	static	void		_RetransmitTimer(net_timer* timer, void* _endpoint);
	static	void		_PersistTimer(net_timer* timer, void* _endpoint);
	static	void		_DelayedAcknowledgeTimer(net_timer* timer,
							void* _endpoint);
	static	void		_KeepAliveTimer(net_timer* timer, void* _endpoint);

	static	status_t	_WaitForCondition(ConditionVariable& condition,
							MutexLocker& locker, bigtime_t timeout);

private:
	TCPEndpoint*	fConnectionHashLink;
	TCPEndpoint*	fEndpointHashLink;
	friend class	EndpointManager;
	friend struct	ConnectionHashDefinition;
	friend class	EndpointHashDefinition;

	mutex			fLock;
	EndpointManager* fManager;
	ConditionVariable
					fReceiveCondition;
	ConditionVariable
					fSendCondition;
	sem_id			fAcceptSemaphore;
	uint8			fOptions;

	uint8			fSendWindowShift;
	uint8			fReceiveWindowShift;

	tcp_sequence	fSendUnacknowledged;
	tcp_sequence	fSendNext;
	tcp_sequence	fSendMax;
	tcp_sequence	fSendUrgentOffset;
	uint32			fSendWindow;
	uint32			fSendMaxWindow;
	uint32			fSendMaxSegmentSize;
	uint32			fSendMaxSegments;
	BufferQueue		fSendQueue;
	tcp_sequence	fLastAcknowledgeSent;
	tcp_sequence	fInitialSendSequence;
	tcp_sequence	fPreviousHighestAcknowledge;
	uint32			fDuplicateAcknowledgeCount;
	uint32			fPreviousFlightSize;
	tcp_sequence	fRecover;

	// SACK scoreboard (RFC 6675): the ranges above fSendUnacknowledged the
	// peer reported, sorted and disjoint, and the end of what fast recovery
	// retransmitted so far
	enum { kScoreboardSize = 16 };
	struct sack_range {
		tcp_sequence	left;
		tcp_sequence	right;
	};
	sack_range		fScoreboard[kScoreboardSize];
	int32			fScoreboardCount;
	bool			fScoreboardKeptAtTimeout;
		// a timeout kept the scoreboard, and nothing was acknowledged since
	tcp_sequence	fHighRetransmitted;
		// highest sequence sent when fast recovery began (RFC 6582)

	net_route		*fRoute;
		// TODO: don't use a net_route, but a net_route_info!!!
		// (the latter will automatically adapt to routing changes)

	tcp_sequence	fReceiveNext;
	tcp_sequence	fReceiveMaxAdvertised;
	uint32			fReceiveWindow;
	uint32			fReceiveMaxSegmentSize;
	BufferQueue		fReceiveQueue;
	bool			fFinishReceived;
	tcp_sequence	fFinishReceivedAt;
	tcp_sequence	fInitialReceiveSequence;

	// round trip time and retransmit timeout computation
	int32			fSmoothedRoundTripTime;
		// in ms, derived from fSmoothedRTT
	int32			fRoundTripVariation;
	bigtime_t		fSmoothedRTT;
	bigtime_t		fRTTVariation;
		// in microseconds; fSmoothedRTT < 0 until the first sample
	uint8			fPersistShift;
		// back-off of the persist timer while the window stays closed
	uint32			fSendTime;
	tcp_sequence	fRoundTripStartSequence;
	bigtime_t		fRetransmitTimeout;
	uint32			fRetransmitInitialCount;
	bigtime_t		fRetransmitStart;
		// when the peer stopped acknowledging data (0: it does)
	bigtime_t		fUserTimeout;
		// TCP_USER_TIMEOUT, 0 for TCP_MAX_RETRANSMIT_DURATION

	// keepalive
	bigtime_t		fLastReceiveTime;
	bigtime_t		fKeepAliveIdle;
	bigtime_t		fKeepAliveInterval;
	uint32			fKeepAliveCount;
	uint32			fKeepAliveProbes;

	uint32			fReceivedTimestamp;
	uint32			fTimestampOffset;
		// added to our timestamp values, different for every connection
	bigtime_t		fLastChallengeAcknowledge;

	tcp_sequence	fReceiveSizingReference;
	uint32			fReceiveSizingTimestamp;

	uint32			fCongestionWindow;
	uint32			fSlowStartThreshold;

	// CUBIC congestion avoidance (RFC 9438); times in ms
	uint32			fCubicMaxWindow;
		// W_max: the window before the last reduction
	uint32			fCubicOrigin;
	uint32			fCubicK;
	bigtime_t		fCubicEpochStart;
		// 0: no epoch started since the last reduction
	bigtime_t		fCubicLastAcknowledge;
	uint64			fCubicEstimate;
		// W_est, the window Reno would have, in bytes * 1024
	uint64			fCubicRemainder;
	bool			fCongestionWindowLimited;
		// the last send left data waiting for the congestion window

	tcp_state		fState;
	uint32			fFlags;

	// timer
	net_timer		fRetransmitTimer;
	net_timer		fPersistTimer;
	net_timer		fDelayedAcknowledgeTimer;
	net_timer		fTimeWaitTimer;
	net_timer		fKeepAliveTimer;
};

#endif	// TCP_ENDPOINT_H
