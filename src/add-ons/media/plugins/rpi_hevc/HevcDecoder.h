/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HEVC_DECODER_H
#define HEVC_DECODER_H


#include <deque>
#include <vector>

#include <OS.h>
#include <String.h>

#include <rpi_hevc.h>

#include "HevcParser.h"


/*!	HEVC (Main and Main 10) on the decoder block of the Raspberry Pi 4,
	through /dev/misc/rpi_hevc.

	The block does the slice data and makes the pictures; what a decoder does
	around that is here: the parameter sets and slice headers, the picture
	order, which pictures are references and when one goes out, and the
	list of register writes that takes the block through a picture's slices.

	NAL units go in one at a time, pictures come out in output order as the
	block made them: columns 128 bytes wide, each with all its luma rows and
	then its chroma rows (Cb and Cr in pairs), ten-bit samples three to a 32
	bit word. CopyPlanes() turns that into planes.
*/
class HevcDecoder {
public:
	struct Picture {
		int32		frame;		// for ReleasePicture()
		int64		pts;
		int32		poc;
		uint32		width;		// what is to be shown
		uint32		height;
		uint32		bitDepth;
		bool		corrupt;
	};

								HevcDecoder();
								~HevcDecoder();

			status_t			Open(BString* _error = NULL);
			void				Close();

			/*!	One NAL unit, without start code or length. \a pts is kept
				with the picture the unit belongs to. A picture is decoded
				when the first unit of the next one arrives, or with
				EndAccessUnit(). */
			status_t			PutNal(const uint8* nal, size_t size,
									int64 pts);
			status_t			EndAccessUnit();
			/*!	The end of the stream: every picture goes out. */
			void				Drain();
			/*!	Forgets all pictures; decoding starts again with the next
				random access point. */
			void				Reset();

			/*!	Pictures with a time before this one are not wanted: those
				that nothing refers to are not decoded. */
			void				SetSkipBefore(int64 pts) { fSkipBefore = pts; }

			bool				NextPicture(Picture& picture);
			void				ReleasePicture(const Picture& picture);

			/*!	The picture as planes: eight bit as luma and then either
				Cb and Cr planes of half the stride (\a interleaved false)
				or one of pairs; ten bit as sixteen bit samples (the value
				times 64), chroma always in pairs. \a stride is in bytes;
				the chroma follows the last luma row. With \a half, half
				as wide and high (each sample the mean of a 2x2 block; not
				for interleaved eight bit planes, and the picture's size
				must be multiples of four). */
			void				CopyPlanes(const Picture& picture,
									uint8* target, uint32 stride,
									bool interleaved, bool half = false);

			const char*			Error() const { return fError.String(); }
			uint32				PicturesDecoded() const { return fDecoded; }

private:
	struct Buffer {
		uint32		id;
		area_id		clone;
		uint8*		address;
		size_t		size;
		uint64		physical;

		Buffer() : id(RPI_HEVC_NO_BUFFER), clone(-1), address(NULL), size(0),
			physical(0) {}
	};

	struct Geometry {
		uint32		width;			// coded
		uint32		height;
		uint32		bitDepth;
		uint32		columns;
		uint32		columnStride;	// bytes from one column to the next
		uint32		chromaOffset;
		uint32		frameSize;
		uint32		mvStride;
		uint32		mvSize;
		uint32		cropLeft, cropTop;
		uint32		shownWidth, shownHeight;

		bool operator==(const Geometry& other) const;
	};

	struct Frame {
		Buffer		buffer;
		Buffer		mv;
		Geometry	geometry;
		int32		poc;
		int64		pts;
		bool		shortTerm;
		bool		longTerm;
		bool		neededForOutput;
		bool		queued;			// in the output queue or with the caller
		bool		current;
		bool		corrupt;
		uint32		latency;
	};

	struct Slice {
		hevc::SliceHeader	header;
		uint32				dataOffset;	// in the bitstream buffer
		uint32				dataSize;
	};

			class CommandBuilder;
			friend class CommandBuilder;

			status_t			_Allocate(Buffer& buffer, size_t size,
									bool map);
			void				_Free(Buffer& buffer);
			status_t			_Fail(const char* format, ...);

			status_t			_AddSlice(const uint8* nal, size_t size,
									int64 pts);
			status_t			_DecodePicture();
			status_t			_StartPicture(const hevc::SliceHeader& header);
			status_t			_Configure(const hevc::SPS& sps);
			void				_UpdateTiles();
			void				_ComputePoc(const hevc::SliceHeader& header);
			void				_ApplyReferenceSet(
									const hevc::SliceHeader& header);
			Frame*				_FindReference(int32 poc, bool longTerm,
									bool fullPoc);
			Frame*				_MissingReference(int32 poc);
			void				_BuildLists(const hevc::SliceHeader& header,
									uint8 lists[2][hevc::kMaxReferences]);
			Frame*				_NewFrame();
			bool				_FrameIsFree(const Frame* frame) const;
			void				_ReleaseUnused();
			bool				_Bump();
			void				_BumpAll();
			void				_BumpBeforeDecoding();
			void				_BumpAfterDecoding();
			status_t			_RunHardware(bool& corrupt);

			int					fDevice;
			BString				fError;
			bool				fTrace;

			hevc::SPS			fSpsList[hevc::kMaxSpsCount];
			hevc::PPS			fPpsList[hevc::kMaxPpsCount];
			const hevc::SPS*	fSps;		// of the picture being decoded
			const hevc::PPS*	fPps;
			Geometry			fGeometry;
			bool				fConfigured;

			// tiles of the picture
			std::vector<uint32>	fColumnBoundary;
			std::vector<uint32>	fRowBoundary;
			std::vector<uint32>	fRsToTs;
			std::vector<uint32>	fTsToRs;

			// what the block works with besides the pictures
			Buffer				fCommands;
			Buffer				fBitstream;
			Buffer				fPu;
			Buffer				fCoeff;
			uint32				fCommandCount;

			// the picture being collected
			std::vector<Slice>	fSlices;
			uint32				fBitstreamUsed;
			int64				fPicturePts;
			int64				fSkipBefore;

			// decoding state
			bool				fAwaitRandomAccess;
			bool				fFirstPicture;
			bool				fNoRaslOutput;
			int32				fPrevTid0Poc;
			int32				fPoc;
			std::vector<Frame*>	fFrames;
			Frame*				fCurrent;
			Frame*				fDpb[hevc::kMaxReferences];
			int32				fDpbCount;
			Frame*				fStCurrBefore[hevc::kMaxReferences];
			Frame*				fStCurrAfter[hevc::kMaxReferences];
			Frame*				fLtCurr[hevc::kMaxReferences];
			int32				fNumStCurrBefore;
			int32				fNumStCurrAfter;
			int32				fNumLtCurr;
			std::deque<Frame*>	fOutput;
			uint32				fDecoded;
};

#endif	// HEVC_DECODER_H
