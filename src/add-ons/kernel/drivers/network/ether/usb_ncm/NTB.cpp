/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "NTB.h"

#include <string.h>


namespace ncm {


static inline bool
is_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}


parse_result
parse_ntb(const uint8_t* data, size_t length, uint32_t maxFrameLength,
	datagram* datagrams, uint32_t maxDatagrams, uint32_t* _count,
	parse_stats* stats)
{
	*_count = 0;
	if (length < 4)
		return PARSE_SHORT;

	bool ntb32;
	uint32_t signature = read32(data);
	if (signature == kNTH16Signature)
		ntb32 = false;
	else if (signature == kNTH32Signature)
		ntb32 = true;
	else
		return PARSE_BAD_SIGNATURE;

	const size_t headerSize = ntb32 ? kNTH32Size : kNTH16Size;
	if (length < headerSize)
		return PARSE_SHORT;
	if (read16(data + 4) != headerSize)
		return PARSE_BAD_HEADER;

	size_t blockLength;
	size_t ndpIndex;
	if (ntb32) {
		blockLength = read32(data + 8);
		ndpIndex = read32(data + 12);
	} else {
		blockLength = read16(data + 8);
		ndpIndex = read16(data + 10);
	}

	// A block length of 0 means that a short packet ends the block (NCM 1.0,
	// table 3-1). A block that claims more than arrived was cut short: what
	// did arrive is still used, each datagram is checked against it.
	size_t limit = length;
	if (blockLength != 0) {
		if (blockLength < headerSize)
			return PARSE_BAD_HEADER;
		if (blockLength < limit)
			limit = blockLength;
	}

	const size_t ndpHeaderSize = ntb32 ? kNDP32HeaderSize : kNDP16HeaderSize;
	const size_t entrySize = ntb32 ? kNDP32EntrySize : kNDP16EntrySize;
	uint32_t count = 0;
	size_t visited[kMaxNDPs];

	for (uint32_t ndp = 0; ndp < kMaxNDPs; ndp++) {
		// an NDP that leads back to one already seen would repeat its
		// datagrams
		for (uint32_t i = 0; i < ndp; i++) {
			if (visited[i] == ndpIndex) {
				*_count = count;
				return PARSE_BAD_NDP;
			}
		}
		visited[ndp] = ndpIndex;

		if (ndpIndex < headerSize || ndpIndex > limit
			|| ndpHeaderSize > limit - ndpIndex) {
			*_count = count;
			return PARSE_BAD_NDP;
		}

		const uint8_t* header = data + ndpIndex;
		uint32_t ndpSignature = read32(header);
		// The last byte says whether each datagram ends in a CRC-32; it is
		// turned off, but a function that sends it anyway is understood.
		bool crc;
		if ((ndpSignature & 0x00ffffff)
				== ((ntb32 ? kNDP32Signature : kNDP16Signature) & 0x00ffffff)
			&& ((ndpSignature >> 24) == '0' || (ndpSignature >> 24) == '1')) {
			crc = (ndpSignature >> 24) == '1';
		} else {
			*_count = count;
			return PARSE_BAD_NDP;
		}

		size_t ndpLength = read16(header + 4);
		size_t nextIndex = ntb32 ? read32(header + 8) : read16(header + 6);
		if (ndpLength < ndpHeaderSize + 2 * entrySize
			|| (ndpLength % (ntb32 ? 8 : 4)) != 0
			|| ndpLength > limit - ndpIndex) {
			*_count = count;
			return PARSE_BAD_NDP;
		}

		if (stats != NULL)
			stats->ndps++;

		const size_t entries = (ndpLength - ndpHeaderSize) / entrySize;
		const uint8_t* entry = header + ndpHeaderSize;
		for (size_t i = 0; i < entries; i++, entry += entrySize) {
			size_t offset;
			size_t datagramLength;
			if (ntb32) {
				offset = read32(entry);
				datagramLength = read32(entry + 4);
			} else {
				offset = read16(entry);
				datagramLength = read16(entry + 2);
			}

			// a zero index or length ends the table
			if (offset == 0 || datagramLength == 0)
				break;

			if (crc) {
				if (datagramLength < 4) {
					if (stats != NULL)
						stats->bad_datagrams++;
					continue;
				}
				datagramLength -= 4;
			}

			if (datagramLength < kEthernetHeaderSize
				|| datagramLength > maxFrameLength
				|| offset < headerSize || offset > limit
				|| datagramLength > limit - offset) {
				if (stats != NULL)
					stats->bad_datagrams++;
				continue;
			}

			if (count == maxDatagrams) {
				if (stats != NULL)
					stats->truncated++;
				continue;
			}

			datagrams[count].offset = (uint32_t)offset;
			datagrams[count].length = (uint32_t)datagramLength;
			count++;
		}

		if (nextIndex == 0) {
			*_count = count;
			return PARSE_OK;
		}
		ndpIndex = nextIndex;
	}

	// a chain this long is more likely a loop
	*_count = count;
	return PARSE_BAD_NDP;
}


//	#pragma mark - NTBWriter


NTBWriter::NTBWriter()
	:
	fBuffer(NULL),
	fNTB32(false),
	fCapacity(0),
	fDivisor(4),
	fRemainder(0),
	fPayloadRemainder(0),
	fNDPAlignment(4),
	fMaxDatagrams(0),
	fNDPOffset(0),
	fFirstDatagram(0),
	fEnd(0),
	fPreviousEnd(0),
	fCount(0)
{
}


bool
NTBWriter::Init(bool ntb32, size_t capacity, uint16_t divisor,
	uint16_t remainder, uint16_t ndpAlignment, uint32_t maxDatagrams)
{
	// What the function asks for, made sane the way the hosts that came
	// before do it: powers of two of at least 4 bytes.
	if (!is_power_of_two(divisor) || divisor < 4 || divisor >= capacity)
		divisor = 4;
	if (remainder >= divisor)
		remainder = 0;
	if (!is_power_of_two(ndpAlignment) || ndpAlignment < 4
		|| ndpAlignment >= capacity) {
		ndpAlignment = 4;
	}
	if (maxDatagrams == 0)
		return false;

	const size_t headerSize = ntb32 ? kNTH32Size : kNTH16Size;
	const size_t ndpSize = (ntb32 ? kNDP32HeaderSize : kNDP16HeaderSize)
		+ (maxDatagrams + 1) * (ntb32 ? kNDP32EntrySize : kNDP16EntrySize);
	const size_t ndpOffset = (headerSize + ndpAlignment - 1)
		& ~(size_t)(ndpAlignment - 1);

	// a block of 16 bit format cannot be longer than 64 KiB
	if (!ntb32 && capacity > 0xffff)
		capacity = 0xffff;
	if (ndpOffset + ndpSize + divisor + kEthernetHeaderSize > capacity)
		return false;

	fNTB32 = ntb32;
	fCapacity = capacity;
	fDivisor = divisor;
	fPayloadRemainder = remainder;
	// The remainder is that of the payload after the Ethernet header: the
	// datagram itself starts 14 bytes earlier (as in Linux' cdc_ncm and
	// FreeBSD's cdce).
	fRemainder = (uint16_t)((remainder - kEthernetHeaderSize)
		& (size_t)(divisor - 1));
	fNDPAlignment = ndpAlignment;
	fMaxDatagrams = maxDatagrams;
	fNDPOffset = ndpOffset;
	fFirstDatagram = ndpOffset + ndpSize;
	fEnd = fFirstDatagram;
	fCount = 0;
	return true;
}


void
NTBWriter::Start(uint8_t* buffer)
{
	fBuffer = buffer;
	fCount = 0;
	fEnd = fFirstDatagram;
	fPreviousEnd = fEnd;
	// the header, the NDP and its unused entries, which end the table
	memset(buffer, 0, fFirstDatagram);
}


size_t
NTBWriter::_AlignedOffset(size_t offset) const
{
	// the next offset with offset % divisor == the datagram remainder
	return offset + ((fRemainder - offset) & (size_t)(fDivisor - 1));
}


bool
NTBWriter::CanAdd(size_t length) const
{
	if (fCount >= fMaxDatagrams)
		return false;
	size_t offset = _AlignedOffset(fEnd);
	return offset <= fCapacity && length <= fCapacity - offset;
}


uint8_t*
NTBWriter::Add(size_t length)
{
	if (!CanAdd(length))
		return NULL;

	size_t offset = _AlignedOffset(fEnd);
	memset(fBuffer + fEnd, 0, offset - fEnd);

	uint8_t* entry = fBuffer + fNDPOffset;
	if (fNTB32) {
		entry += kNDP32HeaderSize + fCount * kNDP32EntrySize;
		write32(entry, (uint32_t)offset);
		write32(entry + 4, (uint32_t)length);
	} else {
		entry += kNDP16HeaderSize + fCount * kNDP16EntrySize;
		write16(entry, (uint16_t)offset);
		write16(entry + 2, (uint16_t)length);
	}

	fCount++;
	fPreviousEnd = fEnd;
	fEnd = offset + length;
	return fBuffer + offset;
}


void
NTBWriter::RemoveLast()
{
	if (fCount == 0)
		return;

	fCount--;
	fEnd = fPreviousEnd;
	size_t entrySize = fNTB32 ? kNDP32EntrySize : kNDP16EntrySize;
	memset(fBuffer + fNDPOffset
			+ (fNTB32 ? kNDP32HeaderSize : kNDP16HeaderSize)
			+ fCount * entrySize,
		0, entrySize);
}


size_t
NTBWriter::Finish(uint16_t sequence, uint16_t maxPacketSize,
	uint32_t deviceMaxSize)
{
	size_t length = fEnd;
	if (maxPacketSize != 0 && (length % maxPacketSize) == 0
		&& length < deviceMaxSize) {
		// the buffer has the byte to spare beyond the capacity
		fBuffer[length++] = 0;
	}

	uint8_t* ndp = fBuffer + fNDPOffset;
	if (fNTB32) {
		write32(fBuffer, kNTH32Signature);
		write16(fBuffer + 4, kNTH32Size);
		write16(fBuffer + 6, sequence);
		write32(fBuffer + 8, (uint32_t)length);
		write32(fBuffer + 12, (uint32_t)fNDPOffset);

		write32(ndp, kNDP32Signature);
		write16(ndp + 4,
			(uint16_t)(kNDP32HeaderSize + (fCount + 1) * kNDP32EntrySize));
		// wReserved6, dwNextNdpIndex and dwReserved12 stay 0
	} else {
		write32(fBuffer, kNTH16Signature);
		write16(fBuffer + 4, kNTH16Size);
		write16(fBuffer + 6, sequence);
		write16(fBuffer + 8, (uint16_t)length);
		write16(fBuffer + 10, (uint16_t)fNDPOffset);

		write32(ndp, kNDP16Signature);
		write16(ndp + 4,
			(uint16_t)(kNDP16HeaderSize + (fCount + 1) * kNDP16EntrySize));
		// wNextNdpIndex stays 0
	}

	return length;
}


}	// namespace ncm
