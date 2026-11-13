/*
 * Copyright 2006, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_STAT_H
#define NET_STAT_H


#include <OS.h>

#include <sys/socket.h>


#define NET_STAT_SOCKET		1
#define NET_STAT_PROTOCOL	2


typedef struct net_stat {
	int		family;
	int		type;
	int		protocol;
	char	state[B_OS_NAME_LENGTH];
	team_id	owner;
	struct	sockaddr_storage address;
	struct	sockaddr_storage peer;
	size_t	receive_queue_size;
	size_t	send_queue_size;
} net_stat;


/*!	Counters of the whole stack, as "netstat -s" shows them. Read them with
	_kern_generic_syscall(NET_STAT_SYSCALLS, NET_STAT_GET_PROTOCOL_STATS,
	&stats, sizeof(stats)); a smaller buffer gets the first fields only, so
	fields are only ever added at the end.
*/
typedef struct net_protocol_stats {
	// IPv4
	uint64	ip_received;
	uint64	ip_header_errors;
	uint64	ip_no_protocol;
	uint64	ip_fragments_received;
	uint64	ip_reassembled;
	uint64	ip_reassembly_dropped;
		// fragments thrown away: timed out, over a limit
	uint64	ip_sent;

	// UDP
	uint64	udp_received;
	uint64	udp_checksum_errors;
	uint64	udp_no_port;
	uint64	udp_receive_buffer_errors;
		// datagrams dropped because the socket's queue was full
	uint64	udp_sent;

	// TCP
	uint64	tcp_received;
	uint64	tcp_checksum_errors;
	uint64	tcp_sent;
	uint64	tcp_retransmitted;
	uint64	tcp_resets_sent;
	uint64	tcp_challenge_acks;
	uint64	tcp_connections_refused;
		// SYNs refused because a listen queue was full

	// raw and ICMP sockets
	uint64	raw_receive_buffer_errors;
} net_protocol_stats;

#define NET_STAT_SYSCALLS				"network/stats"
#define NET_STAT_GET_PROTOCOL_STATS		1

#ifdef _KERNEL_MODE
#	define NET_STAT_ADD(stack, field, count) \
		atomic_add64((int64*)&(stack)->protocol_stats->field, (count))
#	define NET_STAT_INC(stack, field)	NET_STAT_ADD(stack, field, 1)
#endif

#endif	// NET_STAT_H
