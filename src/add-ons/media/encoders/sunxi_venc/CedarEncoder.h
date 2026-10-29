/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_ENCODER_H
#define CEDAR_ENCODER_H


#include <SupportDefs.h>


/*!	H.264 and H.265 on the Allwinner A733's video encoder (the Cedar VE's
	second engine), through /dev/misc/sunxi_ve (SUNXI_VE_ENCODE).

	The engine is programmed as Allwinner's own encoder library programs it
	(docs/airos/SUNXI_VENC.md): H.264 High with CABAC and 8x8 transforms,
	H.265 Main with the engine's fixed coding tools; I and P pictures, one
	reference picture, one slice a picture, a QP per picture (constant, or
	from a simple per-picture rate control when a bit rate is asked). The
	parameter sets and slice headers are written here; the engine writes
	the slice data.

	One picture at a time, synchronously: Encode() returns its NAL units. */
class CedarEncoder {
public:
	enum Codec {
		kH264,
		kHEVC
	};

	struct Params {
		Codec		codec;
		uint32		width;
		uint32		height;
		uint32		qp;				// constant QP, or the first one
		uint32		bitrate;		// bit/s; 0: constant QP
		uint32		frameRateNum;
		uint32		frameRateDen;
		uint32		keyInterval;	// pictures, at most 65535
		uint32		sarNum;			// 0: unknown
		uint32		sarDen;
		uint8		colourPrimaries;	// H.273; 2: unspecified
		uint8		transfer;
		uint8		matrix;
		bool		fullRange;

					Params();
	};

								CedarEncoder();
								~CedarEncoder();

	// Whether the driver is there and has an encoder.
	static	bool				IsAvailable(char* reason = NULL,
									size_t reasonSize = 0);

			status_t			Open(const Params& params);
			void				Close();

	// The parameter sets (VPS, SPS, PPS) in Annex B.
			const uint8*		Headers(size_t* size) const;

	// One NV12 picture in, its access unit (Annex B, with the parameter
	// sets before a key picture) out; valid until the next Encode().
			status_t			Encode(const uint8* luma, uint32 lumaStride,
									const uint8* chroma, uint32 chromaStride,
									bool forceKey, const uint8** data,
									size_t* size, bool* key);

			const char*			Error() const { return fError; }
	// "H.265, constant QP 26, IDR every 60"
			const char*			Description() const { return fDescription; }
	// microseconds the engine took for the last picture
			uint32				EngineTime() const { return fEngineTime; }
			uint32				PicturesEncoded() const { return fPictures; }

private:
			struct Buffer;
			struct Ops;

			status_t			_Allocate(Buffer& buffer, size_t size);
			void				_Free(Buffer& buffer);
			void				_Sync(Buffer& buffer, uint32 direction,
									size_t size = 0);
			void				_WriteHeaders();
			size_t				_SliceHeader(uint8* out, bool idr, uint32 qp);
			void				_Program(Ops& ops, bool idr, uint32 qp);
			uint32				_NextQp(uint32 bits);
			void				_SetError(const char* format, ...);

			bool				_IsHevc() const
									{ return fParams.codec == kHEVC; }

			int					fDevice;
			Params				fParams;
			uint32				fWidth16;		// the input, in whole
			uint32				fHeight16;		// macroblocks
			uint32				fStride;

			Buffer*				fInput[2];		// luma, chroma
			Buffer*				fBitstream;
			Buffer*				fLuma[2];		// reference pictures
			Buffer*				fChroma[2];		// (H.264: = fLuma)
			uint32				fChromaOffset;
			Buffer*				fSubsampled[2];
			Buffer*				fMotion[2];		// H.264: one, mb info
			Buffer*				fBlockInfo;		// H.265
			Buffer*				fColocated;		// H.265
			Buffer*				fStatistics;	// H.264
			Buffer*				fRowTable;
			Buffer*				fLines[2];

			uint8*				fHeaders;
			size_t				fHeadersSize;
			uint8*				fOutput;
			size_t				fOutputCapacity;

			uint32				fPictures;
			uint32				fSinceKey;		// pictures since the IDR
			uint32				fIdrId;
			uint32				fReconstructed;	// fLuma[] index
			uint32				fQp;
			double				fRateBalance;	// bits over (+) or under
			uint32				fEngineTime;
			char				fError[160];
			char				fDescription[96];
};


#endif	// CEDAR_ENCODER_H
