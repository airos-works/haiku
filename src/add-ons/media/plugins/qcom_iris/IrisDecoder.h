/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_DECODER_H
#define IRIS_DECODER_H


#include <deque>
#include <vector>

#include <OS.h>
#include <String.h>

#include "IrisDevice.h"


/*!	H.264 and HEVC on the SC8280XP's video codec (Iris, HFI generation 1),
	through the qcom_iris driver.

	The firmware parses the stream itself: access units (Annex B, with start
	codes, all slices of one picture) go in, NV12 pictures come out in
	display order. It decodes into reference pictures of its own (UBWC) and
	writes a linear NV12 copy of each picture to be shown into buffers of
	ours ("split mode", the only one Linux uses on this hardware).

	Not thread safe; one thread feeds it and takes its pictures.
*/
class IrisDecoder {
public:
	enum codec {
		H264,
		HEVC
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
		bool		corrupt;
	};

								IrisDecoder(codec codec);
								~IrisDecoder();

			/*!	\a width and \a height are what the stream is expected to
				be, when known (the container's); the firmware's sequence
				event says what it is. */
			status_t			Open(uint32 width = 0, uint32 height = 0,
									BString* _error = NULL);
			void				Close();

			/*!	One access unit, Annex B. B_WOULD_BLOCK when no input buffer
				came free within \a timeout: the caller takes the pictures
				that are ready, then tries again. */
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
				chroma follows the last luma row. */
			void				CopyPlanes(const Picture& picture,
									uint8* target, uint32 stride,
									bool interleaved) const;

			const char*			Error() const { return fError.String(); }
			const char*			FirmwareVersion() const
									{ return fDevice.FirmwareVersion(); }
			uint32				PicturesDecoded() const { return fDecoded; }
			uint32				Width() const { return fWidth; }
			uint32				Height() const { return fHeight; }

private:
			struct Output {
				IrisDevice::Buffer	buffer;
				bool		queued;		// with the firmware
				bool		ready;		// in fReady
				bool		held;		// with the caller
			};

			status_t			_Fail(status_t status, const char* format,
									...);

			status_t			_Send(const uint32* packet);
			status_t			_SetProperty(uint32 property,
									const uint32* payload, uint32 words);
			status_t			_SetProperty(uint32 property, uint32 a);
			status_t			_SetProperty(uint32 property, uint32 a,
									uint32 b);
			status_t			_SetProperty(uint32 property, uint32 a,
									uint32 b, uint32 c);
			status_t			_Simple(uint32 command, uint32 answer);
			status_t			_WaitFor(uint32 type, uint32* packet = NULL);
			status_t			_Pump(bigtime_t timeout);
			status_t			_Handle(const uint32* packet, size_t size);
			status_t			_HandleEvent(const uint32* packet,
									size_t size);
			status_t			_Service();

			status_t			_ConfigurePorts(bool input);
			status_t			_SetInternal(uint32 type,
									IrisDevice::Buffer& buffer, size_t size);
			status_t			_ReleaseInternal(uint32 type,
									IrisDevice::Buffer& buffer);
			status_t			_StartOutput();
			status_t			_QueueOutput(int32 index);
			status_t			_QueueReference(int32 index);
			void				_FreeOutputs();

			size_t				_BinSize(uint32 width, uint32 height) const;
			size_t				_Scratch1Size(uint32 width,
									uint32 height) const;
			size_t				_PersistSize() const;

			codec				fCodec;
			IrisDevice			fDevice;
			BString				fError;
			uint32				fSession;
			bool				fOpen;
			bool				fStarted;	// LOAD_RESOURCES and START done
			bool				fFailed;

			// the stream's geometry: as given, then as the firmware said
			uint32				fWidth;
			uint32				fHeight;
			uint32				fCropLeft;
			uint32				fCropTop;
			uint32				fCropWidth;
			uint32				fCropHeight;
			uint32				fMinCount;	// the firmware's minimum of
											// reference and output buffers

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
			uint32				fLastAnswer;
			uint32				fLastAnswerError;
			bool				fEndOfStream;	// sent
			bool				fEnded;			// came back

			std::deque<int32>	fReady;
			std::deque<int64>	fReadyPts;
			std::deque<bool>	fReadyCorrupt;
			uint32				fDecoded;
};


#endif	// IRIS_DECODER_H
