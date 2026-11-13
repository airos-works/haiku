/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Software receive coalescing ("GRO") for the consumer thread of a device
	interface: consecutive in-order TCP segments of one connection in a batch
	of received frames are merged into one buffer before IP and TCP see them,
	so that the stack runs once per batch of a connection instead of once per
	1448 bytes (airos-works/haiku#124).

	The rules follow Linux's tcp_gro_receive(): only IPv4 without options or
	fragmentation, only segments with ACK (and PSH) set and data in them, the
	same addresses, ports, TOS, TTL, acknowledgement, window and TCP options
	(timestamps included), sequence numbers that continue each other, and at
	most 64 KB. The checksums of every segment are verified before it is
	merged; the merged buffer is marked as verified, since its headers no
	longer match the checksums.
*/


#include "coalesce.h"

#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <net_buffer.h>
#include <net_stat.h>
#include <net_stack.h>
#include <util/list.h>

#include "stack_private.h"
#include "utility.h"


static const uint32 kMaxCoalescedSize = 65535;

enum {
	kTCPFlagFinish		= 0x01,
	kTCPFlagSynchronize	= 0x02,
	kTCPFlagReset		= 0x04,
	kTCPFlagPush		= 0x08,
	kTCPFlagAcknowledge	= 0x10,
	kTCPFlagUrgent		= 0x20
};


struct segment_info {
	uint8	headers[20 + 60];
		// the IPv4 header (20 bytes) and the TCP header with its options
	uint32	headerLength;
		// both headers
	uint32	tcpHeaderLength;
	uint32	payload;
	uint32	sequence;
	uint32	totalLength;
};


static inline uint16
read16(const uint8* data)
{
	return (uint16)((data[0] << 8) | data[1]);
}


static inline uint32
read32(const uint8* data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16)
		| ((uint32)data[2] << 8) | data[3];
}


static inline uint16
fold(uint32 sum)
{
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16)sum;
}


/*!	Reads and checks the headers of \a buffer. Returns false when the buffer
	is no candidate for coalescing (including bad checksums: such a segment is
	left alone, and TCP drops it).
*/
static bool
parse_segment(net_buffer* buffer, segment_info& info)
{
	if (buffer->type != B_NET_FRAME_TYPE_IPV4
		|| buffer->interface_address != NULL
		|| (buffer->msg_flags & (MSG_BCAST | MSG_MCAST)) != 0
		|| buffer->size < 40 || buffer->size > kMaxCoalescedSize)
		return false;

	uint8* ip = info.headers;
	if (gNetBufferModule.read(buffer, 0, ip, 20) != B_OK)
		return false;

	// IPv4 without options, not fragmented, carrying TCP
	if (ip[0] != 0x45 || ip[9] != IPPROTO_TCP
		|| (read16(ip + 6) & 0x3fff) != 0)
		return false;

	info.totalLength = read16(ip + 2);
	if (info.totalLength != buffer->size)
		return false;

	if ((buffer->buffer_flags & NET_BUFFER_L3_CHECKSUM_VALID) == 0
		&& fold(compute_checksum(ip, 20)) != 0xffff)
		return false;

	uint8* tcp = ip + 20;
	if (gNetBufferModule.read(buffer, 20, tcp, 20) != B_OK)
		return false;

	info.tcpHeaderLength = (tcp[12] >> 4) * 4;
	if (info.tcpHeaderLength < 20 || 20 + info.tcpHeaderLength > buffer->size)
		return false;
	if (info.tcpHeaderLength > 20
		&& gNetBufferModule.read(buffer, 40, tcp + 20,
			info.tcpHeaderLength - 20) != B_OK)
		return false;

	// only plain data segments: ACK, maybe PSH, nothing else
	uint8 flags = tcp[13];
	if ((flags & ~(kTCPFlagAcknowledge | kTCPFlagPush)) != 0
		|| (flags & kTCPFlagAcknowledge) == 0)
		return false;

	info.headerLength = 20 + info.tcpHeaderLength;
	info.payload = buffer->size - info.headerLength;
	if (info.payload == 0)
		return false;

	info.sequence = read32(tcp + 4);

	if ((buffer->buffer_flags & NET_BUFFER_L4_CHECKSUM_VALID) == 0) {
		// pseudo header, then the TCP header and data
		uint8 pseudo[12];
		memcpy(pseudo, ip + 12, 8);
		pseudo[8] = 0;
		pseudo[9] = IPPROTO_TCP;
		uint16 tcpLength = B_HOST_TO_BENDIAN_INT16(
			(uint16)(buffer->size - 20));
		memcpy(pseudo + 10, &tcpLength, 2);

		uint32 sum = compute_checksum(pseudo, sizeof(pseudo));
		sum += (uint16)gNetBufferModule.checksum(buffer, 20,
			buffer->size - 20, false);
		if (fold(sum) != 0xffff)
			return false;
	}

	return true;
}


/*!	Whether \a next continues \a first: same flow, same header contents but
	for the IP length, ID and checksum, the TCP sequence number and checksum,
	and the PSH flag.
*/
static bool
continues(const segment_info& first, const segment_info& next)
{
	if (next.headerLength != first.headerLength
		|| first.totalLength + next.payload > kMaxCoalescedSize
		|| next.sequence != first.sequence + first.payload)
		return false;

	const uint8* a = first.headers;
	const uint8* b = next.headers;

	// IPv4: TOS, flags (DF), TTL, protocol, addresses
	if (a[1] != b[1] || (read16(a + 6) != read16(b + 6)) || a[8] != b[8]
		|| memcmp(a + 12, b + 12, 8) != 0)
		return false;

	// TCP: ports, acknowledgement, header length, window, urgent pointer and
	// options; the flags may differ in PSH only
	const uint8* ta = a + 20;
	const uint8* tb = b + 20;
	if (memcmp(ta, tb, 4) != 0 || memcmp(ta + 8, tb + 8, 5) != 0
		|| ((ta[13] ^ tb[13]) & ~kTCPFlagPush) != 0
		|| memcmp(ta + 14, tb + 14, 2) != 0
		|| memcmp(ta + 18, tb + 18, 2) != 0
		|| memcmp(ta + 20, tb + 20, first.tcpHeaderLength - 20) != 0)
		return false;

	return true;
}


/*!	Appends \a next (already checked with continues()) to \a first. Returns
	false if the buffers could not be merged; \a next is then left alone.
*/
static bool
append_segment(net_buffer* first, segment_info& firstInfo, net_buffer* next,
	const segment_info& nextInfo)
{
	if (gNetBufferModule.remove_header(next, nextInfo.headerLength) != B_OK)
		return false;

	if (gNetBufferModule.merge(first, next, true) != B_OK) {
		// put the headers back; the caller passes the buffer on as it was
		gNetBufferModule.prepend_size(next, nextInfo.headerLength, NULL);
		gNetBufferModule.write(next, 0, nextInfo.headers,
			nextInfo.headerLength);
		return false;
	}

	firstInfo.totalLength += nextInfo.payload;
	firstInfo.payload += nextInfo.payload;

	uint16 totalLength = B_HOST_TO_BENDIAN_INT16(
		(uint16)firstInfo.totalLength);
	memcpy(firstInfo.headers + 2, &totalLength, 2);
	gNetBufferModule.write(first, 2, &totalLength, 2);

	uint8 flags = firstInfo.headers[20 + 13] | (nextInfo.headers[20 + 13]
		& kTCPFlagPush);
	if (flags != firstInfo.headers[20 + 13]) {
		firstInfo.headers[20 + 13] = flags;
		gNetBufferModule.write(first, 20 + 13, &flags, 1);
	}

	// the checksums in the headers no longer match, but we checked them all
	first->buffer_flags |= NET_BUFFER_L3_CHECKSUM_VALID
		| NET_BUFFER_L4_CHECKSUM_VALID;
	NET_STAT_INC(&gNetStackModule, tcp_segments_coalesced);
	return true;
}


void
coalesce_tcp_segments(struct list* batch)
{
	net_buffer* first = NULL;
	segment_info firstInfo;
	segment_info info;

	net_buffer* buffer = (net_buffer*)list_get_first_item(batch);
	while (buffer != NULL) {
		net_buffer* next = (net_buffer*)list_get_next_item(batch, buffer);

		if (!parse_segment(buffer, info)) {
			first = NULL;
			buffer = next;
			continue;
		}

		if (first != NULL && continues(firstInfo, info)) {
			list_remove_item(batch, buffer);
			if (append_segment(first, firstInfo, buffer, info)) {
				buffer = next;
				continue;
			}

			// could not merge: keep it where it was, and start over with it
			list_insert_item_before(batch, next, buffer);
		}

		first = buffer;
		firstInfo = info;
		buffer = next;
	}
}
