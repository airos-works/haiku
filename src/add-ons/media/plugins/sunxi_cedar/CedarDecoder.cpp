/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CedarDecoder.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON)
	// the header's vector literals narrow, which Haiku's flags warn about
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wnarrowing"
#	include <arm_neon.h>
#	pragma GCC diagnostic pop
#endif

#include "CedarRegisters.h"


// CEDAR_NO_NEON: the plain C loops, to compare with
static const bool sNoNeon = getenv("CEDAR_NO_NEON") != NULL;

//#define TRACE_CEDAR
#ifdef TRACE_CEDAR
#	define TRACE(x...) fprintf(stderr, "sunxi_cedar: " x)
#else
#	define TRACE(x...) ;
#endif


// the references, the current picture, the ones waiting to be shown and
// a few with the caller
static const size_t kMaxFrames = 36;
static const uint32 kMaxSize = 4096;


CedarDecoder::CedarDecoder(codec codec)
	:
	fCodec(codec),
	fEngine(fDevice),
	fH264(NULL),
	fH264Slice(NULL),
	fH264First(NULL),
	fH264Previous(NULL),
	fHavePrevious(false),
	fHevc(NULL),
	fHevcSlice(NULL),
	fHevcFirst(NULL),
	fNoRaslOutput(false),
	fCurrent(-1),
	fFirstSlice(false),
	fSkipping(false),
	fAwaitRandomAccess(true),
	fSkipBefore(INT64_MIN),
	fReorder(0),
	fDpbSize(1),
	fDecoded(0)
{
	fParseError[0] = '\0';
	// never moved: a picture is copied out while the next one is decoded
	fFrames.reserve(kMaxFrames);
}


CedarDecoder::~CedarDecoder()
{
	Close();
}


status_t
CedarDecoder::_Fail(status_t status, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	fError.SetToFormatVarArgs(format, args);
	va_end(args);
	return status;
}


status_t
CedarDecoder::Open(BString* _error)
{
	if (fDevice.IsOpen())
		return B_OK;
	status_t status = fDevice.Open(_error);
	if (status != B_OK)
		return status;
	// a picture's slices go one after the other (see _FinishPicture())
	fDevice.SetInPicture(true);

	if (fCodec == H264) {
		fH264 = new(std::nothrow) H264State;
		fH264Slice = new(std::nothrow) H264Slice;
		fH264First = new(std::nothrow) H264Slice;
		fH264Previous = new(std::nothrow) H264Slice;
		if (fH264 == NULL || fH264Slice == NULL || fH264First == NULL
			|| fH264Previous == NULL) {
			Close();
			return B_NO_MEMORY;
		}
		h264_init(fH264);
	} else {
		fHevc = new(std::nothrow) HevcState;
		fHevcSlice = new(std::nothrow) HevcSlice;
		fHevcFirst = new(std::nothrow) HevcSlice;
		if (fHevc == NULL || fHevcSlice == NULL || fHevcFirst == NULL) {
			Close();
			return B_NO_MEMORY;
		}
		hevc_init(fHevc);
	}
	Reset();
	return B_OK;
}


void
CedarDecoder::Close()
{
	fOutput.clear();
	for (CedarFrame* frame : fFrames) {
		fEngine.FreeFrame(*frame);
		delete frame;
	}
	fFrames.clear();
	fCurrent = -1;
	fEngine.Unconfigure();
	fDevice.Close();

	delete fH264;
	delete fH264Slice;
	delete fH264First;
	delete fH264Previous;
	delete fHevc;
	delete fHevcSlice;
	delete fHevcFirst;
	fH264 = NULL;
	fH264Slice = fH264First = fH264Previous = NULL;
	fHevc = NULL;
	fHevcSlice = fHevcFirst = NULL;
}


//	#pragma mark - pictures in and out


status_t
CedarDecoder::PutNal(const uint8* nal, size_t size, int64 pts)
{
	if (!fDevice.IsOpen())
		return B_NO_INIT;
	if (fCodec == H264)
		return size >= 1 ? _PutH264(nal, size, pts) : B_OK;
	return size >= 2 ? _PutHevc(nal, size, pts) : B_OK;
}


status_t
CedarDecoder::EndAccessUnit()
{
	_FinishPicture();
	return B_OK;
}


void
CedarDecoder::Drain()
{
	_FinishPicture();
	_BumpAll();
	if (fHevc != NULL)
		fHevc->firstPicture = 1;
}


void
CedarDecoder::Reset()
{
	fDevice.EndPicture();
	fOutput.clear();
	for (CedarFrame* frame : fFrames) {
		frame->current = frame->waiting = frame->ready = frame->held = false;
		frame->position = -1;
	}
	fCurrent = -1;
	fSkipping = false;
	fHavePrevious = false;
	fAwaitRandomAccess = true;

	// the parameter sets stay, the references go
	if (fH264 != NULL) {
		for (int i = 0; i < H264_MAX_REFS; i++)
			fH264->refs[i].ref = 0;
		fH264->maxLongTermFrameIdx = -1;
	}
	if (fHevc != NULL) {
		for (int i = 0; i < HEVC_MAX_DPB; i++)
			fHevc->refs[i].used = 0;
		fHevc->firstPicture = 1;
	}
	fNoRaslOutput = false;
}


bool
CedarDecoder::NextPicture(Picture& picture)
{
	if (fOutput.empty())
		return false;
	int32 index = fOutput.front();
	fOutput.pop_front();

	CedarFrame& frame = *fFrames[index];
	frame.ready = false;
	frame.held = true;
	picture.frame = index;
	picture.pts = frame.pts;
	picture.poc = frame.poc;
	picture.width = frame.width;
	picture.height = frame.height;
	picture.bitDepth = frame.bitDepth;
	picture.corrupt = frame.corrupt;
	return true;
}


void
CedarDecoder::ReleasePicture(const Picture& picture)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	fFrames[picture.frame]->held = false;
}


/*!	\a count samples from \a first on of a row as P010: the eight most
	significant bits from the engine's picture, the low two from its two-bit
	row (four samples to a byte, the first in the lowest bits).
*/
static void
merge_ten_bit(const uint8* row, const uint8* twoBit, uint32 first,
	uint32 count, uint16* target)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	if ((first & 3) == 0 && !sNoNeon) {
		static const uint8 kIndex[16]
			= { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };
		static const int8 kShift[16]
			= { 0, -2, -4, -6, 0, -2, -4, -6, 0, -2, -4, -6, 0, -2, -4, -6 };
		uint8x16_t index = vld1q_u8(kIndex);
		int8x16_t shift = vld1q_s8(kShift);
		uint8x16_t three = vdupq_n_u8(3);
		for (; i + 16 <= count; i += 16) {
			uint8x16_t high = vld1q_u8(row + first + i);
			uint32 packed;
			memcpy(&packed, twoBit + (first + i) / 4, 4);
			uint8x16_t low = vqtbl1q_u8(
				vreinterpretq_u8_u32(vdupq_n_u32(packed)), index);
			low = vandq_u8(vshlq_u8(low, shift), three);
			vst1q_u16(target + i, vorrq_u16(vshll_n_u8(vget_low_u8(high), 8),
				vshll_n_u8(vget_low_u8(low), 6)));
			vst1q_u16(target + i + 8, vorrq_u16(
				vshll_n_u8(vget_high_u8(high), 8),
				vshll_n_u8(vget_high_u8(low), 6)));
		}
	}
#endif
	for (; i < count; i++) {
		uint32 x = first + i;
		uint32 low = (twoBit[x / 4] >> ((x % 4) * 2)) & 3;
		target[i] = (uint16)((((uint32)row[x] << 2) | low) << 6);
	}
}


/*!	Half the row: each sample the mean of a 2x2 block of \a top and
	\a bottom (sixteen bit samples; what the mean has below the ten bits
	stays).
*/
static void
halve_samples16(const uint16* top, const uint16* bottom, uint32 count,
	uint16* target)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	const uint32x4_t two = vdupq_n_u32(2);
	for (; i + 8 <= count; i += 8) {
		uint32x4_t low = vaddq_u32(vpaddlq_u16(vld1q_u16(top + 2 * i)),
			vpaddlq_u16(vld1q_u16(bottom + 2 * i)));
		uint32x4_t high = vaddq_u32(vpaddlq_u16(vld1q_u16(top + 2 * i + 8)),
			vpaddlq_u16(vld1q_u16(bottom + 2 * i + 8)));
		vst1q_u16(target + i, vcombine_u16(
			vmovn_u32(vshrq_n_u32(vaddq_u32(low, two), 2)),
			vmovn_u32(vshrq_n_u32(vaddq_u32(high, two), 2))));
	}
#endif
	for (; i < count; i++) {
		target[i] = (uint16)(((uint32)top[2 * i] + top[2 * i + 1]
			+ bottom[2 * i] + bottom[2 * i + 1] + 2) >> 2);
	}
}


/*!	As halve_samples16(), for rows of Cb Cr pairs: \a count pairs out. */
static void
halve_pairs16(const uint16* top, const uint16* bottom, uint32 count,
	uint16* target)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	const uint32x4_t two = vdupq_n_u32(2);
	for (; i + 4 <= count; i += 4) {
		uint16x8x2_t a = vld2q_u16(top + 4 * i);
		uint16x8x2_t b = vld2q_u16(bottom + 4 * i);
		uint16x4x2_t out;
		out.val[0] = vmovn_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(
			vpaddlq_u16(a.val[0]), vpaddlq_u16(b.val[0])), two), 2));
		out.val[1] = vmovn_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(
			vpaddlq_u16(a.val[1]), vpaddlq_u16(b.val[1])), two), 2));
		vst2_u16(target + 2 * i, out);
	}
#endif
	for (; i < count; i++) {
		for (uint32 c = 0; c < 2; c++) {
			target[2 * i + c] = (uint16)(((uint32)top[4 * i + c]
				+ top[4 * i + 2 + c] + bottom[4 * i + c]
				+ bottom[4 * i + 2 + c] + 2) >> 2);
		}
	}
}


/*!	As halve_samples16(), for eight bit samples. */
static void
halve_samples8(const uint8* top, const uint8* bottom, uint32 count,
	uint8* target)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	for (; i + 8 <= count; i += 8) {
		vst1_u8(target + i, vrshrn_n_u16(vaddq_u16(
			vpaddlq_u8(vld1q_u8(top + 2 * i)),
			vpaddlq_u8(vld1q_u8(bottom + 2 * i))), 2));
	}
#endif
	for (; i < count; i++) {
		target[i] = (uint8)(((uint32)top[2 * i] + top[2 * i + 1]
			+ bottom[2 * i] + bottom[2 * i + 1] + 2) >> 2);
	}
}


/*!	Rows of eight bit Cb Cr pairs halved into a Cb and a Cr row: \a count
	samples of each out. */
static void
halve_pairs8(const uint8* top, const uint8* bottom, uint32 count, uint8* cb,
	uint8* cr)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	for (; i + 16 <= count; i += 16) {
		// Cb, Cr of the even pairs, then of the odd ones
		uint8x16x4_t a = vld4q_u8(top + 4 * i);
		uint8x16x4_t b = vld4q_u8(bottom + 4 * i);
		uint16x8_t cbLow = vaddq_u16(vaddl_u8(vget_low_u8(a.val[0]),
			vget_low_u8(a.val[2])), vaddl_u8(vget_low_u8(b.val[0]),
			vget_low_u8(b.val[2])));
		uint16x8_t cbHigh = vaddq_u16(vaddl_u8(vget_high_u8(a.val[0]),
			vget_high_u8(a.val[2])), vaddl_u8(vget_high_u8(b.val[0]),
			vget_high_u8(b.val[2])));
		uint16x8_t crLow = vaddq_u16(vaddl_u8(vget_low_u8(a.val[1]),
			vget_low_u8(a.val[3])), vaddl_u8(vget_low_u8(b.val[1]),
			vget_low_u8(b.val[3])));
		uint16x8_t crHigh = vaddq_u16(vaddl_u8(vget_high_u8(a.val[1]),
			vget_high_u8(a.val[3])), vaddl_u8(vget_high_u8(b.val[1]),
			vget_high_u8(b.val[3])));
		vst1q_u8(cb + i, vcombine_u8(vrshrn_n_u16(cbLow, 2),
			vrshrn_n_u16(cbHigh, 2)));
		vst1q_u8(cr + i, vcombine_u8(vrshrn_n_u16(crLow, 2),
			vrshrn_n_u16(crHigh, 2)));
	}
#endif
	for (; i < count; i++) {
		cb[i] = (uint8)(((uint32)top[4 * i] + top[4 * i + 2]
			+ bottom[4 * i] + bottom[4 * i + 2] + 2) >> 2);
		cr[i] = (uint8)(((uint32)top[4 * i + 1] + top[4 * i + 3]
			+ bottom[4 * i + 1] + bottom[4 * i + 3] + 2) >> 2);
	}
}


static void
split_chroma(const uint8* source, uint8* cb, uint8* cr, uint32 count)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	for (; i + 16 <= count && !sNoNeon; i += 16) {
		uint8x16x2_t pairs = vld2q_u8(source + 2 * i);
		vst1q_u8(cb + i, pairs.val[0]);
		vst1q_u8(cr + i, pairs.val[1]);
	}
#endif
	for (; i < count; i++) {
		cb[i] = source[2 * i];
		cr[i] = source[2 * i + 1];
	}
}




extern "C" status_t _kern_set_thread_affinity(thread_id id,
	const void* userMask, size_t size);


/*!	Threads that merge and copy the rows of a picture between them while
	the caller waits. Where the processors differ (the A733's two Cortex-A76
	and six A55) they are the slow ones: a player scales and tone maps the
	previous picture on the fast ones meanwhile (airTime does), and copying
	is bound by memory more than by the core. A 4K Main 10 picture took
	13-17 ms on whichever core the caller's thread was on.
	CEDAR_MERGE_THREADS sets how many (0: the caller alone), CEDAR_MERGE_CORES
	which ("fast", "slow" or "all").
*/
class MergePool {
public:
	static MergePool* Default()
	{
		static MergePool* sPool = new(std::nothrow) MergePool;
		return sPool;
	}

	int32 Count() const { return (int32)fThreads.size(); }

	// Calls function(first, last) for bands of [0, rows), returns when all
	// are done.
	void Run(uint32 rows, const std::function<void(uint32, uint32)>& function)
	{
		std::unique_lock<std::mutex> lock(fLock);
		fFunction = &function;
		fRows = rows;
		fGrain = rows / (fThreads.size() * 8) + 1;
		fNext.store(0);
		fBusy = fThreads.size();
		fGeneration++;
		fWake.notify_all();
		fDone.wait(lock, [this] { return fBusy == 0; });
		fFunction = NULL;
	}

private:
	MergePool()
		:
		fFunction(NULL),
		fRows(0),
		fGrain(1),
		fBusy(0),
		fGeneration(0)
	{
		const char* cores = getenv("CEDAR_MERGE_CORES");
		std::vector<int32> processors = _Processors(cores != NULL
			&& strcmp(cores, "fast") == 0 ? kFast
			: cores != NULL && strcmp(cores, "all") == 0 ? kAll : kSlow);
		int32 count = (int32)processors.size();
		if (const char* value = getenv("CEDAR_MERGE_THREADS"))
			count = atoi(value);
		for (int32 i = 0; i < count; i++) {
			int32 processor = processors.empty() ? -1
				: processors[i % processors.size()];
			fThreads.emplace_back(&MergePool::_Work, this, processor);
		}
	}

	enum which { kFast, kSlow, kAll };

	/*!	The processors of one kind where there are two (the slow ones by
		their default frequency), else all of them. */
	static std::vector<int32> _Processors(which kind)
	{
		std::vector<int32> chosen;
		cpu_topology_node_info nodes[64];
		uint32 count = 64;
		if (get_cpu_topology_info(nodes, &count) != B_OK)
			return chosen;
		uint64 fastest = 0;
		uint64 slowest = UINT64_MAX;
		for (uint32 i = 0; i < count; i++) {
			if (nodes[i].type != B_TOPOLOGY_CORE)
				continue;
			fastest = max_c(fastest, nodes[i].data.core.default_frequency);
			slowest = min_c(slowest, nodes[i].data.core.default_frequency);
		}
		uint64 frequency = 0;
		for (uint32 i = 0; i < count; i++) {
			if (nodes[i].type == B_TOPOLOGY_CORE)
				frequency = nodes[i].data.core.default_frequency;
			else if (nodes[i].type == B_TOPOLOGY_SMT && nodes[i].id < 64
				&& (kind == kAll || fastest == slowest
					|| frequency == (kind == kFast ? fastest : slowest))) {
				chosen.push_back(nodes[i].id);
			}
		}
		return chosen;
	}

	void _Work(int32 processor)
	{
		// as the player's decoding thread: a picture waits on these
		rename_thread(find_thread(NULL), "sunxi_cedar copy");
		set_thread_priority(find_thread(NULL), B_DISPLAY_PRIORITY);
		if (processor >= 0) {
			uint32 mask[2] = { 0, 0 };
			mask[processor / 32] |= 1u << (processor % 32);
			_kern_set_thread_affinity(find_thread(NULL), mask, sizeof(mask));
		}
		uint64 seen = 0;
		std::unique_lock<std::mutex> lock(fLock);
		while (true) {
			fWake.wait(lock, [&] { return fGeneration != seen; });
			seen = fGeneration;
			const std::function<void(uint32, uint32)>* function = fFunction;
			uint32 rows = fRows;
			uint32 grain = fGrain;
			lock.unlock();
			while (true) {
				uint32 first = fNext.fetch_add(grain);
				if (first >= rows)
					break;
				(*function)(first, min_c(first + grain, rows));
			}
			lock.lock();
			if (--fBusy == 0)
				fDone.notify_one();
		}
	}

	std::vector<std::thread> fThreads;
	std::mutex fLock;
	std::condition_variable fWake;
	std::condition_variable fDone;
	const std::function<void(uint32, uint32)>* fFunction;
	uint32 fRows;
	uint32 fGrain;
	std::atomic<uint32> fNext;
	size_t fBusy;
	uint64 fGeneration;
};


void
CedarDecoder::CopyPlanes(const Picture& picture, uint8* target, uint32 stride,
	bool interleaved)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	const CedarFrame& frame = *fFrames[picture.frame];

	if (frame.bitDepth > 8) {
		// P010: the eight bit picture's samples, with their low two bits
		fDevice.SyncForCpu(frame.picture, 0, frame.picture.size);
		const uint8* twoBit = frame.picture.address + frame.twoBitOffset;
		const uint8* chroma = frame.picture.address + frame.lumaSize;
		const uint8* twoBitChroma = frame.picture.address + frame.twoBitChroma;
		uint8* chromaTarget = target + (size_t)stride * frame.height;
		uint32 chromaRows = (frame.height + 1) / 2;
		// luma rows, then chroma rows, as one run of rows
		auto merge = [&](uint32 first, uint32 last) {
			for (uint32 row = first; row < last; row++) {
				if (row < frame.height) {
					uint32 y = frame.cropTop + row;
					merge_ten_bit(frame.picture.address + (size_t)y * frame.stride,
						twoBit + (size_t)y * frame.twoBitStride, frame.cropLeft,
						frame.width, (uint16*)(target + (size_t)row * stride));
				} else {
					uint32 chromaRow = row - frame.height;
					uint32 y = frame.cropTop / 2 + chromaRow;
					merge_ten_bit(chroma + (size_t)y * frame.stride,
						twoBitChroma + (size_t)y * frame.twoBitStride,
						frame.cropLeft & ~1u, ((frame.width + 1) / 2) * 2,
						(uint16*)(chromaTarget + (size_t)chromaRow * stride));
				}
			}
		};
		MergePool* pool = MergePool::Default();
		if (pool != NULL && pool->Count() > 0)
			pool->Run(frame.height + chromaRows, merge);
		else
			merge(0, frame.height + chromaRows);
		return;
	}

	// what the engine wrote is in memory, the cache may have older lines
	fDevice.SyncForCpu(frame.picture, 0, frame.lumaSize + frame.lumaSize / 2);

	const uint8* luma = frame.picture.address
		+ (size_t)frame.cropTop * frame.stride + frame.cropLeft;
	for (uint32 row = 0; row < frame.height; row++) {
		memcpy(target + (size_t)row * stride,
			luma + (size_t)row * frame.stride, frame.width);
	}

	const uint8* chroma = frame.picture.address + frame.lumaSize
		+ (size_t)(frame.cropTop / 2) * frame.stride + (frame.cropLeft & ~1u);
	uint8* chromaTarget = target + (size_t)stride * frame.height;
	uint32 rows = (frame.height + 1) / 2;
	uint32 pairs = (frame.width + 1) / 2;
	if (interleaved) {
		for (uint32 row = 0; row < rows; row++) {
			memcpy(chromaTarget + (size_t)row * stride,
				chroma + (size_t)row * frame.stride, pairs * 2);
		}
		return;
	}
	uint32 halfStride = stride / 2;
	uint8* cb = chromaTarget;
	uint8* cr = chromaTarget + (size_t)halfStride * rows;
	for (uint32 row = 0; row < rows; row++) {
		split_chroma(chroma + (size_t)row * frame.stride,
			cb + (size_t)row * halfStride, cr + (size_t)row * halfStride,
			pairs);
	}
}


/*!	One row pair of 4:2:0 as Y0 Cb Y1 Cr: \a pairs chroma samples. */
static void
pack_ycbcr422(const uint8* luma, const uint8* chroma, uint8* target,
	uint32 pairs)
{
	uint32 i = 0;
#if defined(__ARM_NEON)
	for (; i + 16 <= pairs && !sNoNeon; i += 16) {
		uint8x16x2_t y = vld2q_u8(luma + 2 * i);
		uint8x16x2_t c = vld2q_u8(chroma + 2 * i);
		uint8x16x4_t out;
		out.val[0] = y.val[0];
		out.val[1] = c.val[0];
		out.val[2] = y.val[1];
		out.val[3] = c.val[1];
		vst4q_u8(target + 4 * i, out);
	}
#endif
	for (; i < pairs; i++) {
		target[4 * i] = luma[2 * i];
		target[4 * i + 1] = chroma[2 * i];
		target[4 * i + 2] = luma[2 * i + 1];
		target[4 * i + 3] = chroma[2 * i + 1];
	}
}


static inline uint8
clamp_sample(int32 value)
{
	return value < 0 ? 0 : value > 255 ? 255 : (uint8)value;
}


/*!	One row of 4:2:0 as B_RGB32 (B, G, R, A in memory), studio range. The
	matrix is the luma scale and the chroma terms (Cr to R, Cb to G, Cr to
	G, Cb to B) in 1/64.
*/
static void
pack_rgb32(const uint8* luma, const uint8* chroma, uint8* target,
	uint32 width, const int16* matrix)
{
	uint32 x = 0;
#if defined(__ARM_NEON)
	int16x8_t sixteen = vdupq_n_s16(16);
	int16x8_t half = vdupq_n_s16(128);
	uint8x8_t opaque = vdup_n_u8(255);
	for (; x + 16 <= width && !sNoNeon; x += 16) {
		uint8x16_t y = vld1q_u8(luma + x);
		uint8x8x2_t c = vld2_u8(chroma + x);
		int16x8_t cb = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(c.val[0])),
			half);
		int16x8_t cr = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(c.val[1])),
			half);
		int16x8_t r = vmulq_n_s16(cr, matrix[1]);
		int16x8_t g = vaddq_s16(vmulq_n_s16(cb, matrix[2]),
			vmulq_n_s16(cr, matrix[3]));
		int16x8_t b = vmulq_n_s16(cb, matrix[4]);
		// each chroma sample for two luma samples
		int16x8x2_t rr = vzipq_s16(r, r);
		int16x8x2_t gg = vzipq_s16(g, g);
		int16x8x2_t bb = vzipq_s16(b, b);
		for (int h = 0; h < 2; h++) {
			uint8x8_t yHalf = h == 0 ? vget_low_u8(y) : vget_high_u8(y);
			int16x8_t yy = vmulq_n_s16(vsubq_s16(
				vreinterpretq_s16_u16(vmovl_u8(yHalf)), sixteen), matrix[0]);
			uint8x8x4_t pixels;
			pixels.val[0] = vqrshrun_n_s16(vqaddq_s16(yy, bb.val[h]), 6);
			pixels.val[1] = vqrshrun_n_s16(vqsubq_s16(yy, gg.val[h]), 6);
			pixels.val[2] = vqrshrun_n_s16(vqaddq_s16(yy, rr.val[h]), 6);
			pixels.val[3] = opaque;
			vst4_u8(target + 4 * (x + 8 * h), pixels);
		}
	}
#endif
	for (; x < width; x++) {
		int32 y = ((int32)luma[x] - 16) * matrix[0];
		int32 cb = (int32)chroma[x & ~1u] - 128;
		int32 cr = (int32)chroma[(x & ~1u) + 1] - 128;
		target[4 * x + 2] = clamp_sample((y + matrix[1] * cr + 32) >> 6);
		target[4 * x + 1] = clamp_sample((y - matrix[2] * cb - matrix[3] * cr
			+ 32) >> 6);
		target[4 * x] = clamp_sample((y + matrix[4] * cb + 32) >> 6);
		target[4 * x + 3] = 255;
	}
}


void
CedarDecoder::CopyHalfPlanes(const Picture& picture, uint8* target,
	uint32 stride)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	const CedarFrame& frame = *fFrames[picture.frame];
	bool tenBit = frame.bitDepth > 8;
	uint32 width = frame.width / 2;
	uint32 height = frame.height / 2;
	uint32 chromaRows = height / 2;

	// what the engine wrote is in memory, the cache may have older lines
	fDevice.SyncForCpu(frame.picture, 0,
		tenBit ? frame.picture.size : frame.lumaSize + frame.lumaSize / 2);

	const uint8* luma = frame.picture.address;
	const uint8* chroma = frame.picture.address + frame.lumaSize;
	const uint8* twoBit = frame.picture.address + frame.twoBitOffset;
	const uint8* twoBitChroma = frame.picture.address + frame.twoBitChroma;
	uint8* chromaTarget = target + (size_t)stride * height;
	uint32 halfStride = stride / 2;
	uint8* cbTarget = chromaTarget;
	uint8* crTarget = chromaTarget + (size_t)halfStride * chromaRows;

	// luma rows, then chroma rows, as one run of rows
	auto halve = [&](uint32 first, uint32 last) {
		static thread_local std::vector<uint16> top, bottom;
		if (tenBit) {
			top.resize(frame.width);
			bottom.resize(frame.width);
		}
		for (uint32 row = first; row < last; row++) {
			if (row < height) {
				uint32 y = frame.cropTop + 2 * row;
				uint16* out = (uint16*)(target + (size_t)row * stride);
				if (tenBit) {
					merge_ten_bit(luma + (size_t)y * frame.stride,
						twoBit + (size_t)y * frame.twoBitStride,
						frame.cropLeft, frame.width, top.data());
					merge_ten_bit(luma + (size_t)(y + 1) * frame.stride,
						twoBit + (size_t)(y + 1) * frame.twoBitStride,
						frame.cropLeft, frame.width, bottom.data());
					halve_samples16(top.data(), bottom.data(), width, out);
				} else {
					halve_samples8(luma + (size_t)y * frame.stride
							+ frame.cropLeft,
						luma + (size_t)(y + 1) * frame.stride + frame.cropLeft,
						width, target + (size_t)row * stride);
				}
				continue;
			}
			uint32 chromaRow = row - height;
			uint32 y = frame.cropTop / 2 + 2 * chromaRow;
			uint32 left = frame.cropLeft & ~1u;
			if (tenBit) {
				merge_ten_bit(chroma + (size_t)y * frame.stride,
					twoBitChroma + (size_t)y * frame.twoBitStride, left,
					frame.width, top.data());
				merge_ten_bit(chroma + (size_t)(y + 1) * frame.stride,
					twoBitChroma + (size_t)(y + 1) * frame.twoBitStride, left,
					frame.width, bottom.data());
				halve_pairs16(top.data(), bottom.data(), width / 2,
					(uint16*)(chromaTarget + (size_t)chromaRow * stride));
			} else {
				halve_pairs8(chroma + (size_t)y * frame.stride + left,
					chroma + (size_t)(y + 1) * frame.stride + left, width / 2,
					cbTarget + (size_t)chromaRow * halfStride,
					crTarget + (size_t)chromaRow * halfStride);
			}
		}
	};
	MergePool* pool = MergePool::Default();
	if (pool != NULL && pool->Count() > 0)
		pool->Run(height + chromaRows, halve);
	else
		halve(0, height + chromaRows);
}


void
CedarDecoder::CopyPacked(const Picture& picture, uint8* target, uint32 stride,
	color_space space)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	const CedarFrame& frame = *fFrames[picture.frame];
	fDevice.SyncForCpu(frame.picture, 0, frame.lumaSize + frame.lumaSize / 2);

	// luma scale (255/219), then Cr to R, Cb to G, Cr to G, Cb to B with
	// studio range chroma (255/224), all in 1/64
	static const int16 kBt601[] = { 75, 102, 25, 52, 129 };
	static const int16 kBt709[] = { 75, 115, 14, 34, 135 };
	const int16* matrix = frame.height >= 720 ? kBt709 : kBt601;

	const uint8* luma = frame.picture.address
		+ (size_t)frame.cropTop * frame.stride + frame.cropLeft;
	const uint8* chroma = frame.picture.address + frame.lumaSize
		+ (size_t)(frame.cropTop / 2) * frame.stride + (frame.cropLeft & ~1u);
	for (uint32 row = 0; row < frame.height; row++) {
		const uint8* lumaRow = luma + (size_t)row * frame.stride;
		const uint8* chromaRow = chroma + (size_t)(row / 2) * frame.stride;
		uint8* targetRow = target + (size_t)row * stride;
		if (space == B_YCbCr422)
			pack_ycbcr422(lumaRow, chromaRow, targetRow, frame.width / 2);
		else
			pack_rgb32(lumaRow, chromaRow, targetRow, frame.width, matrix);
	}
}


size_t
CedarDecoder::CopyTwoBit(const Picture& picture, uint8* target, size_t size)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return 0;
	const CedarFrame& frame = *fFrames[picture.frame];
	if (frame.bitDepth <= 8)
		return 0;
	size_t available = frame.picture.size - frame.twoBitOffset;
	if (size > available)
		size = available;
	fDevice.SyncForCpu(frame.picture, frame.twoBitOffset, size);
	memcpy(target, frame.picture.address + frame.twoBitOffset, size);
	return size;
}


//	#pragma mark - frames


bool
CedarDecoder::_IsReference(int32 index) const
{
	if (fH264 != NULL) {
		for (int i = 0; i < H264_MAX_REFS; i++) {
			if (fH264->refs[i].ref != 0 && fH264->refs[i].frame == index)
				return true;
		}
		return false;
	}
	for (int i = 0; i < HEVC_MAX_DPB; i++) {
		if (fHevc->refs[i].used != 0 && fHevc->refs[i].frame == index)
			return true;
	}
	return false;
}


bool
CedarDecoder::_IsFree(int32 index) const
{
	const CedarFrame& frame = *fFrames[index];
	return !frame.current && !frame.waiting && !frame.ready && !frame.held
		&& !_IsReference(index);
}


uint32
CedarDecoder::_CountWaiting() const
{
	uint32 count = 0;
	for (const CedarFrame* frame : fFrames) {
		if (frame->waiting)
			count++;
	}
	return count;
}


/*!	The pictures the stream's decoded picture buffer holds besides the
	current one: references and those waiting to be shown.
*/
uint32
CedarDecoder::_CountHeld() const
{
	uint32 count = 0;
	for (size_t i = 0; i < fFrames.size(); i++) {
		if ((int32)i != fCurrent
			&& (fFrames[i]->waiting || _IsReference((int32)i))) {
			count++;
		}
	}
	return count;
}


/*!	The first picture in output order of those waiting goes out. */
bool
CedarDecoder::_Bump()
{
	int32 first = -1;
	for (size_t i = 0; i < fFrames.size(); i++) {
		if (fFrames[i]->waiting
			&& (first < 0 || fFrames[i]->poc < fFrames[first]->poc)) {
			first = (int32)i;
		}
	}
	if (first < 0)
		return false;
	fFrames[first]->waiting = false;
	fFrames[first]->ready = true;
	fOutput.push_back(first);
	return true;
}


void
CedarDecoder::_BumpAll()
{
	while (_Bump()) {
	}
}


void
CedarDecoder::_DropWaiting()
{
	for (CedarFrame* frame : fFrames)
		frame->waiting = false;
}


/*!	A frame for the picture that starts, with memory for the engine's
	current geometry; it becomes fCurrent.
*/
status_t
CedarDecoder::_NewFrame(uint32 width, uint32 height, uint32 cropLeft,
	uint32 cropTop, uint32 bitDepth, int64 pts)
{
	int32 index = -1;
	for (size_t i = 0; i < fFrames.size(); i++) {
		if (!_IsFree((int32)i))
			continue;
		const CedarFrame& frame = *fFrames[i];
		bool fits = frame.picture.IsValid()
			&& frame.picture.size >= fEngine.FrameSize()
			&& frame.mvcol.size >= fEngine.MvcolSize();
		// one with fitting memory, else any
		if (fits) {
			index = (int32)i;
			break;
		}
		if (index < 0)
			index = (int32)i;
	}
	if (index < 0) {
		if (fFrames.size() >= kMaxFrames) {
			return _Fail(B_NO_MEMORY, "all %" B_PRIuSIZE " pictures are in"
				" use", kMaxFrames);
		}
		CedarFrame* frame = new(std::nothrow) CedarFrame;
		if (frame == NULL)
			return B_NO_MEMORY;
		fFrames.push_back(frame);
		index = (int32)fFrames.size() - 1;
	}

	CedarFrame& frame = *fFrames[index];
	if (!frame.picture.IsValid() || frame.picture.size < fEngine.FrameSize()
		|| frame.mvcol.size < fEngine.MvcolSize()) {
		fEngine.FreeFrame(frame);
		status_t status = fEngine.AllocateFrame(frame);
		if (status != B_OK) {
			return _Fail(status, "no memory for a picture of %" B_PRIu32 "x%"
				B_PRIu32 ": %s", fEngine.CodedWidth(), fEngine.CodedHeight(),
				strerror(status));
		}
	}

	frame.position = -1;
	frame.poc = 0;
	frame.pts = pts;
	frame.current = true;
	frame.waiting = frame.ready = frame.held = false;
	frame.corrupt = false;
	frame.latency = 0;
	frame.stride = fEngine.Stride();
	frame.lumaSize = fEngine.LumaSize();
	frame.cropLeft = cropLeft;
	frame.cropTop = cropTop;
	frame.width = width;
	frame.height = height;
	frame.bitDepth = bitDepth;
	frame.twoBitOffset = fEngine.TwoBitOffset();
	frame.twoBitChroma = fEngine.TwoBitChroma();
	frame.twoBitStride = fEngine.TwoBitStride();
	fCurrent = index;
	fFirstSlice = true;
	return B_OK;
}


void
CedarDecoder::_FinishPicture()
{
	if (fCurrent >= 0) {
		if (fCodec == H264)
			_FinishH264Picture();
		else
			_FinishHevcPicture();
		// other programs' slices may come again
		fDevice.EndPicture();
	}
	fCurrent = -1;
	fSkipping = false;
}


//	#pragma mark - H.264


status_t
CedarDecoder::_PutH264(const uint8* nal, size_t size, int64 pts)
{
	int type = nal[0] & 0x1f;
	switch (type) {
		case 1:		// a slice
		case 5:		// of an IDR picture
			return _H264Slice(nal, size, pts);

		case 2:		// a slice in data partitions (Extended profile)
		case 3:
		case 4:
			return _Fail(B_NOT_SUPPORTED, "the engine does not do this stream: "
				"data partitioning");

		case 7:		// sequence parameter set
		case 8:		// picture parameter set
		{
			_FinishPicture();
			Rbsp rbsp;
			if (rbsp_from_nal(&rbsp, nal, size) != 0)
				return B_NO_MEMORY;
			BitReader reader;
			br_init(&reader, rbsp.data, rbsp.size);
			int result = type == 7
				? h264_parse_sps(fH264, &reader, fParseError)
				: h264_parse_pps(fH264, &reader, fParseError);
			rbsp_free(&rbsp);
			if (result != 0) {
				TRACE("a parameter set that cannot be used: %s\n",
					fParseError);
			}
			return B_OK;
		}

		case 6:		// SEI
		case 9:		// access unit delimiter
		case 10:	// end of sequence
		case 11:	// end of stream
		case 14:
		case 15:
		case 16:
		case 17:
		case 18:
			// a new access unit (7.4.1.2.3)
			_FinishPicture();
			return B_OK;
	}
	return B_OK;
}


status_t
CedarDecoder::_H264Slice(const uint8* nal, size_t size, int64 pts)
{
	Rbsp rbsp;
	if (rbsp_from_nal(&rbsp, nal, size) != 0)
		return B_NO_MEMORY;
	BitReader reader;
	br_init(&reader, rbsp.data, rbsp.size);
	int result = h264_parse_slice(fH264, &reader, fH264Slice, fParseError);
	rbsp_free(&rbsp);
	if (result == -2) {
		return _Fail(B_NOT_SUPPORTED, "the engine does not do this stream: "
			"%s", fParseError);
	}
	if (result != 0) {
		TRACE("a slice header that cannot be read: %s\n", fParseError);
		return B_OK;
	}
	const H264Slice& slice = *fH264Slice;

	// (the rest of a picture that was ended early is dropped)
	bool newPicture = h264_new_picture(fHavePrevious ? fH264Previous : NULL,
		&slice) != 0;
	*fH264Previous = slice;
	fHavePrevious = true;

	if (newPicture) {
		_FinishPicture();
		status_t status = _StartH264Picture(slice, pts);
		if (status != B_OK)
			return status;
	}
	if (fSkipping || fCurrent < 0)
		return B_OK;

	CedarFrame& current = *fFrames[fCurrent];
	int list0[32], list1[32];
	h264_ref_lists(fH264, &slice, list0, list1);

	// a reference that is not there (a gap, a broken stream, decoding
	// that started at a picture that is no IDR): another one stands in,
	// and the picture is not shown
	bool predicted = slice.sliceType == 0 || slice.sliceType == 1
		|| slice.sliceType == 3;
	if (predicted) {
		int standIn = -1;
		for (int i = 0; i < H264_MAX_REFS && standIn < 0; i++) {
			const H264Ref& ref = fH264->refs[i];
			if (ref.ref != 0 && ref.frame >= 0
				&& fFrames[ref.frame]->position > 0) {
				standIn = i;
			}
		}
		for (int l = 0; l < 2; l++) {
			int* list = l == 0 ? list0 : list1;
			for (int i = 0; i < slice.numRefIdxActive[l] && i < 32; i++) {
				if (list[i] >= 0) {
					const H264Ref& ref = fH264->refs[list[i]];
					if (ref.frame >= 0 && fFrames[ref.frame]->position > 0)
						continue;
					// a frame of a frame_num gap is in the list but never
					// predicted from in a conforming stream
					if (ref.nonExisting) {
						list[i] = standIn;
						continue;
					}
				}
				list[i] = standIn;
				current.corrupt = true;
			}
		}
		if (standIn < 0) {
			TRACE("picture %" B_PRId32 ": nothing to refer to\n",
				fH264->curPoc);
			return B_OK;
		}
	}

	status_t status = fEngine.DecodeH264Slice(*fH264, slice, current,
		fFrames.data(), fFirstSlice, list0, list1, nal, size);
	fFirstSlice = false;
	if (status != B_OK) {
		fprintf(stderr, "sunxi_cedar: picture %" B_PRIu32 ", slice at MB %d:"
			" %s\n", fDecoded, slice.firstMb, fEngine.LastError());
		current.corrupt = true;
	}
	return B_OK;
}


int32
CedarDecoder::_FreeH264Position() const
{
	bool used[H264_FRAME_SLOTS] = {};
	for (int i = 0; i < H264_MAX_REFS; i++) {
		if (fH264->refs[i].ref == 0 || fH264->refs[i].frame < 0)
			continue;
		int32 position = fFrames[fH264->refs[i].frame]->position;
		if (position > 0 && position < H264_FRAME_SLOTS)
			used[position] = true;
	}
	// position 0 is not used (the T527's and the A733's engines)
	for (int32 position = 1; position < H264_FRAME_SLOTS; position++) {
		if (!used[position])
			return position;
	}
	return -1;
}


status_t
CedarDecoder::_StartH264Picture(const H264Slice& slice, int64 pts)
{
	const H264Pps& pps = fH264->pps[slice.ppsId];
	const H264Sps& sps = fH264->sps[pps.spsId];

	// decoding starts (again) at an IDR picture, or at an intra picture
	// that the ones after it may refer to
	if (fAwaitRandomAccess) {
		if (!slice.idr && slice.sliceType != 2 && slice.sliceType != 4) {
			fSkipping = true;
			return B_OK;
		}
		fAwaitRandomAccess = false;
	}

	// a picture nothing refers to that nobody wants to see: only its
	// order count is of interest
	if (slice.nalRefIdc == 0 && pts < fSkipBefore) {
		h264_start_picture(fH264, &slice);
		h264_finish_picture(fH264, &slice, -1);
		fSkipping = true;
		return B_OK;
	}

	uint32 width = sps.widthMbs * 16;
	uint32 height = sps.heightMapUnits * 16;
	if (width > kMaxSize || height > kMaxSize) {
		return _Fail(B_NOT_SUPPORTED, "the engine decodes up to %" B_PRIu32
			"x%" B_PRIu32 ", this is %" B_PRIu32 "x%" B_PRIu32, kMaxSize,
			kMaxSize, width, height);
	}
	status_t status = fEngine.ConfigureH264(sps);
	if (status != B_OK)
		return _Fail(status, "no memory for the engine: %s", strerror(status));

	fDpbSize = sps.maxDecFrameBuffering;
	if (fDpbSize < (uint32)sps.maxNumRefFrames)
		fDpbSize = sps.maxNumRefFrames;
	if (fDpbSize < 1)
		fDpbSize = 1;
	if (fDpbSize > 16)
		fDpbSize = 16;
	fReorder = sps.numReorderFrames;
	if (fReorder > fDpbSize)
		fReorder = fDpbSize;

	h264_start_picture(fH264, &slice);

	status = _NewFrame(width - sps.cropLeft - sps.cropRight,
		height - sps.cropTop - sps.cropBottom, sps.cropLeft, sps.cropTop, 8,
		pts);
	if (status != B_OK)
		return status;
	CedarFrame& frame = *fFrames[fCurrent];
	frame.position = _FreeH264Position();
	frame.poc = fH264->curPoc;
	*fH264First = slice;
	return B_OK;
}


void
CedarDecoder::_FinishH264Picture()
{
	CedarFrame& frame = *fFrames[fCurrent];
	const H264Slice& first = *fH264First;

	// 8.2.5: its marking, and what that leaves of the others
	h264_finish_picture(fH264, &first, fCurrent);
	frame.current = false;
	if (!_IsReference(fCurrent))
		frame.position = -1;

	// C.4.4: an IDR picture or one that resets the order counts first
	// sends out (or drops) all before it
	if (first.idr || fH264->curHadMmco5) {
		if (first.idr && first.noOutputOfPriorPics)
			_DropWaiting();
		else
			_BumpAll();
		if (fH264->curHadMmco5)
			frame.poc = 0;
	}

	// C.4.5.2 and C.4.5.3: no room in the picture buffer. A picture
	// nothing refers to that comes before all that wait goes out at once.
	bool reference = _IsReference(fCurrent);
	bool shown = false;
	while (_CountHeld() >= fDpbSize) {
		if (!reference) {
			bool first = true;
			for (const CedarFrame* other : fFrames) {
				if (other->waiting && other->poc < frame.poc)
					first = false;
			}
			if (first) {
				if (!frame.corrupt) {
					frame.ready = true;
					fOutput.push_back(fCurrent);
				}
				shown = true;
				break;
			}
		}
		if (!_Bump())
			break;
	}

	if (!shown) {
		frame.waiting = !frame.corrupt;
		while (_CountWaiting() > fReorder && _Bump()) {
		}
	}
	fDecoded++;
}


//	#pragma mark - HEVC


status_t
CedarDecoder::_PutHevc(const uint8* nal, size_t size, int64 pts)
{
	int type = (nal[0] >> 1) & 0x3f;
	int layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
	if (layer != 0)
		return B_OK;

	if (type <= 31) {
		// reserved ones of the slice types
		if (type > 21 || (type > 9 && type < 16))
			return B_OK;
		return _HevcSlice(nal, size, pts);
	}

	switch (type) {
		case 33:	// sequence parameter set
		case 34:	// picture parameter set
		{
			_FinishPicture();
			Rbsp rbsp;
			if (rbsp_from_nal(&rbsp, nal, size) != 0)
				return B_NO_MEMORY;
			BitReader reader;
			br_init(&reader, rbsp.data, rbsp.size);
			int result = type == 33
				? hevc_parse_sps(fHevc, &reader, fParseError)
				: hevc_parse_pps(fHevc, &reader, fParseError);
			rbsp_free(&rbsp);
			if (result != 0) {
				TRACE("a parameter set that cannot be used: %s\n",
					fParseError);
			}
			return B_OK;
		}

		case 36:	// end of sequence
		case 37:	// end of bitstream
			_FinishPicture();
			fHevc->firstPicture = 1;
			return B_OK;

		case 32:	// video parameter set
		case 35:	// access unit delimiter
		case 39:	// SEI before the picture
			_FinishPicture();
			return B_OK;
	}
	return B_OK;
}


status_t
CedarDecoder::_HevcSlice(const uint8* nal, size_t size, int64 pts)
{
	Rbsp rbsp;
	if (rbsp_from_nal(&rbsp, nal, size) != 0)
		return B_NO_MEMORY;
	BitReader reader;
	br_init(&reader, rbsp.data, rbsp.size);
	int result = hevc_parse_slice(fHevc, &reader, &rbsp, fHevcSlice,
		fParseError);
	rbsp_free(&rbsp);
	if (result == -2) {
		return _Fail(B_NOT_SUPPORTED, "the engine does not do this stream: "
			"%s", fParseError);
	}
	if (result != 0) {
		TRACE("a slice header that cannot be read: %s\n", fParseError);
		return B_OK;
	}
	const HevcSlice& slice = *fHevcSlice;

	if (slice.firstSliceSegmentInPic) {
		_FinishPicture();
		status_t status = _StartHevcPicture(slice, pts);
		if (status != B_OK)
			return status;
	}
	if (fSkipping || fCurrent < 0)
		return B_OK;
	if (slice.ppsId != fHevcFirst->ppsId)
		return B_OK;

	CedarFrame& current = *fFrames[fCurrent];
	int list0[16], list1[16];
	if (hevc_ref_lists(fHevc, &slice, list0, list1, fParseError) != 0) {
		TRACE("picture %" B_PRId32 ": %s\n", fHevc->curPoc, fParseError);
		current.corrupt = true;
		return B_OK;
	}

	status_t status = fEngine.DecodeHevcSlice(*fHevc, slice, current,
		fFrames.data(), list0, list1, nal, size);
	fFirstSlice = false;
	if (status != B_OK) {
		fprintf(stderr, "sunxi_cedar: picture %" B_PRIu32 ", slice at CTB %d:"
			" %s\n", fDecoded, slice.segmentAddress, fEngine.LastError());
		current.corrupt = true;
	}
	return B_OK;
}


status_t
CedarDecoder::_StartHevcPicture(const HevcSlice& slice, int64 pts)
{
	const HevcPps& pps = fHevc->pps[slice.ppsId];
	const HevcSps& sps = fHevc->sps[pps.spsId];
	int type = slice.nalType;
	bool randomAccess = hevc_is_irap(type);

	if (fAwaitRandomAccess && !randomAccess) {
		fSkipping = true;
		return B_OK;
	}
	// the pictures that lead up to the random access point decoding
	// started at cannot be decoded
	if ((type == 8 || type == 9) && fNoRaslOutput) {
		fSkipping = true;
		return B_OK;
	}
	// a picture nobody wants to see and nothing refers to: one of the
	// highest sub-layer that says so in its type
	if (pts < fSkipBefore && hevc_is_sub_layer_non_reference(type)
		&& slice.temporalId >= sps.maxSubLayersMinus1) {
		fSkipping = true;
		return B_OK;
	}
	fAwaitRandomAccess = false;

	if ((sps.bitDepthLuma != 8 && sps.bitDepthLuma != 10)
		|| sps.bitDepthChroma != sps.bitDepthLuma) {
		return _Fail(B_NOT_SUPPORTED, "the engine decodes eight and ten bit "
			"HEVC, this is %d/%d bit", sps.bitDepthLuma, sps.bitDepthChroma);
	}
	if ((uint32)sps.width > kMaxSize || (uint32)sps.height > kMaxSize) {
		return _Fail(B_NOT_SUPPORTED, "the engine decodes up to %" B_PRIu32
			"x%" B_PRIu32 ", this is %dx%d", kMaxSize, kMaxSize, sps.width,
			sps.height);
	}

	// C.5.2.2: at a random access point that shows nothing of what came
	// before, all that waits goes out first (or is dropped)
	bool noRaslOutput = randomAccess && (hevc_is_idr(type) || type <= 18
		|| fHevc->firstPicture);
	if (noRaslOutput) {
		// a CRA picture there drops them whatever its flag says
		if (slice.noOutputOfPriorPics || type == 21)
			_DropWaiting();
		else
			_BumpAll();
	}

	if (hevc_start_picture(fHevc, &slice, fParseError) != 0)
		return _Fail(B_BAD_DATA, "%s", fParseError);
	if (randomAccess)
		fNoRaslOutput = fHevc->noRaslOutput != 0;

	status_t status = fEngine.ConfigureHevc(sps);
	if (status != B_OK)
		return _Fail(status, "no memory for the engine: %s", strerror(status));

	if (!noRaslOutput) {
		uint32 reorder = sps.maxNumReorder;
		uint32 buffering = sps.maxDecPicBuffering;
		while (true) {
			uint32 waiting = _CountWaiting();
			if (waiting == 0
				|| (waiting <= reorder && _CountHeld() < buffering)) {
				break;
			}
			_Bump();
		}
	}

	status = _NewFrame(sps.width - sps.cropLeft - sps.cropRight,
		sps.height - sps.cropTop - sps.cropBottom, sps.cropLeft, sps.cropTop,
		sps.bitDepthLuma, pts);
	if (status != B_OK)
		return status;
	CedarFrame& frame = *fFrames[fCurrent];
	frame.poc = fHevc->curPoc;
	frame.corrupt = fHevc->missingReference != 0;
	*fHevcFirst = slice;
	return B_OK;
}


void
CedarDecoder::_FinishHevcPicture()
{
	CedarFrame& frame = *fFrames[fCurrent];
	const HevcSlice& first = *fHevcFirst;
	const HevcSps& sps = fHevc->sps[fHevc->pps[first.ppsId].spsId];

	frame.current = false;
	if (hevc_finish_picture(fHevc, &first, fCurrent) < 0)
		fprintf(stderr, "sunxi_cedar: more references than HEVC allows\n");

	// a picture that did not decode is not shown, but stays to be
	// referred to: the ones after it are less wrong with it than without
	frame.waiting = first.picOutputFlag && !frame.corrupt;
	frame.latency = 0;
	if (frame.waiting) {
		for (size_t i = 0; i < fFrames.size(); i++) {
			if ((int32)i != fCurrent && fFrames[i]->waiting)
				fFrames[i]->latency++;
		}
	}
	fDecoded++;

	// C.5.2.3
	uint32 reorder = sps.maxNumReorder;
	uint32 maxLatency = sps.maxLatencyIncreasePlus1 != 0
		? reorder + sps.maxLatencyIncreasePlus1 - 1 : 0;
	while (true) {
		uint32 waiting = 0;
		bool late = false;
		for (const CedarFrame* other : fFrames) {
			if (!other->waiting)
				continue;
			waiting++;
			if (maxLatency != 0 && other->latency >= maxLatency)
				late = true;
		}
		if (waiting == 0 || (waiting <= reorder && !late))
			break;
		_Bump();
	}
}
