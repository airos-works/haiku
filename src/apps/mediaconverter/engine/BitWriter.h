/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BIT_WRITER_H
#define BIT_WRITER_H


#include <SupportDefs.h>

#include <vector>


/*!	Writes the bit syntax of H.264, H.265 and AV1 headers: fixed-width
	fields, Exp-Golomb codes, and AV1's leb128 and uvlc. */
class BitWriter {
public:
								BitWriter();

			void				Bits(uint32 value, int count);
			void				Bit(bool value) { Bits(value ? 1 : 0, 1); }
			void				UnsignedExpGolomb(uint32 value);
			void				SignedExpGolomb(int32 value);
			void				TrailingBits();		// rbsp_trailing_bits()
			void				ByteAlign(bool one = false);
			void				Bytes(const uint8* data, size_t size);

			size_t				BitCount() const { return fBitCount; }
			bool				IsAligned() const { return (fBitCount & 7) == 0; }
	// The data, with a last partial byte padded with zero bits.
	const	std::vector<uint8>&	Data() const { return fData; }

private:
			std::vector<uint8>	fData;
			size_t				fBitCount;
};


// Appends a NAL unit in Annex B form - start code, header, and the RBSP with
// emulation prevention bytes - to `out`. Returns the number of bits of it
// that are payload: for an RBSP that does not end on a byte boundary (a
// packed slice header), the rest of the last byte is not.
size_t	append_annexb_nal(std::vector<uint8>& out, const uint8* header,
			size_t headerSize, const BitWriter& rbsp);


#endif	// BIT_WRITER_H
