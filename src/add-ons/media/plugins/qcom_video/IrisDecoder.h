/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_DECODER_H
#define IRIS_DECODER_H


#include <deque>
#include <vector>

#include <GraphicsDefs.h>
#include <OS.h>
#include <String.h>

#include "IrisSession.h"


/*!	H.264, HEVC, VP9 and MPEG-2 on the SC8280XP's video codec (Iris, HFI
	generation 1), through the qcom_iris driver.

	The firmware parses the stream itself: access units (Annex B, with start
	codes, all slices of one picture; a VP9 frame or superframe as a
	container holds it; an MPEG-2 picture with its headers) go in, NV12 pictures come out in
	display order. It decodes into reference pictures of its own (UBWC) and
	writes a linear NV12 copy of each picture to be shown into buffers of
	ours ("split mode", the only one Linux uses on this hardware).

	Interlaced streams (H.264 and MPEG-2 fields) come out as frames of two
	woven fields, decoded in the single stage mode on one pipe, as Qualcomm's
	msm_vidc has it, into copies with rows of 256 bytes; up to 1920 wide.

	Ten bit streams (HEVC Main 10) are decoded into TP10 UBWC references
	and copied out as P010, as Linux' venus driver has it: luma, then Cb
	and Cr in pairs, each sample sixteen bits with the ten in the top.

	Not thread safe; one thread feeds it and takes its pictures.
*/
class IrisDecoder : public IrisSession {
public:
	enum codec {
		H264,
		HEVC,
		VP9,
		MPEG2
	};

	struct Picture {
		int32		buffer;		// for ReleasePicture()
		int64		pts;
		uint32		width;		// what is to be shown
		uint32		height;
		uint32		cropLeft;
		uint32		cropTop;
		uint32		stride;		// of both planes, in bytes
		uint32		lumaRows;	// the chroma plane follows them
		const uint8* data;		// luma, then Cb and Cr in pairs
		bool		tenBit;		// samples of sixteen bits (P010)
		bool		interlaced;	// two fields, woven
		bool		corrupt;
	};

								IrisDecoder(codec codec);
								~IrisDecoder();

			/*!	\a width and \a height are what the stream is expected to
				be, when known (the container's); the firmware's sequence
				event says what it is. The session starts small anyway (see
				there). */
			status_t			Open(uint32 width = 0, uint32 height = 0,
									BString* _error = NULL);
			void				Close();

			/*!	One access unit, Annex B. B_WOULD_BLOCK when no input buffer
				came free within \a timeout, or pictures are ready while
				none is: the caller takes those, then tries again. */
			status_t			PutAccessUnit(const uint8* data, size_t size,
									int64 pts, bigtime_t timeout = 1000000);
			/*!	The end of the stream: the pictures the firmware still
				holds come out, then NextPicture() says
				B_LAST_BUFFER_ERROR. */
			status_t			EndOfStream();
			/*!	Forgets everything in flight; decoding starts again with
				the next access unit (which should be a random access
				point). Pictures the caller holds stay valid. */
			status_t			Flush();

			/*!	The next picture in display order, waiting up to
				\a timeout for one. */
			status_t			NextPicture(Picture& picture,
									bigtime_t timeout = 0);
			void				ReleasePicture(const Picture& picture);

			/*!	The picture's visible part as planes: luma, then either Cb
				and Cr planes of half the stride (\a interleaved false, I420)
				or one plane of pairs (NV12). \a stride is in bytes; the
				chroma follows the last luma row. Ten bit pictures lose
				their two lowest bits. */
			void				CopyPlanes(const Picture& picture,
									uint8* target, uint32 stride,
									bool interleaved) const;
			/*!	As CopyPlanes() (I420), half as wide and high, each sample
				the mean of a 2x2 block. The picture's width and height are
				multiples of four. */
			void				CopyHalfPlanes(const Picture& picture,
									uint8* target, uint32 stride) const;
			/*!	A ten bit picture's visible part as P010: luma, then one plane
				of Cb and Cr pairs after the last luma row, \a stride bytes a
				row. \a half: half as wide and high, each sample the mean of
				a 2x2 block (width and height multiples of four). */
			void				CopyP010(const Picture& picture,
									uint8* target, uint32 stride,
									bool half) const;
			/*!	The picture as B_YCbCr422 (Y0 Cb Y1 Cr) or B_RGB32 (BT.601
				below 720 rows, else BT.709; studio range). */
			void				CopyPacked(const Picture& picture,
									uint8* target, uint32 stride,
									color_space space) const;
			/*!	Failed because the stream's pictures changed (a broadcast going
				from frames to fields): a new session takes it on from the
				last key frame. */
			bool				NeedsNewSession() const
									{ return fNewSession; }
			bool				EndOfStreamSent() const
									{ return fEndOfStream; }

			uint32				PicturesDecoded() const { return fDecoded; }
			uint32				Width() const { return fWidth; }
			uint32				Height() const { return fHeight; }
			//!	Whether the stream has fields (known from its first
			//	picture on).
			bool				Interlaced() const { return fInterlaced; }

private:
			struct Output {
				IrisDevice::Buffer	buffer;
				bool		queued;		// with the firmware
				bool		ready;		// in fReady
				bool		held;		// with the caller
			};

	virtual	status_t			_Handle(const uint32* packet, size_t size);
			status_t			_HandleEvent(const uint32* packet,
									size_t size);
			status_t			_Service();

			status_t			_ConfigurePorts(bool input);
			status_t			_StartOutput();
			status_t			_QueueOutput(int32 index);
			void				_ReturnDropped();
			uint32				_OutputCount() const;
			uint32				_StrideAlignment() const;
			uint32				_ComvBuffers() const;
			status_t			_QueueReference(int32 index);
			void				_FreeOutputs();

			size_t				_BinSize(uint32 width, uint32 height) const;
			size_t				_Scratch1Size(uint32 width,
									uint32 height) const;
			size_t				_PersistSize() const;

			codec				fCodec;

			// the stream's geometry: as given, then as the firmware said
			uint32				fWidth;
			uint32				fHeight;
			uint32				fCropLeft;
			uint32				fCropTop;
			uint32				fCropWidth;
			uint32				fCropHeight;
			uint32				fMinCount;	// the firmware's minimum of
											// reference and output buffers
			bool				fTenBit;
			uint32				fHintWidth;		// the container's
			uint32				fHintHeight;
			bool				fInterlaced;
			bool				fNewSession;

			IrisDevice::Buffer	fPersist;
			IrisDevice::Buffer	fBin;
			IrisDevice::Buffer	fScratch1;

			std::vector<IrisDevice::Buffer> fInputs;
			std::vector<bool>	fInputQueued;
			std::vector<IrisDevice::Buffer> fReferences;
			std::vector<bool>	fReferenceQueued;
			std::vector<Output>	fOutputs;
			uint32				fOutputStride;
			uint32				fOutputRows;
			size_t				fOutputSize;
			size_t				fReferenceSize;

			// messages that came while waiting for another one
			bool				fSequenceChanged;
			bool				fOutputStarted;
			uint32				fEventWidth;
			uint32				fEventHeight;
			uint32				fEventCrop[4];
			uint32				fEventMinCount;
			bool				fEventTenBit;
			bool				fEventInterlaced;
			uint32				fLastAnswer;
			uint32				fLastAnswerError;
			bool				fEndOfStream;	// sent
			bool				fEnded;			// came back

			std::deque<int32>	fReady;
			std::deque<int64>	fReadyPts;
			std::deque<bool>	fReadyCorrupt;
			// dropped pictures' buffers, back to the firmware with the next
			// picture taken
			std::vector<int32>	fDropped;
			uint32				fDecoded;
};


#endif	// IRIS_DECODER_H
