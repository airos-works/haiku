/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_NCM_NTB_H_
#define _USB_NCM_NTB_H_


/*!	Network Transfer Blocks of the USB CDC Network Control Model (NCM 1.0,
	chapter 3): building them for the bulk OUT pipe and taking them apart
	from the bulk IN pipe. Plain C++ without kernel headers, so that the
	host test in src/tests/add-ons/kernel/drivers/usb_ncm can run it.
*/


#include <stddef.h>
#include <stdint.h>


namespace ncm {


// signatures, little endian
static const uint32_t kNTH16Signature = 0x484d434e;	// "NCMH"
static const uint32_t kNTH32Signature = 0x686d636e;	// "ncmh"
static const uint32_t kNDP16Signature = 0x304d434e;	// "NCM0", no CRC
static const uint32_t kNDP32Signature = 0x306d636e;	// "ncm0", no CRC

static const size_t kNTH16Size = 12;
static const size_t kNTH32Size = 16;
static const size_t kNDP16HeaderSize = 8;
static const size_t kNDP32HeaderSize = 16;
static const size_t kNDP16EntrySize = 4;
static const size_t kNDP32EntrySize = 8;

static const size_t kEthernetHeaderSize = 14;

// an NTB that chains more NDPs than this is taken as broken
static const uint32_t kMaxNDPs = 16;


static inline uint16_t
read16(const uint8_t* p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}


static inline uint32_t
read32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}


static inline void
write16(uint8_t* p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}


static inline void
write32(uint8_t* p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}


struct datagram {
	uint32_t	offset;
	uint32_t	length;
};


struct parse_stats {
	uint32_t	ndps;
		// NDPs walked
	uint32_t	bad_datagrams;
		// entries pointing outside the block, or shorter than an Ethernet
		// header, or longer than a frame
	uint32_t	truncated;
		// good datagrams that did not fit into the caller's array
};


enum parse_result {
	PARSE_OK = 0,
	PARSE_SHORT,
		// shorter than its header
	PARSE_BAD_SIGNATURE,
	PARSE_BAD_HEADER,
		// header length or block length wrong
	PARSE_BAD_NDP
		// an NDP outside the block, with a wrong signature or length
};


/*!	Takes apart the NTB of \a length bytes at \a data, received from the bulk
	IN pipe, 16 or 32 bit format as its header says. The datagrams found go
	into \a datagrams, at most \a maxDatagrams of them; \a _count returns how
	many. Each lies within the block and is between an Ethernet header and
	\a maxFrameLength long; bad entries are skipped and counted, as are good
	ones beyond \a maxDatagrams. A broken NTH or NDP ends the walk: the
	datagrams of the NDPs before it are still returned.
*/
parse_result parse_ntb(const uint8_t* data, size_t length,
	uint32_t maxFrameLength, datagram* datagrams, uint32_t maxDatagrams,
	uint32_t* _count, parse_stats* stats);


/*!	Builds one NTB in a buffer: the header, one NDP right after it, room
	reserved there for up to maxDatagrams entries, then the datagrams. The NDP
	first is what Linux' cdc_ncm and FreeBSD's cdce send, so functions know
	it.

	wNdpOutDivisor and wNdpOutPayloadRemainder place the datagram's payload,
	the part after its 14 byte Ethernet header (the IP header is what a
	function wants aligned): each datagram starts where
	(offset + 14) % divisor == remainder. Linux' cdc_ncm and FreeBSD's cdce
	do the same. A Realtek RTL8156 asks for divisor 4, remainder 2, so its
	frames start on 4 byte boundaries; while they started 2 bytes later,
	none of them reached the wire, without an error on the USB side.
*/
class NTBWriter {
public:
								NTBWriter();

			/*!	\a capacity is the largest NTB to build (at most
				dwNtbOutMaxSize); the buffer must have one byte more, for the
				padding Finish() may add. \a divisor and \a remainder are
				wNdpOutDivisor and wNdpOutPayloadRemainder as the function
				gives them (see above), \a ndpAlignment is
				wNdpOutAlignment. Returns false for parameters that cannot
				work. */
			bool				Init(bool ntb32, size_t capacity,
									uint16_t divisor, uint16_t remainder,
									uint16_t ndpAlignment,
									uint32_t maxDatagrams);

			/*!	Starts an empty NTB in \a buffer. */
			void				Start(uint8_t* buffer);

			bool				IsEmpty() const { return fCount == 0; }
			uint32_t			Count() const { return fCount; }
			size_t				Used() const { return fEnd; }
			size_t				Capacity() const { return fCapacity; }
			uint32_t			MaxDatagrams() const { return fMaxDatagrams; }
			uint16_t			Divisor() const { return fDivisor; }
			uint16_t			PayloadRemainder() const
									{ return fPayloadRemainder; }
			uint16_t			NDPAlignment() const { return fNDPAlignment; }
			/*!	Where datagrams start: offset % Divisor() is this. */
			uint16_t			FrameRemainder() const { return fRemainder; }
			size_t				NDPOffset() const { return fNDPOffset; }

			/*!	Whether a datagram of \a length bytes still fits. */
			bool				CanAdd(size_t length) const;

			/*!	Reserves room for a datagram of \a length bytes and returns
				where to put it, or NULL when it does not fit. */
			uint8_t*			Add(size_t length);

			/*!	Takes back the datagram the last Add() made room for. */
			void				RemoveLast();

			/*!	Fills in the header and the NDP and returns the length to
				transfer. A block that would end exactly on a packet
				boundary of \a maxPacketSize while shorter than the
				function's \a deviceMaxSize gets one byte of padding, so the
				function sees the end without a zero length packet (NCM 1.0,
				3.8.2). */
			size_t				Finish(uint16_t sequence,
									uint16_t maxPacketSize,
									uint32_t deviceMaxSize);

private:
			size_t				_AlignedOffset(size_t offset) const;

			uint8_t*			fBuffer;
			bool				fNTB32;
			size_t				fCapacity;
			uint16_t			fDivisor;
			uint16_t			fRemainder;
				// of the datagram start, from the payload remainder
			uint16_t			fPayloadRemainder;
			uint16_t			fNDPAlignment;
			uint32_t			fMaxDatagrams;
			size_t				fNDPOffset;
			size_t				fFirstDatagram;
			size_t				fEnd;
			size_t				fPreviousEnd;
			uint32_t			fCount;
};


}	// namespace ncm


#endif	// _USB_NCM_NTB_H_
