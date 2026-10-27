#include <Accelerant.h>

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include "NvUtils.h"
#include "NvKmsBitmap.h"
#include "nv-haiku.h"

extern "C" {
#include "ctrl/ctrl2080/ctrl2080gpu.h" // NV2080_CTRL_CMD_GPU_GET_NAME_STRING
#include "ctrl/ctrl0000/ctrl0000client.h" // NV0000_CTRL_CMD_CLIENT_SHARE_OBJECT
#include "class/cl0073.h" // NV04_DISPLAY_COMMON
#include "ctrl/ctrl0073/ctrl0073system.h" // NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE
#include "ctrl/ctrl0073/ctrl0073dp.h" // NV0073_CTRL_CMD_DP_AUXCH_CTRL
#include "rs_access.h"
}

#include "common/edid.h"


static inline status_t ToErrorCode(const std::system_error &ex)
{
	if (ex.code().category() == std::generic_category()) {
		return ex.code().value();
	}
	return B_ERROR;
}


static uint32 CalcRefreshRate(const display_timing &haikuModeTimings)
{
	uint64 multiplier = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0 ? 2000000LL : 1000000LL;
	return (uint32)(multiplier * haikuModeTimings.pixel_clock / ((uint64)haikuModeTimings.h_total * haikuModeTimings.v_total));
}

static display_timing ToHaikuModeTimings(const NvModeTimings &nvKmsModeTimings) {
	display_timing haikuModeTimings {
		.pixel_clock  = nvKmsModeTimings.pixelClockHz / 1000,
		.h_display    = nvKmsModeTimings.hVisible,
		.h_sync_start = nvKmsModeTimings.hSyncStart,
		.h_sync_end   = nvKmsModeTimings.hSyncEnd,
		.h_total      = nvKmsModeTimings.hTotal,
		.v_display    = nvKmsModeTimings.vVisible,
		.v_sync_start = nvKmsModeTimings.vSyncStart,
		.v_sync_end   = nvKmsModeTimings.vSyncEnd,
		.v_total      = nvKmsModeTimings.vTotal,
		.flags        =
			(nvKmsModeTimings.interlaced ? B_TIMING_INTERLACED : 0) |
			(nvKmsModeTimings.hSyncPos ? B_POSITIVE_HSYNC : 0) |
			(nvKmsModeTimings.vSyncPos ? B_POSITIVE_VSYNC : 0),
	};
	return haikuModeTimings;
}

static NvModeTimings ToNvKmsModeTimings(const display_timing &haikuModeTimings) {
	NvModeTimings nvKmsModeTimings {
		.RRx1k        = CalcRefreshRate(haikuModeTimings),
		.pixelClockHz = haikuModeTimings.pixel_clock * 1000,
		.hVisible     = haikuModeTimings.h_display,
		.hSyncStart   = haikuModeTimings.h_sync_start,
		.hSyncEnd     = haikuModeTimings.h_sync_end,
		.hTotal       = haikuModeTimings.h_total,
		.vVisible     = haikuModeTimings.v_display,
		.vSyncStart   = haikuModeTimings.v_sync_start,
		.vSyncEnd     = haikuModeTimings.v_sync_end,
		.vTotal       = haikuModeTimings.v_total,
		.interlaced   = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0,
		.hSyncPos     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) != 0,
		.hSyncNeg     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) == 0,
		.vSyncPos     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) != 0,
		.vSyncNeg     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) == 0,
	};
	return nvKmsModeTimings;
}

static display_mode ToHaikuMode(const NvKmsMode &nvKmsMode) {
	display_mode haikuMode {
		.timing = ToHaikuModeTimings(nvKmsMode.timings),
		.space = B_RGB32,
		.virtual_width  = nvKmsMode.timings.hVisible,
		.virtual_height = nvKmsMode.timings.vVisible,
		// The frame buffer can be written while the GPU is drawing, which is
		// what lets a window be direct connected: its application then draws
		// into the screen itself.
		.flags = B_PARALLEL_ACCESS,
	};
	return haikuMode;
}

static NvKmsMode ToNvKmsMode(const display_mode &haikuMode) {
	NvKmsMode nvKmsMode {
		.timings = ToNvKmsModeTimings(haikuMode.timing),
	};
	return nvKmsMode;
}



class NvAccelerant {
private:
	static NvAccelerant *sInstance;

	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;
	NvKmsDispHandle fDisp;
	NVDpyIdList fValidDpys {};

	// One connector of the card, and the monitor on it.
	//
	// Every connected output has a region of the frame buffer that its head
	// scans out. The region is measured in frame buffer pixels; the display
	// engine scales it to the monitor's own resolution when the two differ.
	// The output's scale says how large the picture is on the monitor
	// relative to its logical size (150 percent: a 3840 pixel wide monitor is
	// 2560 logical pixels wide), and renderScale how many frame buffer pixels
	// app_server draws per logical pixel, also in percent. At renderScale
	// equal to the scale the region is the monitor's own size and nothing is
	// scaled at all, which is what keeps text crisp; at a smaller renderScale
	// the engine enlarges the region, at a larger one it shrinks it. Whether
	// it can shrink depends on the monitor: a 1080p monitor shown a region
	// drawn at twice its density is fine, a 4K one is beyond what the card
	// can fetch, so every layout is checked with NVKMS before it is agreed
	// to, and app_server tries the densest one first. Outputs can be placed
	// anywhere in the frame buffer - side by side, stacked, swapped - and the
	// frame buffer is the smallest rectangle that holds them all.
	struct Output {
		NVDpyId dpyId;
		uint32 id;					// what the rest of the system calls it
		NvU32 head;					// the head driving it while enabled
		NvU32 headMask;				// the heads able to drive it
		char name[NVKMS_DPY_NAME_SIZE];
		bool connected;
		bool forced;				// see IsForcedConnected()
		bool enabled;
		bool displayPort;
		NvKmsMode preferredMode;
		NvKmsMode mode;				// the timing the head is driven with
		int32 x, y;					// region in the frame buffer
		uint16 width, height;		// its size, in frame buffer pixels
		uint16 scale;				// percent
		uint16 renderScale;			// frame buffer pixels per logical pixel, percent
		bool mirror = false;		// shows the region of the output that starts
									// at the same place, enlarged by scale /
									// renderScale and centred on the monitor
		std::vector<uint8> edid;
	};
	std::vector<Output> fOutputs;

	// The primary output is the one the mode list describes, the one a
	// program asking for "the" monitor's EDID gets, and the head whose
	// blanks the retrace semaphore follows.
	NVDpyId fDpyId;
	NvU32 fHead;

	std::vector<NvKmsMode> fModeList;
	NvKmsMode fLayoutMode {};			// the frame buffer the layout needs
	NvKmsMode fCurrentMode {};
	display_mode fCurrentHaikuMode {};
	bool fLayoutApplied = false;		// the layout is what is on screen
	NvKmsDpyAttributeDpmsValue fDpmsState = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
	NvKmsBitmap fOldFramebuffer, fFramebuffer;
	NvKmsBitmap fCheckedFramebuffer;	// allocated to check the layout with,
										// shown when its mode is set

	NvKmsBitmap fCursor, fNewCursor;
	struct {
		int32 x;
		int32 y;
	} fCursorPos, fCursorHotSpot {};
	bool fCursorVisible = false;

	// last cursor image, the surface contents do not survive suspend
	struct {
		uint16 width = 0;
		uint16 height = 0;
		uint16 hotX = 0;
		uint16 hotY = 0;
		color_space colorSpace = B_RGBA32;
		uint16 bytesPerRow = 0;
		std::vector<uint8> data;
	} fCursorImage;

	// Where NVKMS writes the frame number at every vertical blank, and the
	// thread that turns that into Haiku's retrace semaphore.
	NvRmObject fVblankMemory;
	NvKmsSurface fVblankSurface;
	NvRmMemoryMapping fVblankMapping;
	NvKmsVblankSemControlHandle fVblankControl = 0;
	sem_id fRetraceSem = -1;
	thread_id fRetraceThread = -1;
	std::atomic<bool> fQuitRetraceThread {false};

	// Serializes NVKMS state changes between app_server and the resume thread.
	std::recursive_mutex fLock;
	thread_id fResumeThread = -1;
	std::atomic<bool> fQuitResumeThread {false};
	sem_id fDisplayRestoredSem = -1;

	// Watching for monitors being plugged in and pulled out.
	thread_id fHotplugThread = -1;
	std::atomic<bool> fQuitHotplugThread {false};
	NvRmObject fRmDisplay;
	NvU32 fRmDisplayMask = 0;
	port_id fChangePort = -1;
	int32 fChangeCode = 0;
	// A connector changed while the displays were asleep; guarded by fLock.
	bool fChangedWhileAsleep = false;
	// How many more times to check the links after waking; guarded by fLock.
	int32 fWakeChecksLeft = 0;
	// When to look at the connectors and links again after waking.
	std::atomic<bigtime_t> fRecheckAt {0};

	NvAccelerant(int devFd);

	void ApplyMode(const display_mode &mode, NvKmsBitmap &framebuffer);
	void FillLayoutRequest(const std::vector<Output> &outputs, NvKmsBitmap &framebuffer,
		NvKmsSetModeRequest &request);
	void ApplyLayout(NvKmsBitmap &framebuffer);
	bool CheckLayout(const std::vector<Output> &outputs, uint32 width, uint32 height);
	static status_t ResumeThreadEntry(void *arg);
	void ResumeThread();
	void RestoreAfterResume();

	void FindOutputs();
	bool QueryOutput(Output &output, NvKmsQueryDpyDynamicDataReply *dynamic = nullptr);
	bool AssignHead(Output &output, NvU32 &usedHeads);
	void ReadModeList();
	bool ValidateMode(const NvKmsMode &mode);
	NvKmsMode PreferredMode(NVDpyId dpyId);
	std::vector<NvKmsMode> ModesOf(NVDpyId dpyId);
	void ApplyDefaultLayout();
	void BuildLayoutMode();
	bool IsLayoutMode(const display_timing &timing) const;
	void SetLayoutMode(const display_mode &mode);
	void PublishScanout();
	Output *OutputByID(uint32 id);
	Output *PrimaryOutput();
	void FillDisplayOutput(const Output &output, display_output &info) const;

	void StartRetraceThread();
	void StopRetraceThread();
	void RefreshVblankReports();
	void EnableVblankReports();
	void DisableVblankReports();
	static status_t RetraceThreadEntry(void *arg);
	void RetraceThread();

	void StartHotplugThread();
	void StopHotplugThread();
	static status_t HotplugThreadEntry(void *arg);
	void HotplugThread();
	NvU32 ConnectedDisplayMask();
	bool RefreshOutputs();
	void NotifyDisplayChange();
	bool IsDisplayPort(NVDpyId dpyId);

	NvRmObject &RmDisplay();
	bool AccessDpcd(NvU32 displayId, NvU32 address, NvU8 *data, NvU32 size, bool write);
	bool LinkIsUp(const Output &output);
	void WakeSink(const Output &output);
	bool RetrainLinks(bool always);
	void WakeDisplays();
	void CheckLinksAfterWake();

public:
	sem_id RetraceSemaphore();

private:

	void UpdateCursor(bool updateImage, bool updatePos);

public:
	~NvAccelerant();

	static NvAccelerant *Instance() {return sInstance;}

	static void Init(int fd);
	static void Clone(void* data);
	void Uninit();

	ssize_t CloneInfoSize();
	void GetCloneInfo(void* data);
	void GetDeviceInfo(accelerant_device_info* adi);
	uint32 ModeCount();
	void GetModeList(display_mode* modes);
	status_t ProposeMode(display_mode *target, display_mode *low, display_mode *high);
	void SetDisplayMode(display_mode* modeToSet);
	void GetDisplayMode(display_mode* currentMode);
	void GetFrameBufferConfig(frame_buffer_config* frameBuffer);
	void GetPixelClockLimits(display_mode* dm, uint32* low /* in kHz */, uint32* high);

	uint32 DpmsCapabilities();
	uint32 DpmsMode();
	void SetDpmsMode(uint32 dpms_flags);
	void GetPreferredDisplayMode(display_mode* preferredMode);
	void GetMonitorInfo(monitor_info* info);
	void GetEdidInfo(void* info, uint32 size, uint32* _version);
	status_t WaitForDisplayRestore(bigtime_t timeout);

	uint32 DisplayOutputCount();
	void GetDisplayOutputs(display_output* outputs, uint32* count);
	void GetDisplayOutputModes(uint32 id, display_mode* modes, uint32* count);
	void SetDisplayLayout(const display_output_config* configs, uint32 count, display_mode* mode);
	void SetDisplayChangePort(port_id port, int32 code);

	void MoveCursor(uint16 x, uint16 y);
	void ShowCursor(bool isVisible);
	void SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask);
	void SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData);
};


NvAccelerant *NvAccelerant::sInstance {};


// The settings file, ~/config/settings/nvidia_rm_accelerant, is a testing
// aid. It can force a connector on ("force_connected DP-0", using the EDID of
// the first connected monitor, so that layouts can be tried without the
// monitors), and it can shape the layout the card comes up with before
// app_server has said anything ("scale DP-2 150", "position DP-4 2560 0").
// app_server's own layout, once it applies one, replaces all of that.
static bool ReadSetting(const char *key, const char *connector, char *value, size_t valueSize)
{
	FILE *file = fopen("/boot/home/config/settings/nvidia_rm_accelerant", "r");
	if (file == NULL)
		return false;
	bool found = false;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char lineKey[64], lineConnector[128], rest[128] = "";
		if (sscanf(line, "%63s %127s %127[^\n]", lineKey, lineConnector, rest) < 2)
			continue;
		if (strcmp(lineKey, key) == 0 && strcmp(lineConnector, connector) == 0) {
			strlcpy(value, rest, valueSize);
			found = true;
		}
	}
	fclose(file);
	return found;
}

static bool IsForcedConnected(const char *name)
{
	char value[128];
	return ReadSetting("force_connected", name, value, sizeof(value));
}

// NVKMS names a connector with a monitor on it after the monitor, "DELL
// P2415Q (DP-2)"; the rest of the system wants the connector, "DP-2".
static void ConnectorName(const char *nvKmsName, char *connector, size_t size)
{
	const char *open = strrchr(nvKmsName, '(');
	const char *close = open != NULL ? strchr(open, ')') : NULL;
	if (open != NULL && close != NULL && close > open + 1) {
		size_t length = std::min((size_t)(close - open - 1), size - 1);
		memcpy(connector, open + 1, length);
		connector[length] = '\0';
	} else
		strlcpy(connector, nvKmsName, size);
}

static uint16 LogicalSize(uint32 pixels, uint16 scale)
{
	return (uint16)((pixels * 100 + scale / 2) / scale);
}

// The frame buffer region of an output: its logical size drawn at renderScale
// percent. At the output's own scale that is the monitor's size exactly.
static uint16 RegionSize(uint32 pixels, uint16 scale, uint16 renderScale)
{
	if (renderScale == scale)
		return pixels;
	uint32 logical = LogicalSize(pixels, scale);
	return (uint16)((logical * renderScale + 50) / 100);
}


NvAccelerant::NvAccelerant(int devFd):
	fDevFd(dup(devFd)),
	fRmDev(fRm, 0), // TODO: do not hardcode rmDeviceId
	fKmsDev(fKms, 0)
{
	if (!(fKmsDev.Info().numDisps > 0)) {
		RaiseErrno(ENODEV);
	}
	fDisp = fKmsDev.Info().dispHandles[0];

	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params)));

		fValidDpys = params.reply.validDpys;
	}

	// A monitor takes a moment to answer after the card comes up.
	const int32 totalAttempts = 10;
	for (int32 i = 0; i < totalAttempts; i++) {
		FindOutputs();
		if (!fOutputs.empty())
			break;
		debug_printf("nvidia_rm: [%" B_PRId32 "/%" B_PRId32 "]: no connected displays\n", i, totalAttempts);
		snooze(100000);
	}
	if (fOutputs.empty()) {
		RaiseErrno(ENODEV);
	}

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		CheckErrno(fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params)));
	}

	ApplyDefaultLayout();
	ReadModeList();

	fDisplayRestoredSem = create_sem(0, "nvidia_rm display restored");
	fResumeThread = spawn_thread(ResumeThreadEntry, "nvidia_rm resume",
		B_DISPLAY_PRIORITY, this);
	if (fResumeThread >= 0)
		resume_thread(fResumeThread);

	StartHotplugThread();
}

NvAccelerant::~NvAccelerant()
{
	StopHotplugThread();

	if (fResumeThread >= 0) {
		fQuitResumeThread = true;
		status_t result;
		wait_for_thread(fResumeThread, &result);
	}
	if (fDisplayRestoredSem >= 0)
		delete_sem(fDisplayRestoredSem);

	StopRetraceThread();
	if (fRetraceSem >= 0)
		delete_sem(fRetraceSem);
}

status_t NvAccelerant::WaitForDisplayRestore(bigtime_t timeout)
{
	status_t status = acquire_sem_etc(fDisplayRestoredSem, 1, B_RELATIVE_TIMEOUT,
		timeout);
	return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;
}

status_t NvAccelerant::ResumeThreadEntry(void *arg)
{
	static_cast<NvAccelerant*>(arg)->ResumeThread();
	return B_OK;
}

// The display hardware state is lost while the system is suspended. The
// kernel driver bumps a generation counter after resuming, restore the mode
// and cursor when it changes.
void NvAccelerant::ResumeThread()
{
	nv_haiku_resume_params params {};
	if (ioctl(fDevFd.Get(), NV_HAIKU_BASE + NV_HAIKU_WAIT_FOR_RESUME, &params,
			sizeof(params)) < 0) {
		debug_printf("nvidia_rm: resume notifications not supported\n");
		return;
	}
	uint32 generation = params.generation;

	while (!fQuitResumeThread) {
		params.generation = generation;
		params.timeout = 500000;
		if (ioctl(fDevFd.Get(), NV_HAIKU_BASE + NV_HAIKU_WAIT_FOR_RESUME,
				&params, sizeof(params)) < 0) {
			snooze(500000);
			continue;
		}
		if (params.generation == generation)
			continue;

		generation = params.generation;
		debug_printf("nvidia_rm: restoring display state after resume\n");
		try {
			RestoreAfterResume();
			release_sem(fDisplayRestoredSem);
		} catch (const std::system_error &ex) {
			debug_printf("[!] nvidia_rm: restoring after resume failed: %s\n",
				ex.what());
		}
	}
}

void NvAccelerant::RestoreAfterResume()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (fCurrentMode.timings.hVisible == 0 || !fFramebuffer.IsSet())
		return;

	if (fLayoutApplied)
		ApplyLayout(fFramebuffer);
	else
		ApplyMode(fCurrentHaikuMode, fFramebuffer);

	// The display was off; whatever was reporting its blanks is not any more.
	RefreshVblankReports();

	if (!fCursorImage.data.empty()) {
		SetCursorBitmap(fCursorImage.width, fCursorImage.height,
			fCursorImage.hotX, fCursorImage.hotY, fCursorImage.colorSpace,
			fCursorImage.bytesPerRow, fCursorImage.data.data());
	}
	if (fCursorVisible)
		UpdateCursor(true, true);

	if (fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON)
		SetDpmsMode(B_DPMS_OFF);
}


// Ask NVKMS about one connector. Returns whether a monitor is on it (or is
// being pretended to be, see the settings file). The reply is handed back to
// callers that want more of it than the summary kept in the Output.
bool NvAccelerant::QueryOutput(Output &output, NvKmsQueryDpyDynamicDataReply *dynamic)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.dpyId = output.dpyId;
	CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));

	ConnectorName(params.reply.name, output.name, sizeof(output.name));
	output.connected = params.reply.connected || params.reply.edid.valid;
	if (params.reply.edid.valid && params.reply.edid.bufferSize > 0) {
		output.edid.assign(params.reply.edid.buffer,
			params.reply.edid.buffer + params.reply.edid.bufferSize);
	} else
		output.edid.clear();

	if (dynamic != nullptr)
		*dynamic = params.reply;
	return output.connected;
}

bool NvAccelerant::AssignHead(Output &output, NvU32 &usedHeads)
{
	NvU32 freeHeads = output.headMask & ~usedHeads;
	if (freeHeads == 0) {
		debug_printf("nvidia_rm: no free head for display %s\n", output.name);
		return false;
	}
	output.head = __builtin_ctz(freeHeads);
	usedHeads |= 1U << output.head;
	return true;
}

// Find every monitor on the card. Connectors are listed in NVKMS's order,
// which puts the connector the firmware used first; that becomes the primary
// output and the left end of the default layout.
void NvAccelerant::FindOutputs()
{
	std::vector<Output> outputs;
	NvKmsQueryDpyDynamicDataReply firstConnected {};
	bool haveFirstConnected = false;

	for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), fValidDpys);
			!nvDpyIdIsInvalid(dpyId);
			dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, fValidDpys)) {
		Output output {};
		output.dpyId = dpyId;
		output.id = nvDpyIdToNvU32(dpyId);
		output.scale = 100;
		output.renderScale = 100;

		NvKmsQueryDpyDynamicDataReply dynamic;
		bool connected = QueryOutput(output, &dynamic);
		debug_printf("nvidia_rm: connector %s%s%s%s\n", output.name,
			connected ? " (connected" : "", connected ? dynamic.name : "",
			connected ? ")" : "");

		if (!connected && haveFirstConnected && IsForcedConnected(output.name)) {
			NvKmsQueryDpyDynamicDataParams forceParams {};
			forceParams.request.deviceHandle = fKmsDev.Get();
			forceParams.request.dispHandle = fDisp;
			forceParams.request.dpyId = dpyId;
			forceParams.request.forceConnected = true;
			forceParams.request.overrideEdid = true;
			forceParams.request.edid.bufferSize = firstConnected.edid.bufferSize;
			memcpy(forceParams.request.edid.buffer, firstConnected.edid.buffer,
				sizeof(forceParams.request.edid.buffer));
			CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &forceParams, sizeof(forceParams)));
			debug_printf("nvidia_rm: forcing %s connected: %d\n", output.name,
				forceParams.reply.connected);
			if (forceParams.reply.connected) {
				connected = QueryOutput(output);
				output.forced = true;
			}
		}
		if (!connected)
			continue;
		if (!haveFirstConnected) {
			firstConnected = dynamic;
			haveFirstConnected = true;
		}

		NvKmsQueryDpyStaticDataParams staticParams {};
		staticParams.request.deviceHandle = fKmsDev.Get();
		staticParams.request.dispHandle = fDisp;
		staticParams.request.dpyId = dpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &staticParams, sizeof(staticParams)));
		output.headMask = staticParams.reply.headMask;
		output.displayPort = IsDisplayPort(dpyId);

		output.preferredMode = PreferredMode(dpyId);
		output.mode = output.preferredMode;
		if (output.mode.timings.hVisible == 0) {
			debug_printf("nvidia_rm: display %s has no usable mode\n", output.name);
			continue;
		}
		outputs.push_back(output);
	}

	fOutputs = outputs;
	if (fOutputs.empty())
		return;
	fDpyId = fOutputs[0].dpyId;
}

// The layout the card comes up with: every monitor at its own resolution,
// side by side from left to right, one frame buffer pixel per monitor pixel -
// unless the settings file says otherwise. app_server replaces this with the
// user's layout as soon as it starts.
void NvAccelerant::ApplyDefaultLayout()
{
	NvU32 usedHeads = 0;
	int32 x = 0;
	for (auto &output: fOutputs) {
		output.enabled = AssignHead(output, usedHeads);
		if (!output.enabled)
			continue;

		char value[128];
		if (ReadSetting("scale", output.name, value, sizeof(value))) {
			int scale = atoi(value);
			if (scale >= 100 && scale <= 400)
				output.scale = scale;
		}
		output.width = LogicalSize(output.mode.timings.hVisible, output.scale);
		output.height = LogicalSize(output.mode.timings.vVisible, output.scale);
		output.x = x;
		output.y = 0;
		if (ReadSetting("position", output.name, value, sizeof(value))) {
			int px, py;
			if (sscanf(value, "%d %d", &px, &py) == 2) {
				output.x = px;
				output.y = py;
			}
		}
		x = output.x + output.width;

		debug_printf("nvidia_rm: display %s on head %" B_PRIu32 ", %" B_PRIu32 "x%" B_PRIu32
			" at %" B_PRId32 ",%" B_PRId32 " scale %u%%\n",
			output.name, output.head, (uint32)output.mode.timings.hVisible,
			(uint32)output.mode.timings.vVisible, output.x, output.y, output.scale);
	}
	BuildLayoutMode();
}

NvKmsMode NvAccelerant::PreferredMode(NVDpyId dpyId)
{
	NvKmsMode firstMode {};
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;
		if (!params.reply.valid)
			continue;
		if (params.reply.preferredMode)
			return params.reply.mode;
		if (firstMode.timings.hVisible == 0)
			firstMode = params.reply.mode;
	}
	return firstMode;
}

// Every timing a monitor accepts, as NVKMS validated them.
std::vector<NvKmsMode> NvAccelerant::ModesOf(NVDpyId dpyId)
{
	std::vector<NvKmsMode> modes;
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {
			.request = {
				.deviceHandle = fKmsDev.Get(),
				.dispHandle = fDisp,
				.dpyId = dpyId,
				.modeIndex = i,
			},
		};
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;
		if (!params.reply.valid)
			continue;
		modes.push_back(params.reply.mode);
	}
	return modes;
}

// The mode list the classic interface sees: the primary monitor's modes.
void NvAccelerant::ReadModeList()
{
	fModeList = ModesOf(fDpyId);
}

// The frame buffer is the smallest rectangle around every enabled output's
// region. Its timings are synthetic: the primary monitor's blanking around the
// whole thing. They only describe the frame buffer to app_server; every head
// is programmed with its own monitor's timings.
void NvAccelerant::BuildLayoutMode()
{
	int32 minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
	const Output *primary = nullptr;
	for (const auto &output: fOutputs) {
		if (!output.enabled)
			continue;
		if (primary == nullptr || (primary->mirror && !output.mirror))
			primary = &output;
		minX = std::min(minX, output.x);
		minY = std::min(minY, output.y);
		maxX = std::max(maxX, output.x + (int32)output.width);
		maxY = std::max(maxY, output.y + (int32)output.height);
	}
	if (primary == nullptr) {
		fLayoutMode = {};
		return;
	}
	// Regions are kept at the frame buffer's origin.
	for (auto &output: fOutputs) {
		if (!output.enabled)
			continue;
		output.x -= minX;
		output.y -= minY;
	}
	fDpyId = primary->dpyId;
	fHead = primary->head;

	const NvModeTimings &p = primary->mode.timings;
	NvU32 width = maxX - minX;
	NvU32 height = maxY - minY;

	fLayoutMode = primary->mode;
	NvModeTimings &t = fLayoutMode.timings;
	NvU32 extraH = width > p.hVisible ? width - p.hVisible : 0;
	NvU32 extraV = height > p.vVisible ? height - p.vVisible : 0;
	NvU32 lessH = width < p.hVisible ? p.hVisible - width : 0;
	NvU32 lessV = height < p.vVisible ? p.vVisible - height : 0;
	t.hVisible = width;
	t.hSyncStart = t.hSyncStart + extraH - lessH;
	t.hSyncEnd = t.hSyncEnd + extraH - lessH;
	t.hTotal = t.hTotal + extraH - lessH;
	t.vVisible = height;
	t.vSyncStart = t.vSyncStart + extraV - lessV;
	t.vSyncEnd = t.vSyncEnd + extraV - lessV;
	t.vTotal = t.vTotal + extraV - lessV;
	t.pixelClockHz = (NvU32)((uint64)p.RRx1k * t.hTotal * t.vTotal / 1000);
}

bool NvAccelerant::IsLayoutMode(const display_timing &timing) const
{
	return fLayoutMode.timings.hVisible != 0
		&& timing.h_display == fLayoutMode.timings.hVisible
		&& timing.v_display == fLayoutMode.timings.vVisible;
}

NvAccelerant::Output *NvAccelerant::OutputByID(uint32 id)
{
	for (auto &output: fOutputs) {
		if (output.id == id)
			return &output;
	}
	return nullptr;
}

NvAccelerant::Output *NvAccelerant::PrimaryOutput()
{
	for (auto &output: fOutputs) {
		if (nvDpyIdsAreEqual(output.dpyId, fDpyId))
			return &output;
	}
	return fOutputs.empty() ? nullptr : &fOutputs[0];
}

bool NvAccelerant::ValidateMode(const NvKmsMode &mode)
{
	NvKmsValidateModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.dpyId = fDpyId;
	params.request.mode = mode;
	CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
	return params.reply.valid;
}

void NvAccelerant::Init(int fd)
{
	debug_printf("NvAccelerant::Init\n");

	sInstance = new NvAccelerant(fd);
}

static const char kCloneInfo[] = "/dev/graphics/nvidia0"; // FIXME

ssize_t NvAccelerant::CloneInfoSize()
{
	debug_printf("NvAccelerant::CloneInfoSize\n");

	return strlen(kCloneInfo) + 1;
}

void NvAccelerant::GetCloneInfo(void* data)
{
	debug_printf("NvAccelerant::GetCloneInfo\n");

	strcpy((char*)data, kCloneInfo);
}

void NvAccelerant::Clone(void* data)
{
	debug_printf("NvAccelerant::Clone\n");

	FileDesc devFd(open((char*)data, O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	sInstance = new NvAccelerant(devFd.Get());
}

void NvAccelerant::Uninit()
{
	debug_printf("NvAccelerant::Uninit\n");

	delete sInstance;
	sInstance = nullptr;
}

void NvAccelerant::GetDeviceInfo(accelerant_device_info* adi)
{
	debug_printf("NvAccelerant::GetDeviceInfo\n");

	NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS getNameParams = {
		.gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
	};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &getNameParams, sizeof(getNameParams));

	NV2080_CTRL_GPU_GET_SHORT_NAME_STRING_PARAMS getShortNameParams {};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, &getShortNameParams, sizeof(getShortNameParams));

	adi->version = B_ACCELERANT_VERSION;
	strlcpy(adi->name, (char*)getNameParams.gpuNameString.ascii, sizeof(adi->name));
	strlcpy(adi->chipset, (char*)getShortNameParams.gpuShortNameString, sizeof(adi->chipset));
	strcpy(adi->serial_no, "?");
	adi->memory = 0x20000000; // FIXME
	adi->dac_speed = 0;
}


// The classic mode list: the layout's frame buffer first, so that app_server
// picks it up as the preferred mode, then the primary monitor's own timings
// for programs that set a mode the old way (which drives the primary monitor
// alone).
uint32 NvAccelerant::ModeCount()
{
	debug_printf("NvAccelerant::ModeCount\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);
	return fModeList.size() + (fLayoutMode.timings.hVisible != 0 ? 1 : 0);
}

void NvAccelerant::GetModeList(display_mode* mode)
{
	debug_printf("NvAccelerant::GetModeList\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);
	int32 i = 0;
	if (fLayoutMode.timings.hVisible != 0)
		mode[i++] = ToHaikuMode(fLayoutMode);
	for (const auto &nvKmsMode: fModeList) {
		mode[i++] = ToHaikuMode(nvKmsMode);
	}
}

static bool ModeTimingsEqual(const display_timing &a, const display_timing &b)
{
	return
		a.pixel_clock  == b.pixel_clock &&
		a.h_display    == b.h_display &&
		a.h_sync_start == b.h_sync_start &&
		a.h_sync_end   == b.h_sync_end &&
		a.h_total      == b.h_total &&
		a.v_display    == b.v_display &&
		a.v_sync_start == b.v_sync_start &&
		a.v_sync_end   == b.v_sync_end &&
		a.v_total      == b.v_total &&
		a.flags        == b.flags;
}

bool IsDisplayModeWithinBounds(display_mode &mode, const display_mode &low, const display_mode &high)
{
	if (mode.timing.h_display < low.timing.h_display
		|| mode.timing.h_display > high.timing.h_display
		|| mode.timing.h_sync_start < low.timing.h_sync_start
		|| mode.timing.h_sync_start > high.timing.h_sync_start
		|| mode.timing.h_sync_end < low.timing.h_sync_end
		|| mode.timing.h_sync_end > high.timing.h_sync_end
		|| mode.timing.h_total < low.timing.h_total
		|| mode.timing.h_total > high.timing.h_total)
		return false;

	if (mode.timing.v_display < low.timing.v_display
		|| mode.timing.v_display > high.timing.v_display
		|| mode.timing.v_sync_start < low.timing.v_sync_start
		|| mode.timing.v_sync_start > high.timing.v_sync_start
		|| mode.timing.v_sync_end < low.timing.v_sync_end
		|| mode.timing.v_sync_end > high.timing.v_sync_end
		|| mode.timing.v_total < low.timing.v_total
		|| mode.timing.v_total > high.timing.v_total)
		return false;

	if (mode.timing.pixel_clock > high.timing.pixel_clock
		|| mode.timing.pixel_clock < low.timing.pixel_clock)
		return false;

	if (mode.virtual_width > high.virtual_width
		|| mode.virtual_width < low.virtual_width)
		return false;

	if (mode.virtual_height > high.virtual_height
		|| mode.virtual_height < low.virtual_height)
		return false;

	return true;
}

status_t NvAccelerant::ProposeMode(display_mode *target, display_mode *low, display_mode *high)
{
	debug_printf("NvAccelerant::ProposeMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (IsLayoutMode(target->timing)) {
		target->timing = ToHaikuModeTimings(fLayoutMode.timings);
		target->virtual_width = target->timing.h_display;
		target->virtual_height = target->timing.v_display;
		return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
	}

	for (size_t i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto mode = ToHaikuMode(nvKmsMode);
		if (ModeTimingsEqual(target->timing, mode.timing)) {
			return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
		}
	}

	uint32 reqRefreshRate = CalcRefreshRate(target->timing);
	int32 bestCandidateIdx = -1;
	for (size_t i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timings = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timings.h_display == target->timing.h_display &&
			timings.v_display == target->timing.v_display &&
			(timings.flags & B_TIMING_INTERLACED) == (target->timing.flags & B_TIMING_INTERLACED)
		) {
			if (bestCandidateIdx < 0 || std::abs((int64)nvKmsMode.timings.RRx1k - (int64)reqRefreshRate) < std::abs((int64)fModeList[bestCandidateIdx].timings.RRx1k - (int64)reqRefreshRate)) {
				bestCandidateIdx = i;
			}
		}
	}
	if (bestCandidateIdx < 0) {
		return B_ERROR;
	}

	target->timing = ToHaikuModeTimings(fModeList[bestCandidateIdx].timings);

	return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
}

// The classic single-monitor mode set: the primary monitor at that timing,
// with the whole frame buffer, and every other head off.
void NvAccelerant::ApplyMode(const display_mode &mode, NvKmsBitmap &framebuffer)
{
	Output *primary = PrimaryOutput();
	if (primary == nullptr)
		RaiseErrno(ENODEV);
	NvU32 head = primary->head;

	NvKmsSetModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.commit = true;
	params.request.requestedDispsBitMask |= 1U << 0;
	params.request.disp[0].requestedHeadsBitMask |= 1U << head;
	params.request.disp[0].head[head].dpyIdList = nvAddDpyIdToEmptyDpyIdList(fDpyId);
	params.request.disp[0].head[head].mode = ToNvKmsMode(mode);
	params.request.disp[0].head[head].modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
	params.request.disp[0].head[head].viewPortOut = {.x = 0, .y = 0, .width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[head].viewPortSizeIn = {.width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].surface.handle[0] = framebuffer.Surface().Get();
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].surface.specified = true;
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].sizeIn.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].sizeIn.specified = true;
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].sizeOut.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[head].flip.layer[NVKMS_MAIN_LAYER].sizeOut.specified = true;

	// turn off every other head
	params.request.disp[0].requestedHeadsBitMask
		|= (1U << std::min<NvU32>(fKmsDev.Info().numHeads, NVKMS_MAX_HEADS_PER_DISP)) - 1;

	try {
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)));
	} catch (const std::system_error&) {
		debug_printf("[!] NvAccelerant: SetMode failed\n");
		debug_printf("  status: %d\n", params.reply.status);
		debug_printf("  disp[0].status: %d\n", params.reply.disp[0].status);
		debug_printf("  disp[0].head[%" B_PRIu32 "].status: %d\n", head, params.reply.disp[0].head[head].status);
		throw;
	}
	fHead = head;
}

void NvAccelerant::SetDisplayMode(display_mode* modeToSet)
{
	debug_printf("NvAccelerant::SetDisplayMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (IsLayoutMode(modeToSet->timing)) {
		SetLayoutMode(*modeToSet);
		return;
	}
	fCheckedFramebuffer = NvKmsBitmap();

	if (
		modeToSet->virtual_width != modeToSet->timing.h_display ||
		modeToSet->virtual_height != modeToSet->timing.v_display ||
		modeToSet->h_display_start != 0 ||
		modeToSet->v_display_start != 0
	) {
		RaiseErrno(EINVAL);
	}

	{
		char infoString[NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH] {};
		NvKmsValidateModeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.modeValidation.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		params.request.mode = ToNvKmsMode(*modeToSet);
		params.request.infoStringSize = NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH;
		params.request.pInfoString = nvKmsPointerToNvU64(infoString);
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
		if (!params.reply.valid) {
			debug_printf("[!] mode validation failed\n");
			debug_printf("%s", infoString);
			RaiseErrno(EINVAL);
		}
	}

	fOldFramebuffer = NvKmsBitmap();
	NvKmsBitmap newFramebuffer(fRmDev, fKmsDev, modeToSet->virtual_width,
		modeToSet->virtual_height, (color_space)modeToSet->space);

	ApplyMode(*modeToSet, newFramebuffer);

	fCurrentMode = ToNvKmsMode(*modeToSet);
	fCurrentHaikuMode = *modeToSet;
	fLayoutApplied = false;

	fOldFramebuffer = std::move(fFramebuffer);
	fFramebuffer = std::move(newFramebuffer);

	PublishScanout();

	RefreshVblankReports();

	if (fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON)
		SetDpmsMode(B_DPMS_OFF);
}

// The mode set for a layout: every enabled output with its own timings, its
// region of the frame buffer, and the display engine's scaler between the two
// when the region and the monitor differ in size.
void NvAccelerant::FillLayoutRequest(const std::vector<Output> &outputs,
	NvKmsBitmap &framebuffer, NvKmsSetModeRequest &request)
{
	request.deviceHandle = fKmsDev.Get();
	request.requestedDispsBitMask |= 1U << 0;
	// Every head is part of the request; one that gets no display below is
	// turned off, which is what a monitor that was unplugged or disabled
	// needs.
	request.disp[0].requestedHeadsBitMask
		= (1U << std::min<NvU32>(fKmsDev.Info().numHeads, NVKMS_MAX_HEADS_PER_DISP)) - 1;
	for (const auto &output: outputs) {
		if (!output.enabled)
			continue;
		const NvModeTimings &timings = output.mode.timings;
		NvKmsSetModeOneHeadRequest &head = request.disp[0].head[output.head];
		request.disp[0].requestedHeadsBitMask |= 1U << output.head;
		head.dpyIdList = nvAddDpyIdToEmptyDpyIdList(output.dpyId);
		head.mode = output.mode;
		head.modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		// viewPortOut is the whole raster; viewPortSizeIn is how much of the
		// frame buffer is scaled to it.
		head.viewPortOut = {.x = 0, .y = 0, .width = timings.hVisible, .height = timings.vVisible};
		head.viewPortSizeIn = {.width = output.width, .height = output.height};
		if (output.mirror) {
			// A mirror of another shape gets its source's picture as large as
			// it fits, with black bars on the sides that are left over.
			uint32 width = ((uint32)output.width * output.scale + output.renderScale / 2)
				/ output.renderScale;
			uint32 height = ((uint32)output.height * output.scale + output.renderScale / 2)
				/ output.renderScale;
			width = std::min<uint32>(width, timings.hVisible);
			height = std::min<uint32>(height, timings.vVisible);
			if (width != timings.hVisible || height != timings.vVisible) {
				head.viewPortOut = {.x = (NvU16)((timings.hVisible - width) / 2),
					.y = (NvU16)((timings.vVisible - height) / 2),
					.width = (NvU16)width, .height = (NvU16)height};
				head.viewPortOutSpecified = true;
			}
		}
		head.flip.viewPortIn.specified = true;
		head.flip.viewPortIn.point = {.x = (NvU16)output.x, .y = (NvU16)output.y};
		auto &layer = head.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = framebuffer.Surface().Get();
		layer.surface.specified = true;
		// The layer covers the whole desktop; viewPortIn selects this head's part.
		layer.sizeIn.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
		layer.sizeOut.specified = true;
	}
}

// Program the current layout.
void NvAccelerant::ApplyLayout(NvKmsBitmap &framebuffer)
{
	NvKmsSetModeParams params {};
	FillLayoutRequest(fOutputs, framebuffer, params.request);
	params.request.commit = true;

	try {
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)));
	} catch (const std::system_error&) {
		debug_printf("[!] NvAccelerant: layout SetMode failed, status %d\n", params.reply.status);
		for (const auto &output: fOutputs) {
			if (!output.enabled)
				continue;
			debug_printf("  head %" B_PRIu32 " (%s, %ux%u at %" B_PRId32 ",%" B_PRId32 " -> %ux%u): status %d\n",
				output.head, output.name, output.width, output.height, output.x, output.y,
				(unsigned)output.mode.timings.hVisible, (unsigned)output.mode.timings.vVisible,
				params.reply.disp[0].head[output.head].status);
		}
		throw;
	}
}

// Ask NVKMS whether a layout can be shown, without showing it. The frame
// buffer it is checked with is the one the layout will need, kept for when
// its mode is set. What the display engine can do depends on more than the
// scale - shrinking a region onto a 1080p monitor works where shrinking
// onto a 4K one exceeds what the card can fetch - and only NVKMS knows.
bool NvAccelerant::CheckLayout(const std::vector<Output> &outputs, uint32 width,
	uint32 height)
{
	// The frame buffer from before the current one has been off screen for
	// a whole mode set; its video memory can go now.
	fOldFramebuffer = NvKmsBitmap();
	fCheckedFramebuffer = NvKmsBitmap();
	NvKmsBitmap framebuffer(fRmDev, fKmsDev, width, height, B_RGB32, false, false);

	NvKmsSetModeParams params {};
	FillLayoutRequest(outputs, framebuffer, params.request);
	params.request.commit = false;
	if (fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)) < 0) {
		debug_printf("nvidia_rm: a %" B_PRIu32 "x%" B_PRIu32 " layout is beyond the display "
			"engine (status %d, disp status %d)\n", width, height, params.reply.status,
			params.reply.disp[0].status);
		for (const auto &output: outputs) {
			if (!output.enabled)
				continue;
			debug_printf("nvidia_rm:   %s: %ux%u of the frame buffer onto %ux%u, status %d\n",
				output.name, output.width, output.height,
				(unsigned)output.mode.timings.hVisible, (unsigned)output.mode.timings.vVisible,
				params.reply.disp[0].head[output.head].status);
		}
		return false;
	}

	fCheckedFramebuffer = std::move(framebuffer);
	return true;
}

void NvAccelerant::SetLayoutMode(const display_mode &mode)
{
	NvKmsBitmap newFramebuffer;
	if (fCheckedFramebuffer.IsSet()
		&& fCheckedFramebuffer.Width() == fLayoutMode.timings.hVisible
		&& fCheckedFramebuffer.Height() == fLayoutMode.timings.vVisible
		&& fCheckedFramebuffer.ColorSpace() == (color_space)mode.space) {
		newFramebuffer = std::move(fCheckedFramebuffer);
	} else {
		fCheckedFramebuffer = NvKmsBitmap();
		fOldFramebuffer = NvKmsBitmap();
		newFramebuffer = NvKmsBitmap(fRmDev, fKmsDev, fLayoutMode.timings.hVisible,
			fLayoutMode.timings.vVisible, (color_space)mode.space, false, false);
	}

	// Only 256 MiB of video memory can be mapped for the CPU at a time, and
	// two frame buffers of three monitors drawn at 200 percent are 200 MiB:
	// the new one is mapped in place of the current one, which nothing
	// draws into while app_server is setting the mode. Should anything fail,
	// the current one is mapped again and stays on screen; app_server reads
	// its new address.
	fFramebuffer.Unmap();
	try {
		newFramebuffer.Map();
		ApplyLayout(newFramebuffer);
	} catch (...) {
		newFramebuffer.Unmap();
		fFramebuffer.Map();
		throw;
	}

	fCurrentMode = fLayoutMode;
	fCurrentHaikuMode = mode;
	fCurrentHaikuMode.timing = ToHaikuModeTimings(fLayoutMode.timings);
	fCurrentHaikuMode.virtual_width = fLayoutMode.timings.hVisible;
	fCurrentHaikuMode.virtual_height = fLayoutMode.timings.vVisible;
	fCurrentHaikuMode.h_display_start = 0;
	fCurrentHaikuMode.v_display_start = 0;
	fLayoutApplied = true;

	fOldFramebuffer = std::move(fFramebuffer);
	fFramebuffer = std::move(newFramebuffer);

	PublishScanout();

	RefreshVblankReports();

	// A mode set trains every link and wakes every monitor; if they were
	// meant to be asleep, put them back.
	if (fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON)
		SetDpmsMode(B_DPMS_OFF);
}

void NvAccelerant::GetDisplayMode(display_mode* currentMode)
{
	debug_printf("NvAccelerant::GetDisplayMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fCurrentMode.timings.hVisible == 0) {
		RaiseErrno(ENOENT);
	}
	*currentMode = fCurrentHaikuMode;
	currentMode->flags |= B_PARALLEL_ACCESS;
}


// #pragma mark - display outputs


void NvAccelerant::FillDisplayOutput(const Output &output, display_output &info) const
{
	info = {};
	info.version = B_DISPLAY_OUTPUT_VERSION;
	info.id = output.id;
	strlcpy(info.name, output.name, sizeof(info.name));
	info.flags = B_DISPLAY_OUTPUT_SCALABLE;
	if (output.connected)
		info.flags |= B_DISPLAY_OUTPUT_CONNECTED;
	if (output.enabled)
		info.flags |= B_DISPLAY_OUTPUT_ENABLED;
	if (output.enabled && output.mirror)
		info.flags |= B_DISPLAY_OUTPUT_MIRROR;
	info.x = output.x;
	info.y = output.y;
	info.width = output.width;
	info.height = output.height;
	info.scale = output.scale;
	info.render_scale = output.renderScale;
	info.native_timing = ToHaikuModeTimings(output.preferredMode.timings);
	info.timing = ToHaikuModeTimings(output.mode.timings);
	info.edid_length = std::min(output.edid.size(), sizeof(info.edid));
	memcpy(info.edid, output.edid.data(), info.edid_length);
}

uint32 NvAccelerant::DisplayOutputCount()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	return fOutputs.size();
}

void NvAccelerant::GetDisplayOutputs(display_output* outputs, uint32* count)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	uint32 i = 0;
	for (const auto &output: fOutputs) {
		if (i >= *count)
			break;
		FillDisplayOutput(output, outputs[i++]);
	}
	*count = i;
}

void NvAccelerant::GetDisplayOutputModes(uint32 id, display_mode* modes, uint32* count)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Output *output = OutputByID(id);
	if (output == nullptr)
		RaiseErrno(ENOENT);
	std::vector<NvKmsMode> list = ModesOf(output->dpyId);
	uint32 i = 0;
	for (const auto &mode: list) {
		if (i >= *count)
			break;
		modes[i++] = ToHaikuMode(mode);
	}
	*count = i;
}

// Take the layout app_server wants. Nothing changes on screen here; it does
// when the returned mode is set.
void NvAccelerant::SetDisplayLayout(const display_output_config* configs, uint32 count, display_mode* mode)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);

	// Work on a copy so that a refused layout leaves the current one alone.
	std::vector<Output> outputs = fOutputs;
	for (auto &output: outputs) {
		output.enabled = false;
		output.mirror = false;
	}

	NvU32 usedHeads = 0;
	uint32 enabled = 0;
	for (uint32 i = 0; i < count; i++) {
		const display_output_config &config = configs[i];
		Output *output = nullptr;
		for (auto &candidate: outputs) {
			if (candidate.id == config.id)
				output = &candidate;
		}
		if (output == nullptr || !output->connected) {
			debug_printf("nvidia_rm: layout names output %" B_PRIu32 ", which is not there\n",
				config.id);
			RaiseErrno(ENOENT);
		}
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if (config.scale < 100 || config.scale > 400) {
			debug_printf("nvidia_rm: layout asks for a scale of %u%%\n", config.scale);
			RaiseErrno(EINVAL);
		}
		uint16 renderScale = config.render_scale == 0 ? 100 : config.render_scale;
		if (renderScale < 100 || renderScale > 400) {
			debug_printf("nvidia_rm: layout asks for a render scale of %u%%\n", renderScale);
			RaiseErrno(EINVAL);
		}
		if (!AssignHead(*output, usedHeads))
			RaiseErrno(EBUSY);

		NvKmsMode timing = output->preferredMode;
		if (config.timing.h_display != 0) {
			// The exact timing if the monitor lists it, else the listed one
			// with that resolution nearest in refresh rate.
			std::vector<NvKmsMode> list = ModesOf(output->dpyId);
			const NvKmsMode *best = nullptr;
			uint32 wanted = CalcRefreshRate(config.timing);
			for (const auto &candidate: list) {
				display_timing candidateTiming = ToHaikuModeTimings(candidate.timings);
				if (ModeTimingsEqual(candidateTiming, config.timing)) {
					best = &candidate;
					break;
				}
				if (candidateTiming.h_display != config.timing.h_display
					|| candidateTiming.v_display != config.timing.v_display)
					continue;
				if (best == nullptr || std::abs((int64)candidate.timings.RRx1k - (int64)wanted)
						< std::abs((int64)best->timings.RRx1k - (int64)wanted))
					best = &candidate;
			}
			if (best == nullptr) {
				debug_printf("nvidia_rm: %s does not take %ux%u\n", output->name,
					config.timing.h_display, config.timing.v_display);
				RaiseErrno(EINVAL);
			}
			timing = *best;
		}

		output->enabled = true;
		output->mirror = (config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0;
		output->mode = timing;
		output->scale = config.scale;
		output->renderScale = renderScale;
		output->width = RegionSize(timing.timings.hVisible, config.scale, renderScale);
		output->height = RegionSize(timing.timings.vVisible, config.scale, renderScale);
		output->x = (config.x * renderScale + 50) / 100;
		output->y = (config.y * renderScale + 50) / 100;
		enabled++;
	}
	if (enabled == 0)
		RaiseErrno(EINVAL);

	// A mirror scans exactly its source's region: the output without the
	// flag whose region starts at the same place.
	for (auto &output: outputs) {
		if (!output.enabled || !output.mirror)
			continue;
		const Output *source = nullptr;
		for (const auto &candidate: outputs) {
			if (candidate.enabled && !candidate.mirror && candidate.x == output.x
				&& candidate.y == output.y)
				source = &candidate;
		}
		if (source == nullptr) {
			debug_printf("nvidia_rm: layout has %s mirror nothing at %" B_PRId32 ",%" B_PRId32 "\n",
				output.name, output.x, output.y);
			RaiseErrno(EINVAL);
		}
		output.width = source->width;
		output.height = source->height;
	}

	// Regions are kept at the frame buffer's origin. The display engine
	// has to agree before the layout replaces the current one.
	int32 minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
	for (const auto &output: outputs) {
		if (!output.enabled)
			continue;
		minX = std::min(minX, output.x);
		minY = std::min(minY, output.y);
		maxX = std::max(maxX, output.x + (int32)output.width);
		maxY = std::max(maxY, output.y + (int32)output.height);
	}
	for (auto &output: outputs) {
		if (!output.enabled)
			continue;
		output.x -= minX;
		output.y -= minY;
	}
	if (!CheckLayout(outputs, maxX - minX, maxY - minY))
		RaiseErrno(EINVAL);

	fOutputs = outputs;
	BuildLayoutMode();
	ReadModeList();

	for (const auto &output: fOutputs) {
		if (!output.enabled)
			continue;
		debug_printf("nvidia_rm: layout: %s head %" B_PRIu32 " %ux%u@%u at %" B_PRId32 ",%" B_PRId32
			" scale %u%% (%ux%u, drawn at %u%%)%s\n", output.name, output.head,
			(unsigned)output.mode.timings.hVisible, (unsigned)output.mode.timings.vVisible,
			(unsigned)(output.mode.timings.RRx1k / 1000), output.x, output.y, output.scale,
			output.width, output.height, output.renderScale, output.mirror ? " mirror" : "");
	}

	*mode = ToHaikuMode(fLayoutMode);
	if (fCurrentHaikuMode.space != 0)
		mode->space = fCurrentHaikuMode.space;
}

void NvAccelerant::SetDisplayChangePort(port_id port, int32 code)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fChangePort = port;
	fChangeCode = code;
}


// #pragma mark - hotplug


// The hot plug detect lines, straight from resman. NVKMS's own idea of what
// is connected is only as fresh as the last hotplug event it was told about,
// so this is what the poll compares.
NvRmObject &NvAccelerant::RmDisplay()
{
	if (fRmDisplay.Get() == 0) {
		fRmDisplay = fRmDev.Device().Alloc(NV04_DISPLAY_COMMON, NULL, 0);
		NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS supported {};
		fRmDisplay.Control(NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, &supported, sizeof(supported));
		fRmDisplayMask = supported.displayMask;
	}
	return fRmDisplay;
}

NvU32 NvAccelerant::ConnectedDisplayMask()
{
	RmDisplay();

	NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS params {};
	params.displayMask = fRmDisplayMask;
	params.flags = DRF_DEF(0073_CTRL, _SYSTEM_GET_CONNECT_STATE_FLAGS, _DDC, _DISABLE)
		| DRF_DEF(0073_CTRL, _SYSTEM_GET_CONNECT_STATE_FLAGS, _LOAD, _DISABLE);
	NvU32 tries = 0;
	do {
		params.retryTimeMs = 0;
		fRmDisplay.Control(NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, &params, sizeof(params));
		if (params.retryTimeMs > 0)
			snooze(params.retryTimeMs * 1000LL);
	} while (params.retryTimeMs > 0 && ++tries < 50);
	return params.displayMask;
}

// Ask NVKMS about every connector again. Monitors that appeared are added,
// disabled, to the right of the layout; monitors that went are marked and
// their heads freed when app_server next applies a layout. Returns whether
// anything changed.
bool NvAccelerant::RefreshOutputs()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);

	bool changed = false;
	int32 right = 0;
	for (const auto &output: fOutputs) {
		if (output.enabled)
			right = std::max(right, output.x + (int32)output.width);
	}

	for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), fValidDpys);
			!nvDpyIdIsInvalid(dpyId);
			dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, fValidDpys)) {
		Output *known = OutputByID(nvDpyIdToNvU32(dpyId));
		if (known != nullptr && known->forced)
			continue;

		Output probe {};
		probe.dpyId = dpyId;
		probe.id = nvDpyIdToNvU32(dpyId);
		bool connected = QueryOutput(probe);

		if (known == nullptr) {
			if (!connected)
				continue;
			NvKmsQueryDpyStaticDataParams staticParams {};
			staticParams.request.deviceHandle = fKmsDev.Get();
			staticParams.request.dispHandle = fDisp;
			staticParams.request.dpyId = dpyId;
			CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &staticParams, sizeof(staticParams)));
			probe.headMask = staticParams.reply.headMask;
			probe.displayPort = IsDisplayPort(dpyId);
			probe.preferredMode = PreferredMode(dpyId);
			probe.mode = probe.preferredMode;
			if (probe.mode.timings.hVisible == 0)
				continue;
			probe.scale = 100;
			probe.renderScale = 100;
			probe.width = LogicalSize(probe.mode.timings.hVisible, probe.scale);
			probe.height = LogicalSize(probe.mode.timings.vVisible, probe.scale);
			probe.x = right;
			probe.y = 0;
			probe.enabled = false;
			right += probe.width;
			debug_printf("nvidia_rm: %s connected\n", probe.name);
			fOutputs.push_back(probe);
			changed = true;
			continue;
		}

		if (known->connected != connected) {
			debug_printf("nvidia_rm: %s %s\n", known->name,
				connected ? "connected" : "disconnected");
			known->connected = connected;
			changed = true;
		}
		if (connected) {
			// The same connector may now have a different monitor on it.
			if (known->edid != probe.edid) {
				debug_printf("nvidia_rm: %s has a different monitor\n", known->name);
				known->edid = probe.edid;
				known->preferredMode = PreferredMode(dpyId);
				if (!known->enabled) {
					known->mode = known->preferredMode;
					known->width = RegionSize(known->mode.timings.hVisible, known->scale,
						known->renderScale);
					known->height = RegionSize(known->mode.timings.vVisible, known->scale,
						known->renderScale);
				}
				changed = true;
			}
		}
	}

	// A disconnected output that is not being driven has nothing to say.
	for (auto it = fOutputs.begin(); it != fOutputs.end();) {
		if (!it->connected && !it->enabled && !it->forced)
			it = fOutputs.erase(it);
		else
			++it;
	}
	return changed;
}

void NvAccelerant::NotifyDisplayChange()
{
	port_id port;
	int32 code;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		port = fChangePort;
		code = fChangeCode;
	}
	if (port < 0)
		return;
	write_port_etc(port, code, NULL, 0, B_RELATIVE_TIMEOUT, 100000);
}

bool NvAccelerant::IsDisplayPort(NVDpyId dpyId)
{
	NvKmsQueryDpyStaticDataParams staticParams {};
	staticParams.request.deviceHandle = fKmsDev.Get();
	staticParams.request.dispHandle = fDisp;
	staticParams.request.dpyId = dpyId;
	if (fKms.Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &staticParams, sizeof(staticParams)) < 0)
		return false;
	NvKmsQueryConnectorStaticDataParams connector {};
	connector.request.deviceHandle = fKmsDev.Get();
	connector.request.dispHandle = fDisp;
	connector.request.connectorHandle = staticParams.reply.connectorHandle;
	if (fKms.Control(NVKMS_IOCTL_QUERY_CONNECTOR_STATIC_DATA, &connector, sizeof(connector)) < 0)
		return false;
	return connector.reply.isDP;
}

status_t NvAccelerant::HotplugThreadEntry(void *arg)
{
	static_cast<NvAccelerant*>(arg)->HotplugThread();
	return B_OK;
}

// Two ways of hearing about a monitor being plugged in or pulled out: NVKMS
// posts an event when resman tells it about a hotplug, which is immediate,
// and every couple of seconds the hot plug detect lines are read directly,
// which catches what the event path misses. Either way the connectors are
// queried again and app_server is told when something differs.
void NvAccelerant::HotplugThread()
{
	NvU32 lastMask = 0;
	bool haveMask = false;
	try {
		lastMask = ConnectedDisplayMask();
		haveMask = true;
	} catch (const std::system_error &ex) {
		debug_printf("nvidia_rm: cannot read the hot plug lines: %s\n", ex.what());
	}

	while (!fQuitHotplugThread.load()) {
		bool check = false;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(fKms.Fd(), &readSet);
		struct timeval timeout = { 2, 0 };
		int ready = select(fKms.Fd() + 1, &readSet, NULL, NULL, &timeout);
		if (fQuitHotplugThread.load())
			break;
		if (ready > 0) {
			std::lock_guard<std::recursive_mutex> lock(fLock);
			for (;;) {
				NvKmsGetNextEventParams next {};
				if (fKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &next, sizeof(next)) < 0
					|| !next.reply.valid)
					break;
				switch (next.reply.event.eventType) {
					case NVKMS_EVENT_TYPE_DPY_CHANGED:
					case NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED:
					case NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED:
						debug_printf("nvidia_rm: display change event %d\n",
							(int)next.reply.event.eventType);
						check = true;
						break;
					default:
						break;
				}
			}
		} else if (ready < 0) {
			snooze(2000000);
		}

		if (haveMask) {
			try {
				NvU32 mask = ConnectedDisplayMask();
				if (mask != lastMask) {
					debug_printf("nvidia_rm: hot plug lines 0x%" B_PRIx32 " -> 0x%" B_PRIx32 "\n",
						lastMask, mask);
					lastMask = mask;
					check = true;
				}
			} catch (const std::system_error &ex) {
				debug_printf("nvidia_rm: cannot read the hot plug lines: %s\n", ex.what());
				haveMask = false;
			}
		}

		bigtime_t recheckAt = fRecheckAt.load();
		if (recheckAt != 0 && system_time() >= recheckAt) {
			fRecheckAt.store(0);
			CheckLinksAfterWake();
			check = true;
		}
		if (!check)
			continue;

		// A DisplayPort monitor lets go of its hot plug line once it has been
		// asleep for a while, and may take it again later while still asleep
		// (the Dell P2415Q does both within minutes). Taking it out of the
		// layout for that moves every window off it and loses its place, and
		// with no picture sent it may never wake. While the displays sleep,
		// only remember that something happened; waking them looks again.
		{
			std::lock_guard<std::recursive_mutex> lock(fLock);
			if (fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON) {
				if (!fChangedWhileAsleep)
					debug_printf("nvidia_rm: a display changed while asleep; looking again once awake\n");
				fChangedWhileAsleep = true;
				continue;
			}
		}
		try {
			// Give the monitor a moment to answer its EDID before asking.
			snooze(300000);
			if (RefreshOutputs())
				NotifyDisplayChange();
		} catch (const std::system_error &ex) {
			debug_printf("[!] nvidia_rm: re-detecting displays failed: %s\n", ex.what());
		}
	}
}

void NvAccelerant::StartHotplugThread()
{
	NvKmsDeclareEventInterestParams interest {};
	interest.request.interestMask = (1U << NVKMS_EVENT_TYPE_DPY_CHANGED)
		| (1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED)
		| (1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED);
	if (fKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &interest, sizeof(interest)) < 0)
		debug_printf("nvidia_rm: NVKMS will not report display events\n");

	fQuitHotplugThread.store(false);
	fHotplugThread = spawn_thread(HotplugThreadEntry, "nvidia_rm hotplug",
		B_LOW_PRIORITY, this);
	if (fHotplugThread >= 0)
		resume_thread(fHotplugThread);
}

void NvAccelerant::StopHotplugThread()
{
	if (fHotplugThread < 0)
		return;
	fQuitHotplugThread.store(true);
	status_t result;
	wait_for_thread(fHotplugThread, &result);
	fHotplugThread = -1;
}

// Telling the rest of the system when the display is between frames.
//
// NVKMS writes the frame number into memory a client registers, at every
// vertical blank. Nothing here has to ask for it - the frame number is updated
// whether or not a request is pending - so the thread below only reads, and
// releases Haiku's retrace semaphore each time the number changes. That is
// what BScreen::WaitForRetrace() waits on, so every program gets it, not only
// the one presenting with the GPU.
// NVKMS stops reporting blanks for a head it has shut down, and the head a
// mode is driven from can change, so anything that sets a mode - including
// coming back from suspend, where the display was off entirely - has to ask
// again.
void NvAccelerant::RefreshVblankReports()
{
	if (fRetraceThread < 0)
		return;

	DisableVblankReports();
	EnableVblankReports();
}

void NvAccelerant::EnableVblankReports()
{
	if (!fKmsDev.Info().supportsVblankSemControl) {
		debug_printf("nvidia_rm: the driver will not report vertical blanks\n");
		return;
	}

	try {
		if (!fVblankSurface.IsSet()) {
			const uint64 size = B_PAGE_SIZE;
			NvU8 compressible = 0;
			nvKmsKapiAllocateSystemMemory(fRmDev, fKmsDev, fVblankMemory,
				NvKmsSurfaceMemoryLayoutPitch, size,
				NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN, &compressible);

			FileDesc memoryFd = fRmDev.ExportObjectToFd(fRmDev.Device().Get(),
				fVblankMemory.Get());

			NvKmsRegisterSurfaceParams params {};
			params.request.deviceHandle = fKmsDev.Get();
			params.request.useFd = true;
			params.request.planes[0].u.fd = memoryFd.Get();
			params.request.planes[0].offset = 0;
			params.request.planes[0].pitch = size;
			params.request.planes[0].rmObjectSizeInBytes = size;
			params.request.widthInPixels = size / 4;
			params.request.heightInPixels = 1;
			params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
			params.request.format = NvKmsSurfaceMemoryFormatX8R8G8B8;
			// Nothing scans this out; it is a place for counters.
			params.request.noDisplayHardwareAccess = true;
			params.request.isoType = NVKMS_MEMORY_NISO;
			CheckErrno(fKms.Control(NVKMS_IOCTL_REGISTER_SURFACE, &params,
				sizeof(params)));
			fVblankSurface = NvKmsSurface(fKmsDev, params.reply.surfaceHandle);

			fVblankMapping = fRmDev.MapMemory(fVblankMemory.Get(), true, 0,
				size, 0);
			memset(fVblankMapping.Address(), 0,
				sizeof(NvKmsVblankSemControlData));
		}

		NvKmsEnableVblankSemControlParams enableParams {};
		enableParams.request.deviceHandle = fKmsDev.Get();
		enableParams.request.dispHandle = fDisp;
		enableParams.request.headMask = 1U << fHead;
		enableParams.request.surfaceHandle = fVblankSurface.Get();
		enableParams.request.surfaceOffset = 0;
		CheckErrno(fKms.Control(NVKMS_IOCTL_ENABLE_VBLANK_SEM_CONTROL,
			&enableParams, sizeof(enableParams)));
		fVblankControl = enableParams.reply.vblankSemControlHandle;
	} catch (const std::system_error &ex) {
		debug_printf("[!] nvidia_rm: no vertical blank reports: %s\n",
			ex.what());
		fVblankControl = 0;
	}
}

void NvAccelerant::DisableVblankReports()
{
	if (fVblankControl == 0)
		return;

	NvKmsDisableVblankSemControlParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.vblankSemControlHandle = fVblankControl;
	fKms.Control(NVKMS_IOCTL_DISABLE_VBLANK_SEM_CONTROL, &params,
		sizeof(params));
	fVblankControl = 0;
}

status_t NvAccelerant::RetraceThreadEntry(void *arg)
{
	static_cast<NvAccelerant*>(arg)->RetraceThread();
	return B_OK;
}

void NvAccelerant::RetraceThread()
{
	auto *data = (volatile NvKmsVblankSemControlData *)fVblankMapping.Address();
	uint64 previous = 0;
	bigtime_t lastChange = system_time();

	while (!fQuitRetraceThread.load()) {
		// Following the display closely costs a few hundred wake-ups a second,
		// which is worth paying only while somebody is waiting for a blank. A
		// semaphore with a negative count has threads blocked on it; with none
		// blocked, look every so often for one arriving and leave the
		// processor alone in between. The cost of that is that the first wait
		// after a quiet spell can be one look late.
		int32 count = 0;
		const bool waiting = get_sem_count(fRetraceSem, &count) == B_OK
			&& count < 0;
		if (!waiting) {
			snooze(10000);
			previous = data->head[fHead].vblankCount;
			lastChange = system_time();
			continue;
		}

		volatile NvKmsVblankSemControlDataOneHead &one = data->head[fHead];
		const uint64 vblankCount = one.vblankCount;

		if (vblankCount != previous) {
			previous = vblankCount;
			lastChange = system_time();

			// Every program waiting on this blank is let go on this blank,
			// rather than one of them per blank. B_RELEASE_ALL also leaves the
			// count at zero, so a semaphore nobody is waiting on does not
			// build up a stock of stale blanks to hand out later.
			release_sem_etc(fRetraceSem, 0,
				B_DO_NOT_RESCHEDULE | B_RELEASE_ALL);

			// The next blank is a frame away; there is no sense looking for it
			// until most of that frame has gone by.
			bigtime_t period = 1000000 / MAX(1, (int)CalcRefreshRate(
				fCurrentHaikuMode.timing));
			snooze(period > 2000 ? period - 1500 : 0);
			continue;
		}

		// Nothing is changing - the display is off, or the head this was
		// enabled on is no longer driving anything. Stop spinning on it.
		if (system_time() - lastChange > 500000) {
			snooze(100000);
			continue;
		}

		snooze(200);
	}
}

void NvAccelerant::StartRetraceThread()
{
	if (fRetraceThread >= 0)
		return;

	EnableVblankReports();
	if (fVblankControl == 0)
		return;

	if (fRetraceSem < 0) {
		fRetraceSem = create_sem(0, "nvidia_rm retrace");
		if (fRetraceSem < 0)
			return;
	}

	fQuitRetraceThread.store(false);
	fRetraceThread = spawn_thread(RetraceThreadEntry, "nvidia_rm retrace",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fRetraceThread < 0) {
		DisableVblankReports();
		return;
	}
	resume_thread(fRetraceThread);
}

void NvAccelerant::StopRetraceThread()
{
	if (fRetraceThread >= 0) {
		fQuitRetraceThread.store(true);
		status_t result;
		wait_for_thread(fRetraceThread, &result);
		fRetraceThread = -1;
	}
	DisableVblankReports();
}

sem_id NvAccelerant::RetraceSemaphore()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fRetraceThread < 0)
		StartRetraceThread();

	return fRetraceSem >= 0 ? fRetraceSem : B_ERROR;
}


// Let other programs draw straight into the screen.
//
// The frame buffer is video memory, so a program rendering with the GPU can
// write its finished frame there without it ever crossing the bus. Sharing the
// memory object lets such a program duplicate the handle into its own resman
// client; the driver keeps the description so that it can be asked for.
void NvAccelerant::PublishScanout()
{
	nv_haiku_scanout_info info {};
	if (fFramebuffer.IsSet()) {
		try {
			NV0000_CTRL_CLIENT_SHARE_OBJECT_PARAMS shareParams {};
			shareParams.hObject = fFramebuffer.Memory().Get();
			shareParams.sharePolicy.type = RS_SHARE_TYPE_ALL;
			RS_ACCESS_MASK_ADD(&shareParams.sharePolicy.accessMask,
				RS_ACCESS_DUP_OBJECT);
			fRm.Client().Control(NV0000_CTRL_CMD_CLIENT_SHARE_OBJECT,
				&shareParams, sizeof(shareParams));
		} catch (const std::system_error &ex) {
			debug_printf("[!] NvAccelerant: sharing the frame buffer failed\n");
			return;
		}

		info.client = fRm.Client().Get();
		info.memory = fFramebuffer.Memory().Get();
		info.width = fFramebuffer.Width();
		info.height = fFramebuffer.Height();
		info.bytes_per_row = fFramebuffer.BytesPerRow();
		info.color_space = fFramebuffer.ColorSpace();
		info.size = (uint64)fFramebuffer.BytesPerRow() * fFramebuffer.Height();
	}

	if (ioctl(fDevFd.Get(), NV_HAIKU_BASE + NV_HAIKU_PUBLISH_SCANOUT, &info,
			sizeof(info)) < 0) {
		debug_printf("[!] NvAccelerant: publishing the frame buffer failed\n");
	}
}


void NvAccelerant::GetFrameBufferConfig(frame_buffer_config* frameBuffer)
{
	debug_printf("NvAccelerant::GetFrameBufferConfig\n");

	if (!fFramebuffer.IsSet()) {
		RaiseErrno(ENOENT);
	}
	frameBuffer->frame_buffer = fFramebuffer.Bits();
	frameBuffer->frame_buffer_dma = nullptr;
	frameBuffer->bytes_per_row = fFramebuffer.BytesPerRow();
}

void NvAccelerant::GetPixelClockLimits(display_mode* dm, uint32* low, uint32* high)
{
	debug_printf("NvAccelerant::GetPixelClockLimits\n");

	*low = UINT32_MAX;
	*high = 0;

	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timing = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timing.h_display == dm->timing.h_display &&
			timing.v_display == dm->timing.v_display &&
			(timing.flags & B_TIMING_INTERLACED) == (dm->timing.v_display & B_TIMING_INTERLACED)
		) {
			*low = std::min(*low, timing.pixel_clock);
			*high = std::max(*high, timing.pixel_clock);
		}
	}
	if (*low > *high) {
		RaiseErrno(B_ERROR);
	}
}

uint32 NvAccelerant::DpmsCapabilities()
{
	debug_printf("NvAccelerant::DpmsCapabilities\n");

	return B_DPMS_ON | B_DPMS_OFF;
}

uint32 NvAccelerant::DpmsMode()
{
	debug_printf("NvAccelerant::DpmsMode\n");

	switch (fDpmsState) {
		case NV_KMS_DPY_ATTRIBUTE_DPMS_ON:
			return B_DPMS_ON;
		case NV_KMS_DPY_ATTRIBUTE_DPMS_OFF:
			return B_DPMS_OFF;
		default:
			return B_DPMS_OFF;
	}
}

void NvAccelerant::SetDpmsMode(uint32 dpms_flags)
{
	debug_printf("NvAccelerant::SetDpmsMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);

	NvS64 value;
	switch (dpms_flags) {
		case B_DPMS_ON:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
			break;
		case B_DPMS_OFF:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_OFF;
			break;
		default:
			RaiseErrno(EINVAL);
	}

	bool waking = value == NV_KMS_DPY_ATTRIBUTE_DPMS_ON
		&& fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
	for (const auto &output: fOutputs) {
		if (fLayoutApplied ? !output.enabled : !nvDpyIdsAreEqual(output.dpyId, fDpyId))
			continue;
		NvKmsSetDpyAttributeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = output.dpyId;
		params.request.attribute = NV_KMS_DPY_ATTRIBUTE_DPMS;
		params.request.value = value;
		int result = fKms.Control(NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &params, sizeof(params));
		if (result < 0 && waking) {
			// One monitor that will not listen must not keep the others
			// dark; WakeDisplays() below takes care of it.
			debug_printf("nvidia_rm: %s did not take the power state\n", output.name);
			continue;
		}
		CheckErrno(result);
	}
	fDpmsState = (NvKmsDpyAttributeDpmsValue)value;

	if (waking)
		WakeDisplays();
}


// Talk to a DisplayPort monitor's DPCD over its AUX channel through resman.
// The display ID is the NVKMS dpy ID, which for a connector is resman's.
bool NvAccelerant::AccessDpcd(NvU32 displayId, NvU32 address, NvU8 *data, NvU32 size,
	bool write)
{
	NV0073_CTRL_DP_AUXCH_CTRL_PARAMS aux {};
	aux.displayId = displayId;
	aux.cmd = DRF_DEF(0073_CTRL, _DP_AUXCH_CMD, _TYPE, _AUX)
		| (write ? DRF_DEF(0073_CTRL, _DP_AUXCH_CMD, _REQ_TYPE, _WRITE)
			: DRF_DEF(0073_CTRL, _DP_AUXCH_CMD, _REQ_TYPE, _READ));
	aux.addr = address;
	aux.size = size - 1; // the control call takes a zero-based size
	if (write)
		memcpy(aux.data, data, size);
	try {
		RmDisplay().Control(NV0073_CTRL_CMD_DP_AUXCH_CTRL, &aux, sizeof(aux));
	} catch (const std::system_error &) {
		return false;
	}
	if (aux.replyType != NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK)
		return false;
	if (!write) {
		if (aux.size + 1 < size)
			return false;
		memcpy(data, aux.data, size);
	}
	return true;
}

// Whether a DisplayPort monitor is awake and its link trained: every lane has
// its clock recovered, is equalized and has symbol lock, and the lanes are
// aligned. Then it shows what the head sends it. Other kinds of output have
// nothing to ask, and count as up.
bool NvAccelerant::LinkIsUp(const Output &output)
{
	if (!output.displayPort)
		return true;
	NvU8 power = 0, link[2] = {}, status[3] = {};
	if (!AccessDpcd(output.id, 0x600, &power, 1, false)
		|| !AccessDpcd(output.id, 0x100, link, 2, false)
		|| !AccessDpcd(output.id, 0x202, status, 3, false))
		return false;
	unsigned lanes = link[1] & 0x1f;
	if ((power & 7) != 1 || lanes == 0 || (status[2] & 1) == 0)
		return false;
	for (unsigned lane = 0; lane < lanes && lane < 4; lane++) {
		if (((status[lane / 2] >> (4 * (lane % 2))) & 7) != 7)
			return false;
	}
	return true;
}

// Ask a DisplayPort monitor to power up (DPCD SET_POWER to D0). One in its
// deepest sleep may take a while to answer its AUX channel at all; NVKMS
// gives up after a few milliseconds, this keeps asking for a second.
void NvAccelerant::WakeSink(const Output &output)
{
	for (int attempt = 0; attempt < 50; attempt++) {
		NvU8 d0 = 1;
		if (AccessDpcd(output.id, 0x600, &d0, 1, true)) {
			if (attempt > 0)
				debug_printf("nvidia_rm: %s answered after %d attempts\n", output.name,
					attempt + 1);
			return;
		}
		snooze(20000);
	}
	debug_printf("nvidia_rm: %s does not answer on its AUX channel\n", output.name);
}

// Makes sure every enabled monitor shows a picture: each DisplayPort link is
// checked, and when one is not up (or \a always) the monitor is asked to
// power up and the layout programmed anew, which trains every link from the
// start. Returns whether all links are up afterwards. fLock must be held.
bool NvAccelerant::RetrainLinks(bool always)
{
	if (!fLayoutApplied || !fFramebuffer.IsSet())
		return true;

	bool allUp = true;
	for (const auto &output: fOutputs) {
		if (!output.enabled || LinkIsUp(output))
			continue;
		debug_printf("nvidia_rm: %s is not showing a picture\n", output.name);
		allUp = false;
		WakeSink(output);
	}
	if (allUp && !always)
		return true;

	debug_printf("nvidia_rm: programming the layout again\n");
	try {
		ApplyLayout(fFramebuffer);
		RefreshVblankReports();
		if (!fCursorImage.data.empty()) {
			SetCursorBitmap(fCursorImage.width, fCursorImage.height,
				fCursorImage.hotX, fCursorImage.hotY, fCursorImage.colorSpace,
				fCursorImage.bytesPerRow, fCursorImage.data.data());
		}
		if (fCursorVisible)
			UpdateCursor(true, true);
	} catch (const std::system_error &ex) {
		debug_printf("[!] nvidia_rm: programming the layout failed: %s\n", ex.what());
		return false;
	}

	allUp = true;
	for (const auto &output: fOutputs) {
		if (output.enabled && !LinkIsUp(output)) {
			debug_printf("nvidia_rm: %s is still not showing a picture\n", output.name);
			allUp = false;
		}
	}
	return allUp;
}

// The displays were asleep and have been told to wake. For DisplayPort that
// is a request to the monitor to power up; one that misses it stays asleep
// with its link powered down and never sees a picture again. So the links are
// checked now, and again a few times over the next seconds: a monitor that
// had let go of its hot plug line comes back only after a moment, and cannot
// be trained before. Once the monitors had that time the connectors are
// looked at too; one really pulled out while asleep is only reported then.
void NvAccelerant::WakeDisplays()
{
	bool changed = fChangedWhileAsleep;
	fChangedWhileAsleep = false;
	if (changed)
		debug_printf("nvidia_rm: a display changed while asleep\n");
	fWakeChecksLeft = RetrainLinks(changed) ? 1 : 3;
	fRecheckAt.store(system_time() + 2000000);
}

void NvAccelerant::CheckLinksAfterWake()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fWakeChecksLeft <= 0 || fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON)
		return;
	fWakeChecksLeft--;
	if (RetrainLinks(false))
		fWakeChecksLeft = 0;
	if (fWakeChecksLeft > 0)
		fRecheckAt.store(system_time() + 3000000);
}

// The layout's frame buffer is what app_server should come up in.
void NvAccelerant::GetPreferredDisplayMode(display_mode* preferredMode)
{
	debug_printf("NvAccelerant::GetPreferredDisplayMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fLayoutMode.timings.hVisible == 0)
		RaiseErrno(ENOENT);
	*preferredMode = ToHaikuMode(fLayoutMode);
}

void NvAccelerant::GetMonitorInfo(monitor_info* info)
{
	debug_printf("NvAccelerant::GetMonitorInfo\n");

	RaiseErrno(ENOSYS);
}

// The primary monitor's EDID, for the classic single-monitor interface.
void NvAccelerant::GetEdidInfo(void* info, uint32 size, uint32* _version)
{
	debug_printf("NvAccelerant::GetEdidInfo\n");

	if (size < sizeof(struct edid1_info)) {
		RaiseErrno(B_BUFFER_OVERFLOW);
	}

	std::lock_guard<std::recursive_mutex> lock(fLock);
	Output *primary = PrimaryOutput();
	if (primary == nullptr || primary->edid.size() < sizeof(edid1_raw))
		RaiseErrno(ENOENT);
	edid_decode((edid1_info*)info, (const edid1_raw*)primary->edid.data());
	*_version = EDID_VERSION_1;
}


void NvAccelerant::UpdateCursor(bool updateImage, bool updatePos)
{
	for (const auto &output: fOutputs) {
		if (fLayoutApplied ? !output.enabled : output.head != fHead)
			continue;

		if (updateImage) {
			NvKmsSetCursorImageParams params {
				.request = {
					.deviceHandle = fKmsDev.Get(),
					.dispHandle = fDisp,
					.head = output.head,
					.common = {
						.surfaceHandle = {
							fCursorVisible ? (fNewCursor.IsSet() ? fNewCursor.Surface().Get() : fCursor.Surface().Get()) : 0,
						},
						.cursorCompParams = {
							.blendingMode = {
								NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
								NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
							}
						},
					},
				},
			};
			CheckErrno(fKms.Control(NVKMS_IOCTL_SET_CURSOR_IMAGE, &params, sizeof(params)));
		}

		if (updatePos) {
			NvKmsMoveCursorParams params {
				.request = {
					.deviceHandle = fKmsDev.Get(),
					.dispHandle = fDisp,
					.head = output.head,
					.common = {
						.x = (NvS16)(fCursorPos.x - fCursorHotSpot.x - (fLayoutApplied ? output.x : 0)),
						.y = (NvS16)(fCursorPos.y - fCursorHotSpot.y - (fLayoutApplied ? output.y : 0)),
					},
				},
			};
			CheckErrno(fKms.Control(NVKMS_IOCTL_MOVE_CURSOR, &params, sizeof(params)));
		}
	}

	if (updateImage && fNewCursor.IsSet()) {
		fCursor = std::move(fNewCursor);
	}
}

void NvAccelerant::MoveCursor(uint16 x, uint16 y)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fCursorPos.x = x;
	fCursorPos.y = y;
	UpdateCursor(false, true);
}

void NvAccelerant::ShowCursor(bool isVisible)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fCursorVisible == isVisible) {
		return;
	}
	fCursorVisible = !fCursorVisible;
	UpdateCursor(true, false);
}

void NvAccelerant::SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask)
{
	RaiseErrno(ENOSYS);
}

void NvAccelerant::SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData)
{
	int32 cursorWidth;
	int32 cursorHeight;
	if (width > 256 || height > 256) {
		RaiseErrno(EINVAL);
	} else if (width > 128 || height > 128) {
		cursorWidth = 256;
		cursorHeight = 256;
	} else if (width > 64 || height > 64) {
		cursorWidth = 128;
		cursorHeight = 128;
	} else {
		// Not 32 x 32: the display engine reads a cursor's rows packed,
		// 128 bytes apart at that size, and NvKmsBitmap starts them every 256.
		cursorWidth = 64;
		cursorHeight = 64;
	}

	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (bitmapData != fCursorImage.data.data()) {
		fCursorImage.width = width;
		fCursorImage.height = height;
		fCursorImage.hotX = hotX;
		fCursorImage.hotY = hotY;
		fCursorImage.colorSpace = colorSpace;
		fCursorImage.bytesPerRow = bytesPerRow;
		fCursorImage.data.assign(bitmapData, bitmapData + bytesPerRow * height);
	}

	NvKmsBitmap newCursor(fRmDev, fKmsDev, cursorWidth, cursorHeight, colorSpace);
	memset(newCursor.Bits(), 0, newCursor.BitsLength());
	auto dstLine = (uint8*)newCursor.Bits();
	auto srcLine = bitmapData;
	for (int32 y = 0; y < height; y++) {
		memcpy(dstLine, srcLine, 4*width /* FIXME */);
		dstLine += newCursor.BytesPerRow();
		srcLine += bytesPerRow;
	}

	fNewCursor = std::move(newCursor);
	fCursorHotSpot.x = hotX;
	fCursorHotSpot.y = hotY;

	if (fCursorVisible) {
		UpdateCursor(true, true);
	}
}



_EXPORT void *get_accelerant_hook(uint32 feature, void *data)
{
	switch (feature) {
		case B_INIT_ACCELERANT: {
			init_accelerant fn = [](int fd) {
				try {
					NvAccelerant::Init(fd);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_ACCELERANT_CLONE_INFO_SIZE: {
			accelerant_clone_info_size fn = []() {
				return NvAccelerant::Instance()->CloneInfoSize();
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_CLONE_INFO: {
			get_accelerant_clone_info fn = [](void* data) {
				return NvAccelerant::Instance()->GetCloneInfo(data);
			};
			return (void*)fn;
		}
		case B_CLONE_ACCELERANT: {
			clone_accelerant fn = [](void *data) {
				try {
					NvAccelerant::Clone(data);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_UNINIT_ACCELERANT: {
			uninit_accelerant fn = []() {
				try {
					NvAccelerant::Instance()->Uninit();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_DEVICE_INFO: {
			get_accelerant_device_info fn = [](accelerant_device_info* adi) {
				try {
					NvAccelerant::Instance()->GetDeviceInfo(adi);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		case B_ACCELERANT_RETRACE_SEMAPHORE: {
			accelerant_retrace_semaphore fn = []() -> sem_id {
				try {
					return NvAccelerant::Instance()->RetraceSemaphore();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return B_ERROR;
				}
			};
			return (void*)fn;
		}

		case B_ACCELERANT_MODE_COUNT: {
			accelerant_mode_count fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->ModeCount();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_GET_MODE_LIST: {
			get_mode_list fn = [](display_mode* modes) {
				try {
					NvAccelerant::Instance()->GetModeList(modes);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_PROPOSE_DISPLAY_MODE: {
			propose_display_mode fn = [](display_mode *target, display_mode *low, display_mode *high) {
				try {
					return NvAccelerant::Instance()->ProposeMode(target, low, high);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_SET_DISPLAY_MODE: {
			set_display_mode fn = [](display_mode* modeToSet) {
				try {
					NvAccelerant::Instance()->SetDisplayMode(modeToSet);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_DISPLAY_MODE: {
			get_display_mode fn = [](display_mode* currentMode) {
				try {
					NvAccelerant::Instance()->GetDisplayMode(currentMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_FRAME_BUFFER_CONFIG: {
			get_frame_buffer_config fn = [](frame_buffer_config* frameBuffer) {
				try {
					NvAccelerant::Instance()->GetFrameBufferConfig(frameBuffer);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_PIXEL_CLOCK_LIMITS: {
			get_pixel_clock_limits fn = [](display_mode* dm, uint32* low, uint32* high) {
				try {
					NvAccelerant::Instance()->GetPixelClockLimits(dm, low, high);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		case B_DPMS_CAPABILITIES: {
			dpms_capabilities fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsCapabilities();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_DPMS_MODE: {
			dpms_mode fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsMode();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return B_DPMS_ON;
				}
			};
			return (void*)fn;
		}
		case B_SET_DPMS_MODE: {
			set_dpms_mode fn = [](uint32 dpms_flags) {
				try {
					NvAccelerant::Instance()->SetDpmsMode(dpms_flags);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_PREFERRED_DISPLAY_MODE: {
			get_preferred_display_mode fn = [](display_mode* preferredMode) {
				try {
					NvAccelerant::Instance()->GetPreferredDisplayMode(preferredMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_EDID_INFO: {
			get_edid_info fn = [](void* info, uint32 size, uint32* _version) {
				try {
					NvAccelerant::Instance()->GetEdidInfo(info, size, _version);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_WAIT_FOR_DISPLAY_RESTORE: {
			wait_for_display_restore fn = [](bigtime_t timeout) {
				return NvAccelerant::Instance()->WaitForDisplayRestore(timeout);
			};
			return (void*)fn;
		}

		case B_GET_DISPLAY_OUTPUT_COUNT: {
			get_display_output_count fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DisplayOutputCount();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_GET_DISPLAY_OUTPUTS: {
			get_display_outputs fn = [](display_output* outputs, uint32* count) {
				try {
					NvAccelerant::Instance()->GetDisplayOutputs(outputs, count);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_DISPLAY_OUTPUT_MODES: {
			get_display_output_modes fn = [](uint32 id, display_mode* modes, uint32* count) {
				try {
					NvAccelerant::Instance()->GetDisplayOutputModes(id, modes, count);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_SET_DISPLAY_LAYOUT: {
			set_display_layout fn = [](const display_output_config* configs, uint32 count, display_mode* mode) {
				try {
					NvAccelerant::Instance()->SetDisplayLayout(configs, count, mode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_SET_DISPLAY_CHANGE_PORT: {
			set_display_change_port fn = [](port_id port, int32 code) {
				try {
					NvAccelerant::Instance()->SetDisplayChangePort(port, code);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		// app_server's software cursor is drawn into the frame buffer, where
		// a direct window drawing into it paints over it.
		case B_MOVE_CURSOR: {
			move_cursor fn = [](uint16 x, uint16 y) {
				try {
					NvAccelerant::Instance()->MoveCursor(x, y);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
#if 0
		case B_SET_CURSOR_SHAPE: {
			set_cursor_shape fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask) {
				try {
					NvAccelerant::Instance()->SetCursorShape(width, height, hotX, hotY, andMask, xorMask);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
#endif
		case B_SHOW_CURSOR: {
			show_cursor fn = [](bool isVisible) {
				try {
					NvAccelerant::Instance()->ShowCursor(isVisible);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
		case B_SET_CURSOR_BITMAP: {
			set_cursor_bitmap fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData) {
				try {
					NvAccelerant::Instance()->SetCursorBitmap(width, height, hotX, hotY, colorSpace, bytesPerRow, bitmapData);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		default:
			return nullptr;
	}
}
