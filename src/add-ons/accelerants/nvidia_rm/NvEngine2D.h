#pragma once

#include <mutex>
#include <vector>

#include <Accelerant.h>
#include <GraphicsDefs.h>
#include <Region.h>

#include <NvRmApi.h>
#include <NvRmDevice.h>


// The card's 2D engine (FERMI_TWOD_A), on a graphics channel of our own.
//
// It fills, inverts and copies rectangles of pitch linear surfaces: the
// frame buffer, and buffers in system memory that the GPU reads over the bus
// (snooped, so whatever the CPU last wrote is what it sees). Work is a list of
// methods in a push buffer that the channel fetches; every submission ends by
// releasing a semaphore with its sequence number, which is what waiting
// compares against.
//
// The channel is the pre-Volta kind, as for the video decoder: resman owns
// USERD, which is reached by mapping the channel, and there is no doorbell,
// so work is kicked by writing GPPut. Everything here throws when the card
// says no; once a submission has not completed in time the engine counts as
// lost and every call does nothing, so that callers fall back to the CPU.
class NvEngine2D {
public:
	// A surface the engine draws into or reads from, at its GPU address.
	struct Surface {
		NvU64 gpuAddress = 0;
		uint32 width = 0;
		uint32 height = 0;
		uint32 bytesPerRow = 0;
		color_space colorSpace = B_RGB32;

		bool IsSet() const {return gpuAddress != 0;}
	};

	// Memory the GPU can reach, mapped for both sides.
	class Buffer {
	public:
		Buffer() = default;
		Buffer(Buffer &&other) {*this = std::move(other);}
		Buffer &operator=(Buffer &&other);
		~Buffer() {Release();}

		Buffer(const Buffer &other) = delete;
		Buffer &operator=(const Buffer &other) = delete;

		void *Address() const {return fMapping.Address();}
		NvU64 GpuAddress() const {return fGpuAddress;}
		NvU64 Size() const {return fSize;}
		bool IsSet() const {return fGpuAddress != 0;}
		void Release();

	private:
		friend class NvEngine2D;
		NvRmApi *fRm = nullptr;
		NvHandle fDevice = 0;
		NvHandle fMemory = 0;		// what is mapped, ours or not
		NvRmObject fPhysical;		// set when the memory is ours
		NvRmObject fVirtual;
		NvRmMemoryMapping fMapping;
		NvU64 fGpuAddress = 0;
		NvU64 fSize = 0;
	};

	NvEngine2D(NvRmApi &rm, NvRmDevice &rmDev);
	~NvEngine2D();

	NvEngine2D(const NvEngine2D &other) = delete;
	NvEngine2D &operator=(const NvEngine2D &other) = delete;

	bool IsLost() const {return fLost;}
	// What the constructor was doing when it threw.
	const char *FailedStep() const;

	// Memory of our own. mapped leaves the CPU without a view of it.
	Buffer Allocate(NvU64 size, bool systemMemory, bool mapped = true);
	// Somebody else's memory object of this client, e.g. the frame buffer,
	// placed in the engine's address space; the returned buffer has no CPU
	// mapping and does not free the memory.
	Buffer Import(NvHandle memory, NvU64 size);

	// The surface the screen to screen operations work on.
	void SetTarget(const Surface &surface);
	const Surface &Target() const {return fTarget;}

	// The classic accelerant operations, on the target. Rectangles and spans
	// are inclusive, blit sizes one less than the real one, as the hooks have
	// them. Nothing is started until Submit().
	void FillRectangles(uint32 color, const fill_rect_params *list, uint32 count);
	void InvertRectangles(const fill_rect_params *list, uint32 count);
	void FillSpans(uint32 color, const uint16 *list, uint32 count);
	void Blit(const blit_params *list, uint32 count);
	void TransparentBlit(uint32 transparentColor, const blit_params *list,
		uint32 count);
	void ScaledFilteredBlit(const scaled_blit_params *list, uint32 count);

	// Copies rectangles of one surface to the same place in another (both
	// the same size), e.g. app_server's back buffer to the screen.
	void Copy(const Surface &from, const Surface &to,
		const clipping_rect *rects, uint32 count);

	// Hands everything queued since the last submission to the GPU; returns
	// the sequence number that says when it is done.
	uint32 Submit();
	uint32 LastSubmitted() const {return fSequence;}
	bool IsDone(uint32 sequence) const;
	// False when the work did not finish in time (the engine is lost then).
	bool Wait(uint32 sequence, bigtime_t timeout = 2000000);
	bool WaitIdle() {return Wait(Submit());}

	// Serializes callers; every public operation above expects it held.
	std::recursive_mutex &Lock() {return fLock;}

private:
	void Init();
	void Method(uint32 method, uint32 value);
	void Methods(uint32 method, const uint32 *values, uint32 count);
	void Reserve(uint32 words);
	void SetDestination(const Surface &surface);
	void SetSource(const Surface &surface, bool opaque = false);
	void SetOperation(uint32 operation);
	void BeginSolid(uint32 color);
	void Rect(int32 left, int32 top, int32 right, int32 bottom);
	void BlitWith(uint32 operation, const blit_params *list, uint32 count);
	void BlitRect(int32 dstX, int32 dstY, uint32 width, uint32 height,
		int32 srcX, int32 srcY);
	bool CheckLost();

	NvRmApi &fRm;
	NvRmDevice &fRmDev;
	std::recursive_mutex fLock;

	NvRmObject fVaSpace;
	Buffer fNotifier;
	Buffer fGpFifo;
	Buffer fPushBuffer;
	Buffer fSemaphore;
	NvRmObject fErrorContext;
	NvRmObject fChannel;
	NvRmMemoryMapping fUserdMapping;
	volatile uint32 *fUserd = nullptr;
	NvRmObject fTwoD;

	uint32 *fPush = nullptr;
	uint32 fPushStart = 0;		// words: where the open submission begins
	uint32 fPushAt = 0;			// and where the next method goes
	uint32 fGpPut = 0;
	uint32 fSequence = 0;
	uint32 fOperation = ~0u;
	bool fLost = false;
	const char *fStep = nullptr;

	Surface fTarget;
};
