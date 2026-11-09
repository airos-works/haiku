/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef XDNA_SHA256_H
#define XDNA_SHA256_H


#include <SupportDefs.h>


/*!	SHA-256 (FIPS 180-4), for the keys design bundles are found by. */
class Sha256 {
public:
								Sha256();

			void				Update(const void* data, size_t size);
			void				Finish(uint8 digest[32]);

	// the pieces the keys are made of, little endian
			void				AddUInt8(uint8 value);
			void				AddUInt32(uint32 value);
			void				AddInt32(int32 value);
			void				AddUInt64(uint64 value);
			void				AddFloat(float value);
			void				AddString(const char* string);

	//! \a count bytes of \a digest as 2 * count lower case digits
	static	void				ToHex(const uint8* digest, size_t count,
									char* text);

private:
			void				_Block(const uint8* block);

			uint32				fState[8];
			uint64				fLength;
			uint8				fBuffer[64];
			size_t				fFill;
};


#endif	// XDNA_SHA256_H
