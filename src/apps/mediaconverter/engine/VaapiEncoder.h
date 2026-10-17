/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VAAPI_ENCODER_H
#define VAAPI_ENCODER_H


#include <deque>

#include "VaapiLibrary.h"
#include "VideoEncoder.h"


class BitWriter;


/*!	H.264, H.265 and AV1 on a GPU's video encoder through VA-API: on AMD
	cards that is Mesa's radeonsi driver on amdgpu_drm driving VCE, UVD's
	encoder or VCN.

	The structure follows FFmpeg's VA-API encoders, which Mesa is tested
	against: the parameter sets and slice headers are written here and
	handed over as packed headers (Mesa parses them and, for VCE, needs a
	packed slice header before it puts the parameter sets in the stream),
	pictures are I and P (P referring to the one before it), and up to
	kPipelineDepth pictures are in the encoder at once so the GPU never
	waits for the CPU to read a result. */
class VaapiEncoder : public VideoEncoder {
public:
								VaapiEncoder(const char* renderNode,
									const char* deviceName);
	virtual						~VaapiEncoder();

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error);
	virtual	AVPixelFormat		InputFormat() const;
	virtual	status_t			SendFrame(const AVFrame* frame);
	virtual	status_t			ReceivePacket(AVPacket* packet);
	virtual	encoder_backend		Backend() const;
	virtual	BString				Name() const;
	virtual	BString				Description() const;

private:
	struct Picture {
		int64			pts;
		bool			idr;
		int64			index;			// pictures since the last IDR
		VASurfaceID		input;
		VASurfaceID		recon;
		VASurfaceID		releaseRecon;	// no longer needed once this is done
		VABufferID		coded;
		std::vector<VABufferID> buffers;
		uint32			frameNum;		// H.264
		int32			order;			// picture order count
		int				slot;			// AV1 reference slot
	};

	enum { kPipelineDepth = 3 };

			status_t			_ChooseProfile(BString& error);
			status_t			_CreateSession(BString& error);
			void				_Align();
			status_t			_PrimeExtradata();
			status_t			_Upload(const AVFrame* frame,
									VASurfaceID surface);
			status_t			_Issue(Picture& picture);
			status_t			_Retrieve();
			status_t			_AddBuffer(Picture& picture, VABufferType type,
									const void* data, size_t size);
			status_t			_AddMisc(Picture& picture,
									VAEncMiscParameterType type,
									const void* data, size_t size);
			status_t			_AddPacked(Picture& picture,
									VAEncPackedHeaderType type,
									const std::vector<uint8>& data,
									size_t bits, bool emulationBytes = true);
			void				_ReleaseBuffers(Picture& picture);

			// H.264
			void				_WriteH264Headers(std::vector<uint8>& out);
			void				_WriteH264Vui(BitWriter& bits);
			status_t			_BuildH264(Picture& picture);
			// H.265
			void				_WriteHevcHeaders(std::vector<uint8>& out);
			void				_WriteHevcProfileTierLevel(BitWriter& bits);
			status_t			_BuildHevc(Picture& picture);
			// AV1
			void				_WriteAv1SequenceHeader(
									std::vector<uint8>& out);
			status_t			_BuildAv1(Picture& picture);

			status_t			_AddRateControl(Picture& picture);
			int					_Level() const;

			BString				fRenderNode;
			BString				fDeviceName;
			VaapiDisplay		fDisplay;
			const VaapiLibrary*	fVA;
			VideoEncoderParams	fParams;
			AVCodecID			fCodec;
			int					fBitDepth;
			VAProfile			fProfile;
			VAEntrypoint		fEntrypoint;
			uint32				fRTFormat;
			uint32				fRateControl;
			uint32				fPackedHeaders;
			uint32				fQualityRange;
			bool				fMesa;
			VAConfigID			fConfig;
			VAContextID			fContext;
			int					fAlignedWidth;
			int					fAlignedHeight;
			int					fBlockSize;		// macroblock or CTU
			int					fMinBlockSize;	// H.265 minimum CB
			uint32				fHevcFeatures;
			uint32				fHevcBlockSizes;
			int					fQP;			// P pictures
			int					fQPIntra;

			std::vector<VASurfaceID> fFreeInputs;
			std::vector<VASurfaceID> fFreeRecons;
			std::vector<VABufferID> fFreeCoded;
			std::vector<VASurfaceID> fAllSurfaces;
			std::vector<VABufferID> fAllCoded;
			bool				fDeriveWorks;
			VAImageFormat		fImageFormat;

			std::deque<Picture>	fInFlight;
			std::deque<AVPacket*> fOutput;
			bool				fHaveReference;
			VASurfaceID			fReference;
			uint32				fReferenceFrameNum;
			int32				fReferenceOrder;
			int					fReferenceSlot;
			int64				fCount;			// pictures sent
			int64				fIndexInGop;
			uint32				fIdrId;
			bool				fDrained;
			BString				fDescription;
};


#endif	// VAAPI_ENCODER_H
