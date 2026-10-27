/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_DECODER_H
#define CEDAR_DECODER_H


#include <deque>
#include <vector>

#include <GraphicsDefs.h>
#include <OS.h>
#include <String.h>

#include "CedarEngine.h"
#include "VeDevice.h"
#include "h264_parse.h"
#include "hevc_parse.h"


/*!	H.264 and HEVC on the Allwinner A733's Cedar video engine, through
	/dev/misc/sunxi_ve.

	The engine decodes one slice at a time and makes NV12 pictures; what a
	decoder does around that is here: the parameter sets and slice headers,
	the picture order, which pictures are references and when one goes out.

	NAL units go in one at a time; a slice is decoded as it arrives, a
	picture is done when the first slice of the next one arrives, or with
	EndAccessUnit(). Pictures come out in output order.
*/
class CedarDecoder {
public:
	enum codec {
		H264,
		HEVC
	};

	struct Picture {
		int32		frame;		// for ReleasePicture()
		int64		pts;
		int32		poc;
		uint32		width;		// what is to be shown
		uint32		height;
		uint32		bitDepth;
		bool		corrupt;
	};

								CedarDecoder(codec codec);
								~CedarDecoder();

			status_t			Open(BString* _error = NULL);
			void				Close();

			/*!	One NAL unit, without start code or length. \a pts is kept
				with the picture the unit belongs to. Returns an error only
				for what the decoder cannot go on with (a stream it does not
				support, no memory); a slice that does not decode leaves its
				picture corrupt, which is then not shown. */
			status_t			PutNal(const uint8* nal, size_t size,
									int64 pts);
			status_t			EndAccessUnit();
			/*!	The end of the stream: every picture goes out. */
			void				Drain();
			/*!	Forgets all pictures; decoding starts again with the next
				random access point. */
			void				Reset();

			/*!	Pictures with a time before this one are not wanted: those
				nothing refers to are not decoded. */
			void				SetSkipBefore(int64 pts) { fSkipBefore = pts; }

			bool				NextPicture(Picture& picture);
			void				ReleasePicture(const Picture& picture);

			/*!	The picture as planes: luma, then either Cb and Cr planes of
				half the stride (\a interleaved false) or one of pairs
				(NV12); ten bit pictures always as P010 (sixteen bit
				samples, the value in the upper ten bits, chroma in pairs),
				put together from the engine's eight bit picture and its
				two-bit planes.
				\a stride is in bytes; the chroma follows the last luma
				row. */
			void				CopyPlanes(const Picture& picture,
									uint8* target, uint32 stride,
									bool interleaved);
			/*!	As CopyPlanes(), half as wide and high, each sample the mean
				of a 2x2 block: ten bit pictures as P010, eight bit ones as
				I420. The picture's width and height are multiples of
				four. */
			void				CopyHalfPlanes(const Picture& picture,
									uint8* target, uint32 stride);
			/*!	The picture as Haiku's B_YCbCr422 (Y0 Cb Y1 Cr) or B_RGB32
				(BT.601 below 720 rows, else BT.709; studio range), for the
				Media Kit's users; ten bit pictures by their eight most
				significant bits. */
			void				CopyPacked(const Picture& picture,
									uint8* target, uint32 stride,
									color_space space);
			/*!	A ten bit picture's low two bits as the engine left them
				(for finding out their layout); returns their size. */
			size_t				CopyTwoBit(const Picture& picture,
									uint8* target, size_t size);

			const char*			Error() const { return fError.String(); }
			uint32				PicturesDecoded() const { return fDecoded; }
			uint32				SlicesDecoded() const
									{ return fDevice.Slices(); }
			bigtime_t			EngineTime() const
									{ return fDevice.EngineTime(); }
			const sunxi_ve_info& Info() const { return fDevice.Info(); }

private:
			status_t			_Fail(status_t status, const char* format,
									...);

			status_t			_PutH264(const uint8* nal, size_t size,
									int64 pts);
			status_t			_H264Slice(const uint8* nal, size_t size,
									int64 pts);
			status_t			_StartH264Picture(const H264Slice& slice,
									int64 pts);
			void				_FinishH264Picture();
			int32				_FreeH264Position() const;

			status_t			_PutHevc(const uint8* nal, size_t size,
									int64 pts);
			status_t			_HevcSlice(const uint8* nal, size_t size,
									int64 pts);
			status_t			_StartHevcPicture(const HevcSlice& slice,
									int64 pts);
			void				_FinishHevcPicture();

			void				_FinishPicture();
			status_t			_NewFrame(uint32 width, uint32 height,
									uint32 cropLeft, uint32 cropTop,
									uint32 bitDepth, int64 pts);
			bool				_IsReference(int32 index) const;
			bool				_IsFree(int32 index) const;
			uint32				_CountWaiting() const;
			uint32				_CountHeld() const;
			bool				_Bump();
			void				_BumpAll();
			void				_DropWaiting();

			codec				fCodec;
			VeDevice			fDevice;
			CedarEngine			fEngine;
			BString				fError;
			char				fParseError[160];

			// stream state: the parsers' (large, so on the heap)
			H264State*			fH264;
			H264Slice*			fH264Slice;		// the slice at hand
			H264Slice*			fH264First;		// of the current picture
			H264Slice*			fH264Previous;	// the last one seen
			bool				fHavePrevious;
			HevcState*			fHevc;
			HevcSlice*			fHevcSlice;
			HevcSlice*			fHevcFirst;
			bool				fNoRaslOutput;	// of the last IRAP picture

			// pictures
			std::vector<CedarFrame*> fFrames;
			int32				fCurrent;
			bool				fFirstSlice;	// none of it decoded yet
			bool				fSkipping;		// its slices are dropped
			bool				fAwaitRandomAccess;
			int64				fSkipBefore;
			uint32				fReorder;		// H.264 output
			uint32				fDpbSize;
			std::deque<int32>	fOutput;
			uint32				fDecoded;
};


#endif	// CEDAR_DECODER_H
