/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_ENGINE_H
#define CEDAR_ENGINE_H


#include "VeDevice.h"
#include "h264_parse.h"
#include "hevc_parse.h"


/*!	A picture in the engine's memory and what the decoder keeps of it. */
struct CedarFrame {
	VeBuffer	picture;	// NV12: luma, then Cb and Cr in pairs
	VeBuffer	mvcol;		// the engine's motion data of the picture
	int32		position;	// H.264 frame list slot (1-17) while it is
							// decoded or a reference
	int32		poc;
	int64		pts;
	bool		current;	// being decoded
	bool		waiting;	// decoded, not yet in output order
	bool		ready;		// in output order, not yet taken
	bool		held;		// with the caller
	bool		corrupt;
	uint32		latency;

	// the picture's geometry, for copying it out
	uint32		stride;
	size_t		lumaSize;
	uint32		cropLeft;
	uint32		cropTop;
	uint32		width;		// what is to be shown
	uint32		height;
	uint32		bitDepth;
	// ten bits: the low two bits of every sample, behind the eight bit
	// picture; four samples to a byte, the lowest first, a row of luma
	// samples twoBitStride bytes long, then the chroma rows (Cb and Cr in
	// pairs) the same way
	size_t		twoBitOffset;
	size_t		twoBitChroma;	// where the chroma rows begin
	uint32		twoBitStride;

	CedarFrame() : position(-1), poc(0), pts(0), current(false),
		waiting(false), ready(false), held(false), corrupt(false),
		latency(0), stride(0), lumaSize(0), cropLeft(0), cropTop(0),
		width(0), height(0), bitDepth(8), twoBitOffset(0), twoBitChroma(0),
		twoBitStride(0) {}
};


/*!	Pictures and side buffers of one stream, and a slice as the engine's
	register writes (cedar-register-map.md sections 2.4 and 3.4, with what
	the T527 and H616 ports of Cedrus and the A733's own tests added:
	format registers after VE_MODE, NV12 as the secondary format too, line
	buffers in DRAM, 256-bit DDR mode, H.264 frame list slot 0 unused).
*/
class CedarEngine {
public:
								CedarEngine(VeDevice& device);
								~CedarEngine();

			//!	The stream's geometry; frees the side buffers and sets up new
			//	ones when it changed (the pictures are the caller's).
			status_t			ConfigureH264(const H264Sps& sps);
			status_t			ConfigureHevc(const HevcSps& sps);
			void				Unconfigure();

			status_t			AllocateFrame(CedarFrame& frame);
			void				FreeFrame(CedarFrame& frame);

			status_t			DecodeH264Slice(const H264State& state,
									const H264Slice& slice,
									CedarFrame& current,
									CedarFrame* const* frames,
									bool firstSliceInPicture,
									const int list0[32], const int list1[32],
									const uint8* nal, size_t size);
			status_t			DecodeHevcSlice(const HevcState& state,
									const HevcSlice& slice,
									CedarFrame& current,
									CedarFrame* const* frames,
									const int list0[16], const int list1[16],
									const uint8* nal, size_t size);

			uint32				CodedWidth() const { return fWidth; }
			uint32				CodedHeight() const { return fHeight; }
			uint32				Stride() const { return fStride; }
			size_t				LumaSize() const { return fLumaSize; }
			size_t				FrameSize() const { return fFrameSize; }
			size_t				MvcolSize() const { return fMvcolSize; }
			uint32				BitDepth() const { return fBitDepth; }
			size_t				TwoBitOffset() const { return fTwoBitOffset; }
			uint32				TwoBitStride() const { return fTwoBitStride; }
			//!	The chroma rows of the two-bit planes follow as many luma rows
			//	as the picture is high (not the 16 aligned height: 1080p and
			//	1000x562 Main 10 streams say so).
			size_t				TwoBitChroma() const
									{ return fTwoBitOffset
										+ (size_t)fTwoBitStride * fHeight; }
			bool				Matches(uint32 width, uint32 height,
									size_t mvcolSize) const;

			const char*			LastError() const { return fError; }

private:
			status_t			_Configure(uint32 width, uint32 height,
									size_t mvcolSize, int codec,
									uint32 bitDepth);
			status_t			_LoadBitstream(const uint8* nal, size_t size);
			void				_SetOutputFormat();
			uint32				_Mode(uint32 engine) const;
			void				_H264Sram(uint32 wordOffset,
									const uint32* data, int words);
			void				_H264SramBytes(uint32 wordOffset,
									const uint8* data, size_t size);
			void				_HevcFrameInfo(int slot,
									const CedarFrame& frame, int32 poc);
			void				_HevcRefList(const HevcState& state,
									const int* list, int count,
									uint32 sramOffset);
			void				_HevcWeights(const HevcSlice& slice,
									int list);
			void				_HevcScaling(const HevcScaling& scaling);
			void				_HevcTiles(const HevcState& state,
									const HevcSlice& slice, int ctbX,
									int ctbY);
			status_t			_Run(uint32 trigger, uint32 status,
									const char* what);

			VeDevice&			fDevice;
			int					fCodec;		// 1 H.264, 4 HEVC, 0 none
			uint32				fWidth;		// coded, luma samples
			uint32				fHeight;
			uint32				fStride;	// 16-aligned
			uint32				fAlignedHeight;
			size_t				fLumaSize;
			size_t				fChromaSize;
			size_t				fFrameSize;
			size_t				fMvcolSize;
			uint32				fBitDepth;
			// ten bits: the low two bits of every sample after the eight
			// bit picture (which holds the eight most significant ones)
			uint32				fTwoBitStride;
			size_t				fTwoBitOffset;

			VeBuffer			fBitstream;
			// H.264
			VeBuffer			fPictureInfo;
			VeBuffer			fNeighbour;
			VeBuffer			fDeblocking;
			VeBuffer			fIntraPrediction;
			// HEVC
			VeBuffer			fHevcNeighbour;
			VeBuffer			fEntryPoints;

			bool				fDdr128;
			bool				fEntryPointsPlus1;
			char				fError[96];
};


#endif	// CEDAR_ENGINE_H
