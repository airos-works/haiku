/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_ENCODER_H
#define IRIS_ENCODER_H


#include <deque>
#include <vector>

#include "IrisSession.h"


/*!	H.264 and HEVC Main encoding on the SC8280XP's video codec (Iris, HFI
	generation 1), through the qcom_iris driver: NV12 pictures in, Annex B
	access units out, in the order the pictures went in (no B pictures).
	Every intra picture is an IDR picture with the parameter sets in front.

	Not thread safe; one thread feeds it and takes its output.
*/
class IrisEncoder : public IrisSession {
public:
	enum codec {
		H264,
		HEVC
	};

	struct Params {
		IrisEncoder::codec codec;
		uint32		width;
		uint32		height;
		uint32		framesPerSecond;
		uint32		bitrate;		// bits per second
		uint32		keyInterval;	// pictures from one IDR to the next
		bool		constantBitrate;	// else variable

					Params()
						: codec(H264), width(0), height(0),
						framesPerSecond(30), bitrate(8000000), keyInterval(60),
						constantBitrate(false) {}
	};

	struct Packet {
		int32		buffer;			// for ReleasePacket()
		const uint8* data;
		size_t		size;
		int64		pts;
		bool		key;
	};

								IrisEncoder();
								~IrisEncoder();

			status_t			Open(const Params& params,
									BString* _error = NULL);
			void				Close();

			/*!	One picture: \a width x \a height luma samples \a lumaStride
				apart, then rows of Cb and Cr pairs (NV12) \a chromaStride
				apart. B_WOULD_BLOCK when no input buffer came free within
				\a timeout, or output is ready while none is: the caller
				takes that, then tries again. */
			status_t			PutPicture(const uint8* luma,
									uint32 lumaStride, const uint8* chroma,
									uint32 chromaStride, int64 pts,
									bigtime_t timeout = 1000000);
			//!	As PutPicture(), from I420 (Cb and Cr planes of their own).
			status_t			PutPicture420(const uint8* luma,
									uint32 lumaStride, const uint8* cb,
									const uint8* cr, uint32 chromaStride,
									int64 pts, bigtime_t timeout = 1000000);
			/*!	The end of the stream: what the codec still holds comes out,
				then NextPacket() says B_LAST_BUFFER_ERROR. */
			status_t			EndOfStream();

			status_t			NextPacket(Packet& packet,
									bigtime_t timeout = 0);
			void				ReleasePacket(const Packet& packet);

			//!	The parameter sets with start codes, once the first IDR
			//	picture came out.
			const std::vector<uint8>& ParameterSets() const
									{ return fParameterSets; }
			uint32				PicturesEncoded() const { return fEncoded; }

private:
			struct Output {
				IrisDevice::Buffer	buffer;
				bool		queued;
				bool		held;
				uint32		offset;
				uint32		size;
				int64		pts;
				bool		key;
			};

	virtual	status_t			_Handle(const uint32* packet, size_t size);
			status_t			_Configure();
			status_t			_Queue(int32 index);
			status_t			_InputBuffer(int32& index,
									bigtime_t timeout);
			status_t			_SendInput(int32 index, int64 pts);
			void				_KeepParameterSets(const uint8* data,
									size_t size);

			Params				fParams;
			uint32				fStride;		// of the input's planes
			uint32				fLumaRows;
			size_t				fInputSize;

			IrisDevice::Buffer	fPersist;
			IrisDevice::Buffer	fScratch;
			IrisDevice::Buffer	fScratch1;
			IrisDevice::Buffer	fScratch2;

			std::vector<IrisDevice::Buffer> fInputs;
			std::vector<bool>	fInputQueued;
			std::vector<Output>	fOutputs;
			std::deque<int32>	fReady;

			std::vector<uint8>	fParameterSets;
			bool				fEndOfStream;
			bool				fEnded;
			uint32				fEncoded;
};


#endif	// IRIS_ENCODER_H
