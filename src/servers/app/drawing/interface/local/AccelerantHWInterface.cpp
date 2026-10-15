/*
 * Copyright 2001-2016 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stephan Aßmus, superstippi@gmx.de
 *		DarkWyrm, bpmagic@columbus.rr.com
 *		Axel Dörfler, axeld@pinc-software.de
 *		Michael Lotz, mmlr@mlotz.ch
 *		John Scipione, jscipione@gmail.com
 */


//!	Accelerant based HWInterface implementation


#include "AccelerantHWInterface.h"

#include <new>

#include <dirent.h>
#include <edid.h>
#include <driver_settings.h>
#include <graphic_driver.h>
#include <image.h>
#include <safemode_defs.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <syscalls.h>
#include <syslog.h>
#include <unistd.h>

#include <Accelerant.h>
#include <Cursor.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <PathFinder.h>
#include <String.h>
#include <StringList.h>

#include "AccelerantBuffer.h"
#include "MallocBuffer.h"
#include "Overlay.h"
#include "RGBColor.h"
#include "ServerConfig.h"
#include "ServerCursor.h"
#include "ServerProtocol.h"
#include "SystemPalette.h"


using std::nothrow;


#ifdef DEBUG_DRIVER_MODULE
#	include <stdio.h>
#	define ATRACE(x) printf x
#else
#	define ATRACE(x) ;
#endif


const int32 kDefaultParamsCount = 64;


bool
operator==(const display_mode& a, const display_mode& b)
{
	return memcmp(&a, &b, sizeof(display_mode)) == 0;
}


bool
use_fail_safe_video_mode()
{
	char buffer[B_FILE_NAME_LENGTH];
	size_t size = sizeof(buffer);

	status_t status = _kern_get_safemode_option(
		B_SAFEMODE_FAIL_SAFE_VIDEO_MODE, buffer, &size);
	if (status == B_OK) {
		if (!strncasecmp(buffer, "true", size)
			|| !strncasecmp(buffer, "yes", size)
			|| !strncasecmp(buffer, "on", size)
			|| !strncasecmp(buffer, "enabled", size)) {
			return true;
		}
	}

	return false;
}


//	#pragma mark - AccelerantHWInterface


AccelerantHWInterface::AccelerantHWInterface()
	:
	HWInterface(),
	fCardFD(-1),
	fAccelerantImage(-1),
	fAccelerantHook(NULL),
	fEngineToken(NULL),
	fSyncToken(),

	// required hooks
	fAccGetModeCount(NULL),
	fAccGetModeList(NULL),
	fAccGetFrameBufferConfig(NULL),
	fAccSetDisplayMode(NULL),
	fAccGetDisplayMode(NULL),
	fAccGetPixelClockLimits(NULL),

	// optional accelerant hooks
	fAccGetTimingConstraints(NULL),
	fAccProposeDisplayMode(NULL),
	fAccSetCursorShape(NULL),
	fAccSetCursorBitmap(NULL),
	fAccMoveCursor(NULL),
	fAccShowCursor(NULL),

	// dpms hooks
	fAccDPMSCapabilities(NULL),
	fAccDPMSMode(NULL),
	fAccSetDPMSMode(NULL),

	// brightness hooks
	fAccSetBrightness(NULL),
	fAccGetBrightness(NULL),

	// overlay hooks
	fAccOverlayCount(NULL),
	fAccOverlaySupportedSpaces(NULL),
	fAccOverlaySupportedFeatures(NULL),
	fAccAllocateOverlayBuffer(NULL),
	fAccReleaseOverlayBuffer(NULL),
	fAccGetOverlayConstraints(NULL),
	fAccAllocateOverlay(NULL),
	fAccReleaseOverlay(NULL),
	fAccConfigureOverlay(NULL),

	fAccWaitForDisplayRestore(NULL),
	fDisplayRestoreThread(-1),
	fQuitDisplayRestoreThread(false),

	fAccGetDisplayOutputCount(NULL),
	fAccGetDisplayOutputs(NULL),
	fAccGetDisplayOutputModes(NULL),
	fAccSetDisplayLayout(NULL),
	fAccSetDisplayChangePort(NULL),
	fAccGetDisplayOutputRetraceSemaphore(NULL),
	fDisplayChangePort(-1),
	fDisplayChangeThread(-1),

	fModeCount(0),
	fModeList(NULL),

	fBackBuffer(NULL),
	fFrontBuffer(new (nothrow) AccelerantBuffer()),

	fInitialModeSwitch(true),

	fRetraceSemaphore(-1),

	fRectParams(new (nothrow) fill_rect_params[kDefaultParamsCount]),
	fRectParamsCount(kDefaultParamsCount),
	fBlitParams(new (nothrow) blit_params[kDefaultParamsCount]),
	fBlitParamsCount(kDefaultParamsCount)
{
	fDisplayMode.virtual_width = 0;
	fDisplayMode.virtual_height = 0;
	fDisplayMode.space = B_RGB32;

	// NOTE: I have no clue what I'm doing here.
	//fSyncToken.counter = 0;
	//fSyncToken.engine_id = 0;
	memset(&fSyncToken, 0, sizeof(sync_token));
}


AccelerantHWInterface::~AccelerantHWInterface()
{
	delete[] fRectParams;
	delete[] fBlitParams;

	delete[] fModeList;
}


/*!	Opens the first available graphics device and initializes it.

	\return B_OK on success or an appropriate error message on failure.
*/
status_t
AccelerantHWInterface::Initialize()
{
	status_t ret = HWInterface::Initialize();

	if (!fRectParams || !fBlitParams)
		return B_NO_MEMORY;

	if (ret >= B_OK) {
		for (int32 i = 0; fCardFD != B_ENTRY_NOT_FOUND; i++) {
			fCardFD = _OpenGraphicsDevice(i);
			if (fCardFD < 0) {
				ATRACE(("Failed to open graphics device\n"));
				continue;
			}

			if (_OpenAccelerant(fCardFD) == B_OK)
				break;

			close(fCardFD);
			// _OpenAccelerant() failed, try to open next graphics card
		}

		if (fCardFD < 0)
			return fCardFD;

		// copies to the screen are made at the vertical blanks
		_StartFlushing();
		return B_OK;
	}
	return ret;
}


/*!	Proceeds with a recursive scan, avoiding vesa and framebuffer devices.


	\return Whether a device path matching the \a deviceNumber is found.
*/
bool
AccelerantHWInterface::_RecursiveScan(const char* directory, int deviceNumber, int &count,
	char *_path)
{
	ATRACE(("_RecursiveScan directory: %s\n", directory));

	BEntry entry;
	BDirectory dir(directory);
	while (dir.GetNextEntry(&entry) == B_OK) {
		BPath path;
		entry.GetPath(&path);
		if (!strcmp(path.Path(), "/dev/graphics/vesa")
			|| !strcmp(path.Path(), "/dev/graphics/framebuffer")) {
			continue;
		}

		if (entry.IsDirectory()) {
			if (_RecursiveScan(path.Path(), deviceNumber, count, _path))
				return true;
		} else {
			if (count == deviceNumber) {
				strlcpy(_path, path.Path(), PATH_MAX);
				return true;
			}
			count++;
		}
	}
	return false;
}


/*!	Opens a graphics device for read-write access.

	The \a deviceNumber is relative to the number of graphics devices that can
	be opened. One represents the first card that can be opened (not necessarily
	the first one listed in the directory).

	Graphics drivers must be able to be opened more than once, so we really get
	the first working entry.

	\param deviceNumber Number identifying which graphics card to open
	       (1 for first card).

	\return The file descriptor of the opened graphics device.
*/
int
AccelerantHWInterface::_OpenGraphicsDevice(int deviceNumber)
{
	int device = -1;
	int count = 0;
	if (!use_fail_safe_video_mode()) {
		char path[PATH_MAX];
		if (_RecursiveScan("/dev/graphics/", deviceNumber, count, path))
			device = open(path, B_READ_WRITE);
	}

	// Open VESA or Framebuffer driver if we were not able to get a better one.
	if (device == -1 && count < deviceNumber) {
		device = open("/dev/graphics/vesa", B_READ_WRITE);
		if (device > 0) {
			// store the device, so that we can access the planar blitter
			fVGADevice = device;
		} else {
			device = open("/dev/graphics/framebuffer", B_READ_WRITE);
		}

		if (device < 0)
			return B_ENTRY_NOT_FOUND;
	}

	return device;
}


status_t
AccelerantHWInterface::_OpenAccelerant(int device)
{
	char signature[1024];
	if (ioctl(device, B_GET_ACCELERANT_SIGNATURE,
			&signature, sizeof(signature)) != B_OK) {
		return B_ERROR;
	}

	ATRACE(("accelerant signature is: %s\n", signature));

	fAccelerantImage = -1;

	BString leafPath("/accelerants/");
	leafPath << signature;
	BStringList addOnPaths;
	BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY, leafPath.String(),
		addOnPaths);
	int32 count = addOnPaths.CountStrings();
	for (int32 i = 0; i < count; i++) {
		const char* path = addOnPaths.StringAt(i).String();
		struct stat accelerantStat;
		if (stat(path, &accelerantStat) != 0)
			continue;

		ATRACE(("accelerant path is: %s\n", path));

		fAccelerantImage = load_add_on(path);
		if (fAccelerantImage >= 0) {
			if (get_image_symbol(fAccelerantImage, B_ACCELERANT_ENTRY_POINT,
					B_SYMBOL_TYPE_ANY, (void**)(&fAccelerantHook)) != B_OK) {
				ATRACE(("unable to get B_ACCELERANT_ENTRY_POINT\n"));
				unload_add_on(fAccelerantImage);
				fAccelerantImage = -1;
				return B_ERROR;
			}

			init_accelerant initAccelerant;
			initAccelerant = (init_accelerant)fAccelerantHook(
				B_INIT_ACCELERANT, NULL);
			if (!initAccelerant || initAccelerant(device) != B_OK) {
				ATRACE(("InitAccelerant unsuccessful\n"));
				unload_add_on(fAccelerantImage);
				fAccelerantImage = -1;
				return B_ERROR;
			}

			break;
		}
	}

	if (fAccelerantImage < B_OK)
		return B_ERROR;

	if (_SetupDefaultHooks() != B_OK) {
		syslog(LOG_ERR, "Accelerant %s does not export the required hooks.\n",
			signature);

		uninit_accelerant uninitAccelerant = (uninit_accelerant)
			fAccelerantHook(B_UNINIT_ACCELERANT, NULL);
		if (uninitAccelerant != NULL)
			uninitAccelerant();

		unload_add_on(fAccelerantImage);
		return B_ERROR;
	}

	return B_OK;
}


status_t
AccelerantHWInterface::_SetupDefaultHooks()
{
	// required
	fAccGetModeCount
		= (accelerant_mode_count)fAccelerantHook(B_ACCELERANT_MODE_COUNT, NULL);
	fAccGetModeList = (get_mode_list)fAccelerantHook(B_GET_MODE_LIST, NULL);
	fAccGetFrameBufferConfig = (get_frame_buffer_config)fAccelerantHook(
		B_GET_FRAME_BUFFER_CONFIG, NULL);
	fAccSetDisplayMode
		= (set_display_mode)fAccelerantHook(B_SET_DISPLAY_MODE, NULL);
	fAccGetDisplayMode
		= (get_display_mode)fAccelerantHook(B_GET_DISPLAY_MODE, NULL);
	fAccGetPixelClockLimits = (get_pixel_clock_limits)fAccelerantHook(
		B_GET_PIXEL_CLOCK_LIMITS, NULL);

	if (!fAccGetFrameBufferConfig || !fAccGetModeCount || !fAccGetModeList
			|| !fAccSetDisplayMode || !fAccGetDisplayMode || !fAccGetPixelClockLimits) {
		return B_ERROR;
	}

	// optional
	fAccGetTimingConstraints = (get_timing_constraints)fAccelerantHook(
		B_GET_TIMING_CONSTRAINTS, NULL);
	fAccProposeDisplayMode = (propose_display_mode)fAccelerantHook(
		B_PROPOSE_DISPLAY_MODE, NULL);
	fAccGetPreferredDisplayMode = (get_preferred_display_mode)fAccelerantHook(
		B_GET_PREFERRED_DISPLAY_MODE, NULL);
	fAccGetMonitorInfo
		= (get_monitor_info)fAccelerantHook(B_GET_MONITOR_INFO, NULL);
	fAccGetEDIDInfo = (get_edid_info)fAccelerantHook(B_GET_EDID_INFO, NULL);

	// cursor
	fAccSetCursorShape
		= (set_cursor_shape)fAccelerantHook(B_SET_CURSOR_SHAPE, NULL);
	fAccSetCursorBitmap
		= (set_cursor_bitmap)fAccelerantHook(B_SET_CURSOR_BITMAP, NULL);
	fAccMoveCursor = (move_cursor)fAccelerantHook(B_MOVE_CURSOR, NULL);
	fAccShowCursor = (show_cursor)fAccelerantHook(B_SHOW_CURSOR, NULL);

	// dpms
	fAccDPMSCapabilities
		= (dpms_capabilities)fAccelerantHook(B_DPMS_CAPABILITIES, NULL);
	fAccDPMSMode = (dpms_mode)fAccelerantHook(B_DPMS_MODE, NULL);
	fAccSetDPMSMode = (set_dpms_mode)fAccelerantHook(B_SET_DPMS_MODE, NULL);

	// brightness
	fAccGetBrightness = (get_brightness)fAccelerantHook(B_GET_BRIGHTNESS, NULL);
	fAccSetBrightness = (set_brightness)fAccelerantHook(B_SET_BRIGHTNESS, NULL);

	// display state restoring (after suspend)
	fAccWaitForDisplayRestore = (wait_for_display_restore)fAccelerantHook(
		B_WAIT_FOR_DISPLAY_RESTORE, NULL);
	if (fAccWaitForDisplayRestore != NULL && fDisplayRestoreThread < 0) {
		fQuitDisplayRestoreThread = false;
		fDisplayRestoreThread = spawn_thread(_DisplayRestoreThread,
			"display restore", B_DISPLAY_PRIORITY, this);
		if (fDisplayRestoreThread >= 0)
			resume_thread(fDisplayRestoreThread);
	}

	// display outputs and their layout
	fAccGetDisplayOutputCount = (get_display_output_count)fAccelerantHook(
		B_GET_DISPLAY_OUTPUT_COUNT, NULL);
	fAccGetDisplayOutputs = (get_display_outputs)fAccelerantHook(
		B_GET_DISPLAY_OUTPUTS, NULL);
	fAccGetDisplayOutputModes = (get_display_output_modes)fAccelerantHook(
		B_GET_DISPLAY_OUTPUT_MODES, NULL);
	fAccSetDisplayLayout = (set_display_layout)fAccelerantHook(
		B_SET_DISPLAY_LAYOUT, NULL);
	fAccSetDisplayChangePort = (set_display_change_port)fAccelerantHook(
		B_SET_DISPLAY_CHANGE_PORT, NULL);
	fAccGetDisplayOutputRetraceSemaphore
		= (get_display_output_retrace_semaphore)fAccelerantHook(
			B_GET_DISPLAY_OUTPUT_RETRACE_SEMAPHORE, NULL);
	if (fAccGetDisplayOutputCount == NULL || fAccGetDisplayOutputs == NULL
		|| fAccSetDisplayLayout == NULL) {
		fAccGetDisplayOutputCount = NULL;
		fAccGetDisplayOutputs = NULL;
		fAccGetDisplayOutputModes = NULL;
		fAccSetDisplayLayout = NULL;
		fAccSetDisplayChangePort = NULL;
		fAccGetDisplayOutputRetraceSemaphore = NULL;
	}

	// Monitors coming and going: the accelerant writes to this port, and the
	// thread turns that into a screen change for the desktop to deal with.
	if (fAccSetDisplayChangePort != NULL && fDisplayChangeThread < 0) {
		fDisplayChangePort = create_port(8, "display change");
		if (fDisplayChangePort >= 0) {
			fDisplayChangeThread = spawn_thread(_DisplayChangeThread,
				"display change", B_DISPLAY_PRIORITY, this);
			if (fDisplayChangeThread >= 0) {
				resume_thread(fDisplayChangeThread);
				fAccSetDisplayChangePort(fDisplayChangePort, 'dchg');
			}
		}
	}

	return B_OK;
}


/*static*/ status_t
AccelerantHWInterface::_DisplayChangeThread(void* data)
{
	AccelerantHWInterface* interface = (AccelerantHWInterface*)data;

	while (true) {
		int32 code;
		ssize_t bytes = read_port(interface->fDisplayChangePort, &code, NULL, 0);
		if (bytes == B_BAD_PORT_ID || bytes == B_INTERRUPTED)
			break;
		if (bytes < B_OK || code != 'dchg')
			continue;

		// Let a burst of events (one per connector) settle into one change.
		snooze(200000);
		while (port_count(interface->fDisplayChangePort) > 0)
			read_port_etc(interface->fDisplayChangePort, &code, NULL, 0,
				B_RELATIVE_TIMEOUT, 0);

		interface->_NotifyScreenChanged();
	}

	return B_OK;
}


/*static*/ status_t
AccelerantHWInterface::_DisplayRestoreThread(void* data)
{
	AccelerantHWInterface* interface = (AccelerantHWInterface*)data;

	while (!interface->fQuitDisplayRestoreThread) {
		if (interface->fAccWaitForDisplayRestore(500000) != B_OK)
			continue;

		// The screen contents were lost, have everything redrawn.
		interface->_NotifyScreenChanged();
	}

	return B_OK;
}


void
AccelerantHWInterface::_UpdateHooksAfterModeChange()
{
	// overlay
	fAccOverlayCount = (overlay_count)fAccelerantHook(B_OVERLAY_COUNT, NULL);
	fAccOverlaySupportedSpaces = (overlay_supported_spaces)fAccelerantHook(
		B_OVERLAY_SUPPORTED_SPACES, NULL);
	fAccOverlaySupportedFeatures = (overlay_supported_features)fAccelerantHook(
		B_OVERLAY_SUPPORTED_FEATURES, NULL);
	fAccAllocateOverlayBuffer = (allocate_overlay_buffer)fAccelerantHook(
		B_ALLOCATE_OVERLAY_BUFFER, NULL);
	fAccReleaseOverlayBuffer = (release_overlay_buffer)fAccelerantHook(
		B_RELEASE_OVERLAY_BUFFER, NULL);
	fAccGetOverlayConstraints = (get_overlay_constraints)fAccelerantHook(
		B_GET_OVERLAY_CONSTRAINTS, NULL);
	fAccAllocateOverlay
		= (allocate_overlay)fAccelerantHook(B_ALLOCATE_OVERLAY, NULL);
	fAccReleaseOverlay
		= (release_overlay)fAccelerantHook(B_RELEASE_OVERLAY, NULL);
	fAccConfigureOverlay
		= (configure_overlay)fAccelerantHook(B_CONFIGURE_OVERLAY, NULL);
}


status_t
AccelerantHWInterface::Shutdown()
{
	_StopFlushing();

	if (fDisplayChangeThread >= 0) {
		if (fAccSetDisplayChangePort != NULL)
			fAccSetDisplayChangePort(-1, 0);
		delete_port(fDisplayChangePort);
		fDisplayChangePort = -1;
		status_t result;
		wait_for_thread(fDisplayChangeThread, &result);
		fDisplayChangeThread = -1;
	}

	if (fDisplayRestoreThread >= 0) {
		fQuitDisplayRestoreThread = true;
		status_t result;
		wait_for_thread(fDisplayRestoreThread, &result);
		fDisplayRestoreThread = -1;
	}

	if (fAccelerantHook != NULL) {
		uninit_accelerant uninitAccelerant
			= (uninit_accelerant)fAccelerantHook(B_UNINIT_ACCELERANT, NULL);
		if (uninitAccelerant != NULL)
			uninitAccelerant();

		fAccelerantHook = NULL;
		fRetraceSemaphore = -1;
	}

	if (fAccelerantImage >= 0) {
		unload_add_on(fAccelerantImage);
		fAccelerantImage = -1;
	}

	if (fCardFD >= 0) {
		close(fCardFD);
		fCardFD = -1;
	}

	return B_OK;
}


/*!	Finds the mode in the mode list that is closest to the mode specified.
	As long as the mode list is not empty, this method will always succeed.
*/
status_t
AccelerantHWInterface::_FindBestMode(const display_mode& compareMode,
	float compareAspectRatio, display_mode& modeFound, int32 *_diff) const
{
	int32 bestDiff = 0;
	int32 bestIndex = -1;
	for (int32 i = 0; i < fModeCount; i++) {
		display_mode& mode = fModeList[i];
		float aspectRatio = 0;

		if (compareAspectRatio != 0 && mode.timing.v_display != 0)
			aspectRatio = mode.timing.h_display / mode.timing.v_display;

		// compute some random equality score
		// TODO: check if these scores make sense
		int32 diff
			= 1000 * abs(mode.timing.h_display - compareMode.timing.h_display)
			+ 1000 * abs(mode.timing.v_display - compareMode.timing.v_display)
			+ abs(mode.timing.h_total * mode.timing.v_total
					- compareMode.timing.h_total * compareMode.timing.v_total)
				/ 100
			+ abs((int)(mode.timing.pixel_clock - compareMode.timing.pixel_clock))
				/ 100
			+ (int32)(500 * fabs(aspectRatio - compareAspectRatio))
			+ 100 * abs((int)(mode.space - compareMode.space));

		if (bestIndex == -1 || diff < bestDiff) {
			bestDiff = diff;
			bestIndex = i;
		}
	}

	if (bestIndex < 0)
		return B_ERROR;

	modeFound = fModeList[bestIndex];
	if (_diff != 0)
		*_diff = bestDiff;

	return B_OK;
}


/*!	This method is used for the initial mode set only - because that one
	should really not fail.

	Basically we try to set all modes as found in the mode list the driver
	returned, but we start with the one that best fits the originally
	desired mode.

	The mode list must have been retrieved already.
*/
status_t
AccelerantHWInterface::_SetFallbackMode(display_mode& newMode) const
{
	// At first, we search the closest display mode from the list of
	// supported modes - if that fails, we just take one

	if (_FindBestMode(newMode, 0, newMode) == B_OK
		&& fAccSetDisplayMode(&newMode) == B_OK) {
		return B_OK;
	}

	// That failed as well, this looks like a bug in the graphics
	// driver, but we have to try to be as forgiving as possible
	// here - just take the first mode that works!

	for (int32 i = 0; i < fModeCount; i++) {
		newMode = fModeList[i];
		if (fAccSetDisplayMode(&newMode) == B_OK)
			return B_OK;
	}

	// Well, we tried.
	return B_ERROR;
}


status_t
AccelerantHWInterface::SetMode(const display_mode& mode)
{
	status_t status = _SetMode(mode, false);
	// A mode set may leave the display engine without the cursor.
	_UpdateHardwareCursor(true);
	return status;
}


/*!	\a force sets the mode again even when it is the current one, which is
	what a changed display layout needs: the frame buffer is the same size,
	but the monitors behind it are arranged differently and the accelerant
	allocates a new one.
*/
status_t
AccelerantHWInterface::_SetMode(const display_mode& mode, bool force)
{
	AutoWriteLocker _(this);
	// TODO: There are places this function can fail,
	// maybe it needs to roll back changes in case of an
	// error.

	// prevent from doing the unnecessary
	if (!force && fModeCount > 0 && fFrontBuffer.IsSet()
		&& fDisplayMode == mode) {
		// TODO: better comparison of display modes
		return B_OK;
	}

	// some safety checks
	// TODO: more of those!
	if (!_IsValidMode(mode))
		return B_BAD_VALUE;

	if (!fFrontBuffer.IsSet())
		return B_NO_INIT;

	// just try to set the mode - we let the graphics driver
	// approve or deny the request, as it should know best

	display_mode newMode = mode;

	status_t status = B_ERROR;
	if (!use_fail_safe_video_mode() || !fInitialModeSwitch)
		status = fAccSetDisplayMode(&newMode);
	if (status != B_OK) {
		ATRACE(("setting display mode failed\n"));
		if (!fInitialModeSwitch) {
			// The old mode stays, but the accelerant may have had to map
			// its frame buffer anew while trying.
			_UpdateFrameBufferConfig();
			return status;
		}

		if (fModeList == NULL) {
			status = _UpdateModeList();
			if (status != B_OK)
				return status;
		}

		// If this is the initial mode switch, we try a number of fallback
		// modes first, before we have to fail

		status = use_fail_safe_video_mode()
			? B_ERROR : _SetFallbackMode(newMode);
		if (status != B_OK) {
			// The driver doesn't allow us the mode switch - this usually
			// means we have a driver that doesn't allow mode switches at
			// all.
			// All we can do now is to ask the driver which mode we can
			// use - this is always necessary for VESA mode, for example.
			if (fAccGetDisplayMode(&newMode) != B_OK)
				return B_ERROR;

			// TODO: check if the mode returned is valid!
			if (!_IsValidMode(newMode))
				return B_BAD_DATA;

			// TODO: if the mode switch before fails as well, we must forbid
			//	any uses of this class!
			status = B_OK;
		}
	}

	fDisplayMode = newMode;
	fInitialModeSwitch = false;

	// update frontbuffer
	fFrontBuffer->SetDisplayMode(fDisplayMode);
	if (_UpdateFrameBufferConfig() != B_OK) {
		// TODO: if this fails, we're basically toasted - we need to handle this
		//	differently to crashing later on!
		return B_ERROR;
	}

	// Update the frame buffer used by the on-screen KDL
#ifndef HAIKU_TARGET_PLATFORM_LIBBE_TEST
	uint32 depth = (fFrameBufferConfig.bytes_per_row
		/ fFrontBuffer->Width()) << 3;
	if (fDisplayMode.space == B_RGB15)
		depth = 15;

	_kern_frame_buffer_update((addr_t)fFrameBufferConfig.frame_buffer,
		fFrontBuffer->Width(), fFrontBuffer->Height(),
		depth, fFrameBufferConfig.bytes_per_row);
#endif

	_UpdateHooksAfterModeChange();

	// update backbuffer if necessary
	status = _UpdateBackBuffer();
	if (status != B_OK)
		return status;

	// the outputs may show other parts of it now
	_UpdateFlushTargets();

	// update color palette configuration if necessary
	if (fDisplayMode.space == B_CMAP8)
		_SetSystemPalette();
	else if (fDisplayMode.space == B_GRAY8)
		_SetGrayscalePalette();

	// notify all listeners about the mode change
	_NotifyFrameBufferChanged();

	return status;
}


void
AccelerantHWInterface::GetMode(display_mode* mode)
{
	if (mode && LockParallelAccess()) {
		*mode = fDisplayMode;
		UnlockParallelAccess();
	}
}


status_t
AccelerantHWInterface::_UpdateModeList()
{
	fModeCount = fAccGetModeCount();
	if (fModeCount <= 0)
		return B_ERROR;

	delete[] fModeList;
	fModeList = new(nothrow) display_mode[fModeCount];
	if (!fModeList)
		return B_NO_MEMORY;

	if (fAccGetModeList(fModeList) != B_OK) {
		ATRACE(("unable to get mode list\n"));
		return B_ERROR;
	}

	return B_OK;
}


status_t
AccelerantHWInterface::_UpdateFrameBufferConfig()
{
	if (fAccGetFrameBufferConfig(&fFrameBufferConfig) != B_OK) {
		ATRACE(("unable to get frame buffer config\n"));
		return B_ERROR;
	}

	fFrontBuffer->SetFrameBufferConfig(fFrameBufferConfig);

	return B_OK;
}


status_t
AccelerantHWInterface::GetDeviceInfo(accelerant_device_info* info)
{
	get_accelerant_device_info GetAccelerantDeviceInfo
		= (get_accelerant_device_info)fAccelerantHook(
			B_GET_ACCELERANT_DEVICE_INFO, NULL);
	if (!GetAccelerantDeviceInfo) {
		ATRACE(("No B_GET_ACCELERANT_DEVICE_INFO hook found\n"));
		return B_UNSUPPORTED;
	}

	return GetAccelerantDeviceInfo(info);
}


status_t
AccelerantHWInterface::GetModeList(display_mode** _modes, uint32* _count)
{
	AutoReadLocker _(this);

	if (_count == NULL || _modes == NULL)
		return B_BAD_VALUE;

	status_t status = B_OK;

	if (fModeList == NULL)
		status = _UpdateModeList();

	if (status >= B_OK) {
		*_modes = new(nothrow) display_mode[fModeCount];
		if (*_modes) {
			*_count = fModeCount;
			memcpy(*_modes, fModeList, sizeof(display_mode) * fModeCount);
		} else {
			*_count = 0;
			status = B_NO_MEMORY;
		}
	}
	return status;
}


status_t
AccelerantHWInterface::GetPixelClockLimits(display_mode *mode, uint32* _low,
	uint32* _high)
{
	if (mode == NULL || _low == NULL || _high == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);
	return fAccGetPixelClockLimits(mode, _low, _high);
}


status_t
AccelerantHWInterface::GetTimingConstraints(
	display_timing_constraints* constraints)
{
	if (constraints == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);

	if (fAccGetTimingConstraints)
		return fAccGetTimingConstraints(constraints);

	return B_UNSUPPORTED;
}


status_t
AccelerantHWInterface::ProposeMode(display_mode* candidate,
	const display_mode* _low, const display_mode* _high)
{
	if (candidate == NULL || _low == NULL || _high == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);

	if (fAccProposeDisplayMode == NULL)
		return B_UNSUPPORTED;

	// avoid const issues
	display_mode high, low;
	high = *_high;
	low = *_low;

	return fAccProposeDisplayMode(candidate, &low, &high);
}


status_t
AccelerantHWInterface::GetPreferredMode(display_mode* preferredMode)
{
	status_t status = B_NOT_SUPPORTED;

	if (fAccGetPreferredDisplayMode != NULL) {
		status = fAccGetPreferredDisplayMode(preferredMode);
		if (status == B_OK)
			return B_OK;
	}

	if (fAccGetEDIDInfo != NULL) {
		edid1_info info;
		uint32 version;
		status = fAccGetEDIDInfo(&info, sizeof(info), &version);
		if (status < B_OK)
			return status;
		if (version != EDID_VERSION_1)
			return B_NOT_SUPPORTED;

		if (fModeList == NULL) {
			status = _UpdateModeList();
			if (status != B_OK)
				return status;
		}

		status = B_NOT_SUPPORTED;
		display_mode bestMode;
		int32 bestDiff = INT_MAX;

		// find preferred mode from EDID info
		for (uint32 i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; ++i) {
			if (info.detailed_monitor[i].monitor_desc_type
					!= EDID1_IS_DETAILED_TIMING)
				continue;

			// construct basic mode and find it in the mode list
			const edid1_detailed_timing& timing
				= info.detailed_monitor[i].data.detailed_timing;
			if (timing.h_active < 640 || timing.v_active < 350)
				continue;

			float aspectRatio = 0.0f;
			if (timing.h_size > 0 && timing.v_size > 0)
				aspectRatio = 1.0f * timing.h_size / timing.v_size;

			display_mode modeFound;
			display_mode mode;

			mode.timing.pixel_clock = timing.pixel_clock * 10;
			mode.timing.h_display = timing.h_active;
			mode.timing.h_sync_start = timing.h_active + timing.h_sync_off;
			mode.timing.h_sync_end = mode.timing.h_sync_start
				+ timing.h_sync_width;
			mode.timing.h_total = timing.h_active + timing.h_blank;
			mode.timing.v_display = timing.v_active;
			mode.timing.v_sync_start = timing.v_active + timing.v_sync_off;
			mode.timing.v_sync_end = mode.timing.v_sync_start
				+ timing.v_sync_width;
			mode.timing.v_total = timing.v_active + timing.v_blank;

			mode.space = B_RGB32;
			mode.virtual_width = mode.timing.h_display;
			mode.virtual_height = mode.timing.v_display;

			// TODO: eventually ignore detailed modes for the preferred one
			// if there are more than one usable?
			int32 diff;
			if (_FindBestMode(mode, aspectRatio, modeFound, &diff) == B_OK) {
				status = B_OK;
				if (diff < bestDiff) {
					bestMode = modeFound;
					bestDiff = diff;
				}
			}
		}

		if (status == B_OK)
			*preferredMode = bestMode;
	}

	return status;
}


status_t
AccelerantHWInterface::GetMonitorInfo(monitor_info* info)
{
	status_t status = B_NOT_SUPPORTED;

	if (fAccGetMonitorInfo != NULL) {
		status = fAccGetMonitorInfo(info);
		if (status == B_OK)
			return B_OK;
	}

	if (fAccGetEDIDInfo == NULL)
		return status;

	edid1_info edid;
	uint32 version;
	status = fAccGetEDIDInfo(&edid, sizeof(edid), &version);
	if (status < B_OK)
		return status;
	if (version != EDID_VERSION_1)
		return B_NOT_SUPPORTED;

	memset(info, 0, sizeof(monitor_info));
	strlcpy(info->vendor, edid.vendor.manufacturer, sizeof(info->vendor));
	if (edid.vendor.serial != 0) {
		snprintf(info->serial_number, sizeof(info->serial_number), "%" B_PRIu32,
			edid.vendor.serial);
	}
	info->product_id = edid.vendor.prod_id;
	info->produced.week = edid.vendor.week;
	info->produced.year = edid.vendor.year;
	info->width = edid.display.h_size;
	info->height = edid.display.v_size;

	for (uint32 i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; ++i) {
		edid1_detailed_monitor *monitor = &edid.detailed_monitor[i];

		switch (monitor->monitor_desc_type) {
			case EDID1_SERIAL_NUMBER:
				strlcpy(info->serial_number, monitor->data.serial_number,
					sizeof(info->serial_number));
				break;

			case EDID1_MONITOR_NAME:
				// There can be several of these; in this case we'll just
				// overwrite the previous entries
				// TODO: we could append them as well
				strlcpy(info->name, monitor->data.monitor_name,
					sizeof(info->name));
				break;

			case EDID1_MONITOR_RANGES:
			{
				edid1_monitor_range& range = monitor->data.monitor_range;

				info->min_horizontal_frequency = range.min_h;
				info->max_horizontal_frequency = range.max_h;
				info->min_vertical_frequency = range.min_v;
				info->max_vertical_frequency = range.max_v;
				info->max_pixel_clock = range.max_clock * 10000;
				break;
			}

			case EDID1_IS_DETAILED_TIMING:
			{
				edid1_detailed_timing& timing = monitor->data.detailed_timing;
				info->width = timing.h_size / 10.0;
				info->height = timing.v_size / 10.0;
			}

			default:
				break;
		}
	}

	return B_OK;
}


/*!	Asked for with the interface read locked (WaitForRetraceBeforeCopy()),
	so this must not lock it: the accelerant is asked once, and when two
	threads happen to ask at the same time, both get the same answer.
*/
sem_id
AccelerantHWInterface::RetraceSemaphore()
{
	sem_id semaphore = atomic_get(&fRetraceSemaphore);
	if (semaphore != -1)
		return semaphore;
	if (fAccelerantHook == NULL)
		return B_UNSUPPORTED;

	accelerant_retrace_semaphore AccelerantRetraceSemaphore =
		(accelerant_retrace_semaphore)fAccelerantHook(
			B_ACCELERANT_RETRACE_SEMAPHORE, NULL);
	semaphore = AccelerantRetraceSemaphore != NULL
		? AccelerantRetraceSemaphore() : B_UNSUPPORTED;
	if (semaphore < 0) {
		// also -1, so that an accelerant without one is not asked again
		semaphore = B_UNSUPPORTED;
	}

	atomic_test_and_set(&fRetraceSemaphore, semaphore, -1);
	return atomic_get(&fRetraceSemaphore);
}


sem_id
AccelerantHWInterface::OutputRetraceSemaphore(uint32 id)
{
	if (fAccGetDisplayOutputRetraceSemaphore == NULL)
		return B_UNSUPPORTED;
	sem_id semaphore = fAccGetDisplayOutputRetraceSemaphore(id);
	return semaphore < 0 ? B_UNSUPPORTED : semaphore;
}


status_t
AccelerantHWInterface::WaitForRetrace(bigtime_t timeout)
{
	sem_id sem = RetraceSemaphore();
	if (sem < 0)
		return sem;

	return acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, timeout);
}


status_t
AccelerantHWInterface::SetDPMSMode(uint32 state)
{
	AutoWriteLocker _(this);

	if (!fAccSetDPMSMode)
		return B_UNSUPPORTED;

	return fAccSetDPMSMode(state);
}


uint32
AccelerantHWInterface::DPMSMode()
{
	AutoReadLocker _(this);

	if (!fAccDPMSMode)
		return B_UNSUPPORTED;

	return fAccDPMSMode();
}


uint32
AccelerantHWInterface::DPMSCapabilities()
{
	AutoReadLocker _(this);

	if (!fAccDPMSCapabilities)
		return B_UNSUPPORTED;

	return fAccDPMSCapabilities();
}


status_t
AccelerantHWInterface::SetBrightness(float brightness)
{
	AutoReadLocker _(this);

	if (!fAccSetBrightness)
		return B_UNSUPPORTED;

	return fAccSetBrightness(brightness);
}


status_t
AccelerantHWInterface::GetBrightness(float* brightness)
{
	AutoReadLocker _(this);

	if (!fAccGetBrightness)
		return B_UNSUPPORTED;

	return fAccGetBrightness(brightness);
}


status_t
AccelerantHWInterface::GetAccelerantPath(BString& string)
{
	image_info info;
	status_t status = get_image_info(fAccelerantImage, &info);
	if (status == B_OK)
		string = info.name;
	return status;
}


status_t
AccelerantHWInterface::GetDriverPath(BString& string)
{
	// TODO: this currently assumes that the accelerant's clone info
	//	is always the path name of its driver (that's the case for
	//	all of our drivers)
	char path[B_PATH_NAME_LENGTH];
	get_accelerant_clone_info getCloneInfo;
	getCloneInfo = (get_accelerant_clone_info)fAccelerantHook(
		B_GET_ACCELERANT_CLONE_INFO, NULL);

	if (getCloneInfo == NULL)
		return B_NOT_SUPPORTED;

	getCloneInfo((void*)path);
	string.SetTo(path);
	return B_OK;
}


/*!	The back buffer has the logical size: the front buffer's, reduced by
	the software scale. It is always B_RGBA32, which simplifies the drawing
	backend tremendously; the color space conversion (and the scaling) is
	handled in CopyBackToFront().
*/
status_t
AccelerantHWInterface::_UpdateBackBuffer()
{
	int32 width = BackBufferWidth();
	int32 height = BackBufferHeight();
	if (fBackBuffer.IsSet() && fBackBuffer->Width() == (uint32)width
		&& fBackBuffer->Height() == (uint32)height
		&& fFrontBuffer->ColorSpace() != B_RGB32)
		return B_OK;

	fBackBuffer.Unset();
	fBackBuffer.SetTo(new(nothrow) MallocBuffer(width, height));

	status_t status = fBackBuffer.IsSet()
		? fBackBuffer->InitCheck() : B_NO_MEMORY;
	if (status < B_OK) {
		fBackBuffer.Unset();
		return status;
	}
	// clear out backbuffer, alpha is 255 this way
	memset(fBackBuffer->Bits(), 255, fBackBuffer->BitsLength());
	return B_OK;
}


status_t
AccelerantHWInterface::SetRenderScale(uint16 percent)
{
	AutoWriteLocker _(this);

	if (percent == RenderScale())
		return B_OK;
	status_t status = HWInterface::SetRenderScale(percent);
	if (status != B_OK)
		return status;
	if (!fFrontBuffer.IsSet())
		return B_OK;

	status = _UpdateBackBuffer();
	if (status != B_OK)
		return status;

	_NotifyFrameBufferChanged();
	// the cursor at the new density
	_UpdateHardwareCursor(true);
	return B_OK;
}


status_t
AccelerantHWInterface::SetSoftwareScale(uint16 percent)
{
	AutoWriteLocker _(this);

	if (percent == SoftwareScale())
		return B_OK;
	if (!fFrontBuffer.IsSet())
		return B_NO_INIT;
	switch (fFrontBuffer->ColorSpace()) {
		case B_RGB32:
		case B_RGBA32:
		case B_RGB30:
		case B_RGB24:
		case B_RGB16:
		case B_RGB15:
		case B_RGBA15:
		case B_CMAP8:
			break;
		default:
			return B_UNSUPPORTED;
	}

	status_t status = HWInterface::SetSoftwareScale(percent);
	if (status != B_OK)
		return status;

	status = _UpdateBackBuffer();
	if (status != B_OK)
		return status;

	_NotifyFrameBufferChanged();
	// the hardware cursor is not enlarged with the frame buffer
	_UpdateHardwareCursor(true);
	return B_OK;
}


// #pragma mark - display layout


bool
AccelerantHWInterface::HasDisplayLayout() const
{
	return fAccSetDisplayLayout != NULL;
}


status_t
AccelerantHWInterface::GetDisplayOutputs(display_output** _outputs,
	uint32* _count)
{
	AutoReadLocker _(this);

	if (fAccGetDisplayOutputs == NULL)
		return B_UNSUPPORTED;

	uint32 count = fAccGetDisplayOutputCount();
	if (count == 0)
		return B_ENTRY_NOT_FOUND;

	display_output* outputs = (display_output*)malloc(
		count * sizeof(display_output));
	if (outputs == NULL)
		return B_NO_MEMORY;

	status_t status = fAccGetDisplayOutputs(outputs, &count);
	if (status != B_OK) {
		free(outputs);
		return status;
	}

	*_outputs = outputs;
	*_count = count;
	return B_OK;
}


status_t
AccelerantHWInterface::GetDisplayOutputModes(uint32 id, display_mode** _modes,
	uint32* _count)
{
	AutoReadLocker _(this);

	if (fAccGetDisplayOutputModes == NULL)
		return B_UNSUPPORTED;

	// Monitors list a few dozen timings at most.
	uint32 count = 256;
	display_mode* modes = (display_mode*)malloc(count * sizeof(display_mode));
	if (modes == NULL)
		return B_NO_MEMORY;

	status_t status = fAccGetDisplayOutputModes(id, modes, &count);
	if (status != B_OK) {
		free(modes);
		return status;
	}

	*_modes = modes;
	*_count = count;
	return B_OK;
}


status_t
AccelerantHWInterface::SetDisplayLayout(const display_output_config* configs,
	uint32 count, bool switchMode)
{
	if (fAccSetDisplayLayout == NULL)
		return B_UNSUPPORTED;

	display_mode mode;
	status_t status;
	{
		AutoWriteLocker _(this);
		status = fAccSetDisplayLayout(configs, count, &mode);
		// The mode list changed with the layout.
		delete[] fModeList;
		fModeList = NULL;
		fModeCount = 0;
		if (status == B_OK) {
			// Every output of a layout is drawn at the same density, and
			// the logical size is the rectangle around the enabled outputs.
			// A mirror shows its source's part, whatever its own size.
			uint16 renderScale = 100;
			int32 right = 0, bottom = 0;
			for (uint32 i = 0; i < count; i++) {
				if (configs[i].render_scale > renderScale)
					renderScale = configs[i].render_scale;
				if ((configs[i].flags & B_DISPLAY_OUTPUT_ENABLED) == 0
					|| (configs[i].flags & B_DISPLAY_OUTPUT_MIRROR) != 0)
					continue;
				uint16 scale = configs[i].scale == 0 ? 100 : configs[i].scale;
				int32 width = (configs[i].timing.h_display * 100 + scale / 2)
					/ scale;
				int32 height = (configs[i].timing.v_display * 100 + scale / 2)
					/ scale;
				if (configs[i].x + width > right)
					right = configs[i].x + width;
				if (configs[i].y + height > bottom)
					bottom = configs[i].y + height;
			}
			SetLogicalSize(right, bottom);
			HWInterface::SetRenderScale(renderScale);
		}
	}
	if (status != B_OK || !switchMode)
		return status;

	status = _SetMode(mode, true);
	// the cursor on the new layout, at its density
	_UpdateHardwareCursor(true);
	return status;
}


// #pragma mark - overlays


overlay_token
AccelerantHWInterface::AcquireOverlayChannel()
{
	if (fAccAllocateOverlay == NULL
		|| fAccReleaseOverlay == NULL)
		return NULL;

	// The current display mode only matters at the time we're planning on
	// showing the overlay channel on screen - that's why we can't use
	// the B_OVERLAY_COUNT hook.
	// TODO: remove fAccOverlayCount if we're not going to need it at all.

	return fAccAllocateOverlay();
}


void
AccelerantHWInterface::ReleaseOverlayChannel(overlay_token token)
{
	if (token == NULL)
		return;

	fAccReleaseOverlay(token);
}


status_t
AccelerantHWInterface::GetOverlayRestrictions(const Overlay* overlay,
	overlay_restrictions* restrictions)
{
	if (overlay == NULL || restrictions == NULL)
		return B_BAD_VALUE;
	if (fAccGetOverlayConstraints == NULL)
		return B_NOT_SUPPORTED;

	overlay_constraints constraints;
	status_t status = fAccGetOverlayConstraints(&fDisplayMode,
		overlay->OverlayBuffer(), &constraints);
	if (status < B_OK)
		return status;

	memset(restrictions, 0, sizeof(overlay_restrictions));
	memcpy(&restrictions->source, &constraints.view, sizeof(overlay_limits));
	memcpy(&restrictions->destination, &constraints.window,
		sizeof(overlay_limits));
	restrictions->min_width_scale = constraints.h_scale.min;
	restrictions->max_width_scale = constraints.h_scale.max;
	restrictions->min_height_scale = constraints.v_scale.min;
	restrictions->max_height_scale = constraints.v_scale.max;

	return B_OK;
}


bool
AccelerantHWInterface::CheckOverlayRestrictions(int32 width, int32 height,
	color_space colorSpace)
{
	if (fAccOverlaySupportedSpaces == NULL
		|| fAccGetOverlayConstraints == NULL
		|| fAccAllocateOverlayBuffer == NULL
		|| fAccReleaseOverlayBuffer == NULL)
		return false;

	// Note: we can't really check the size of the overlay upfront - we
	// must assume fAccAllocateOverlayBuffer() will fail in that case.
	if (width < 0 || width > 65535 || height < 0 || height > 65535)
		return false;

	// check color space

	const uint32* spaces = fAccOverlaySupportedSpaces(&fDisplayMode);
	if (spaces == NULL)
		return false;

	for (int32 i = 0; spaces[i] != 0; i++) {
		if (spaces[i] == (uint32)colorSpace)
			return true;
	}

	return false;
}


const overlay_buffer*
AccelerantHWInterface::AllocateOverlayBuffer(int32 width, int32 height,
	color_space space)
{
	if (fAccAllocateOverlayBuffer == NULL)
		return NULL;

	return fAccAllocateOverlayBuffer(space, width, height);
}


void
AccelerantHWInterface::FreeOverlayBuffer(const overlay_buffer* buffer)
{
	if (buffer == NULL || fAccReleaseOverlayBuffer == NULL)
		return;

	fAccReleaseOverlayBuffer(buffer);
}


void
AccelerantHWInterface::ConfigureOverlay(Overlay* overlay)
{
	// TODO: this only needs to be done on mode changes!
	overlay->SetColorSpace(fDisplayMode.space);

	fAccConfigureOverlay(overlay->OverlayToken(), overlay->OverlayBuffer(),
		overlay->OverlayWindow(), overlay->OverlayView());
}


void
AccelerantHWInterface::HideOverlay(Overlay* overlay)
{
	fAccConfigureOverlay(overlay->OverlayToken(), overlay->OverlayBuffer(),
		NULL, NULL);
}


// #pragma mark - cursor


void
AccelerantHWInterface::SetCursor(ServerCursor* cursor)
{
	// cursor should never be NULL, but let us be safe!!
	if (cursor == NULL)
		return;

	// HWInterface makes the cursor at the frame buffer's density first, so
	// that the accelerant gets the one the software cursor would draw.
	HWInterface::SetCursor(cursor);
	_UpdateHardwareCursor();
}


void
AccelerantHWInterface::SetDragBitmap(const ServerBitmap* bitmap,
	const BPoint& offsetFromCursor)
{
	// The drag bitmap is composed into the cursor; the accelerant shows the
	// two together, or the software cursor does while it is too big.
	HWInterface::SetDragBitmap(bitmap, offsetFromCursor);
	_UpdateHardwareCursor();
}


/*!	Hands the cursor, as HWInterface keeps it (at the frame buffer's density,
	the drag bitmap composed into it), to the accelerant, and switches between
	the hardware and the software cursor as the accelerant takes it or not.

	The software cursor is drawn into the frame buffer, out of app_server's
	own copy of the screen. A direct window that draws into the frame buffer
	itself - a browser presenting its pages from the GPU - paints over it and
	leaves app_server restoring stale pixels around it; a hardware cursor is
	not in the frame buffer at all.
*/
void
AccelerantHWInterface::_UpdateHardwareCursor(bool force)
{
	ServerCursorReference cursor = CursorAndDragBitmap();
	if (cursor.Get() == NULL || !LockExclusiveAccess())
		return;

	// The accelerant makes a surface of every one it gets.
	if (!force && fHardwareCursorEnabled && fHardwareCursor.Get() == cursor.Get()) {
		UnlockExclusiveAccess();
		return;
	}

	bool cursorSet = false;

	// A software scale enlarges the frame buffer after the cursor is drawn.
	if (fAccSetCursorBitmap != NULL && SoftwareScale() == 100
		&& (cursor->ColorSpace() == B_RGBA32
			|| cursor->ColorSpace() == B_RGB32)) {
		// Both are premultiplied with alpha, as the software cursor blends
		// them.
		uint16 xHotSpot = (uint16)cursor->GetHotSpot().x;
		uint16 yHotSpot = (uint16)cursor->GetHotSpot().y;
		cursorSet = fAccSetCursorBitmap((uint16)cursor->Width(),
			(uint16)cursor->Height(), xHotSpot, yHotSpot, B_RGBA32,
			(uint16)cursor->BytesPerRow(), cursor->Bits()) == B_OK;
	}

	// Only a cursor as it was set has its BeOS data: one at the frame buffer's
	// density already, without a drag bitmap.
	if (!cursorSet && cursor->CursorData() != NULL && fAccSetCursorShape != NULL
		&& SoftwareScale() == 100) {
		// BeOS BCursor, 16x16 monochrome
		uint8 size = cursor->CursorData()[0];
		// CursorData()[1] is color depth (always monochrome)
		// x and y are switched
		uint8 xHotSpot = cursor->CursorData()[3];
		uint8 yHotSpot = cursor->CursorData()[2];

		// Create pointers to the cursor and/xor bit arrays
		// for the BeOS BCursor there are two 32 byte, 16x16 bit arrays
		// in the first:  1 is black,  0 is white
		// in the second: 1 is opaque, 0 is transparent
		// 1st	2nd
		//  0	 0	 transparent
		//  0	 1	 white
		//  1	 0	 transparent
		//  1	 1	 black
		// for the HW cursor the first is ANDed and the second is XORed
		// AND	XOR
		//  0	 0	 white
		//  0	 1	 black
		//  1	 0	 transparent
		//  1	 1	 reverse
		// so, the first 32 bytes are the XOR mask
		const uint8* xorMask = cursor->CursorData() + 4;
		// the second 32 bytes *NOTed* are the AND mask
		// TODO maybe this should be NOTed when copied to the ServerCursor
		uint8 andMask[32];
		const uint8* transMask = cursor->CursorData() + 36;
		for (int32 i = 0; i < 32; i++)
			andMask[i] = ~transMask[i];

		// Time to talk to the accelerant!
		cursorSet = fAccSetCursorShape(size, size, xHotSpot,
			yHotSpot, andMask, xorMask) == B_OK;
	}

	if (cursorSet && !fHardwareCursorEnabled) {
		// we switched from SW to HW, so we need to erase the SW cursor
		if (fCursorVisible && fFloatingOverlaysLock.Lock()) {
			IntRect r = _CursorFrame();
			fCursorVisible = false;
				// so the Invalidate doesn't draw it again
			_RestoreCursorArea();
			Invalidate(r);
			fCursorVisible = true;
			fFloatingOverlaysLock.Unlock();
		}
	}

	if (cursorSet && fAccMoveCursor != NULL) {
		// the hardware cursor lives in the front buffer's pixels
		fAccMoveCursor((uint16)(fCursorLocation.x * RenderScaleFactor()
				* SoftwareScale() / 100),
			(uint16)(fCursorLocation.y * RenderScaleFactor()
				* SoftwareScale() / 100));
	}

	if (fAccShowCursor != NULL)
		fAccShowCursor(cursorSet && fCursorVisible);

	bool wasEnabled = fHardwareCursorEnabled;
	fHardwareCursorEnabled = cursorSet;
	fHardwareCursor.SetTo(cursorSet ? cursor.Get() : NULL);

	if (wasEnabled && !cursorSet) {
		// and from HW to SW: the software cursor is drawn again
		if (fFloatingOverlaysLock.Lock()) {
			Invalidate(_CursorFrame());
			fFloatingOverlaysLock.Unlock();
		}
	}

	UnlockExclusiveAccess();
}


void
AccelerantHWInterface::SetCursorVisible(bool visible)
{
	HWInterface::SetCursorVisible(visible);

	if (fHardwareCursorEnabled && LockExclusiveAccess()) {
		if (fAccShowCursor != NULL)
				fAccShowCursor(visible);
		else
			fHardwareCursorEnabled = false;

		UnlockExclusiveAccess();
	}
}


void
AccelerantHWInterface::MoveCursorTo(float x, float y)
{
	HWInterface::MoveCursorTo(x, y);

	if (fHardwareCursorEnabled && LockExclusiveAccess()) {
		if (fAccMoveCursor != NULL) {
			// the hardware cursor lives in the front buffer's pixels
			fAccMoveCursor((uint16)(x * RenderScaleFactor() * SoftwareScale() / 100),
				(uint16)(y * RenderScaleFactor() * SoftwareScale() / 100));
		}
		else {
			fHardwareCursorEnabled = false;
			if (fAccShowCursor != NULL)
				fAccShowCursor(false);
		}

		UnlockExclusiveAccess();
	}
}


// #pragma mark - buffer access


RenderingBuffer*
AccelerantHWInterface::FrontBuffer() const
{
	return fFrontBuffer.Get();
}


RenderingBuffer*
AccelerantHWInterface::BackBuffer() const
{
	return fBackBuffer.Get();
}


bool
AccelerantHWInterface::IsDoubleBuffered() const
{
	return fBackBuffer.IsSet();
}


void
AccelerantHWInterface::_CopyBackToFront(/*const*/ BRegion& region)
{
	return HWInterface::_CopyBackToFront(region);
}


// #pragma mark -


void
AccelerantHWInterface::_DrawCursor(IntRect area) const
{
	if (!fHardwareCursorEnabled)
		HWInterface::_DrawCursor(area);
}


void
AccelerantHWInterface::_RegionToRectParams(/*const*/ BRegion* region,
	uint32* count) const
{
	*count = region->CountRects();
	// TODO: locking!!
	if (fRectParamsCount < *count) {
		fRectParamsCount = (*count / kDefaultParamsCount + 1)
			* kDefaultParamsCount;
		// NOTE: realloc() could be used instead...
		fill_rect_params* params
			= new (nothrow) fill_rect_params[fRectParamsCount];
		if (params) {
			delete[] fRectParams;
			fRectParams = params;
		} else {
			*count = fRectParamsCount;
		}
	}

	for (uint32 i = 0; i < *count; i++) {
		clipping_rect r = region->RectAtInt(i);
		fRectParams[i].left = (uint16)r.left;
		fRectParams[i].top = (uint16)r.top;
		fRectParams[i].right = (uint16)r.right;
		fRectParams[i].bottom = (uint16)r.bottom;
	}
}


uint32
AccelerantHWInterface::_NativeColor(const rgb_color& color) const
{
	// NOTE: This functions looks somehow suspicious to me.
	// It assumes that all graphics cards have the same native endianness, no?
	switch (fDisplayMode.space) {
		case B_CMAP8:
		case B_GRAY8:
			return RGBColor(color).GetColor8();

		case B_RGB15_BIG:
		case B_RGBA15_BIG:
		case B_RGB15_LITTLE:
		case B_RGBA15_LITTLE:
			return RGBColor(color).GetColor15();

		case B_RGB16_BIG:
		case B_RGB16_LITTLE:
			return RGBColor(color).GetColor16();

		case B_RGB32_BIG:
		case B_RGBA32_BIG:
		case B_RGB32_LITTLE:
		case B_RGBA32_LITTLE: {
			return (uint32)((color.alpha << 24) | (color.red << 16)
				| (color.green << 8) | color.blue);
		}
	}
	return 0;
}


void
AccelerantHWInterface::_SetSystemPalette()
{
	set_indexed_colors setIndexedColors = (set_indexed_colors)fAccelerantHook(
		B_SET_INDEXED_COLORS, NULL);
	if (setIndexedColors == NULL)
		return;

	const rgb_color* palette = SystemPalette();
	uint8 colors[3 * 256];
		// the color table is an array with 3 bytes per color
	uint32 j = 0;

	for (int32 i = 0; i < 256; i++) {
		colors[j++] = palette[i].red;
		colors[j++] = palette[i].green;
		colors[j++] = palette[i].blue;
	}

	setIndexedColors(256, 0, colors, 0);
}


void
AccelerantHWInterface::_SetGrayscalePalette()
{
	set_indexed_colors setIndexedColors = (set_indexed_colors)fAccelerantHook(
		B_SET_INDEXED_COLORS, NULL);
	if (setIndexedColors == NULL)
		return;

	uint8 colors[3 * 256];
		// the color table is an array with 3 bytes per color
	uint32 j = 0;

	if (fFrontBuffer->Width() > fFrontBuffer->BytesPerRow()) {
		// VGA 16 color grayscale planar mode
		for (int32 i = 0; i < 256; i++) {
			colors[j++] = (i & 0xf) * 17;
			colors[j++] = (i & 0xf) * 17;
			colors[j++] = (i & 0xf) * 17;
		}

		setIndexedColors(256, 0, colors, 0);
	} else {
		for (int32 i = 0; i < 256; i++) {
			colors[j++] = i;
			colors[j++] = i;
			colors[j++] = i;
		}

		setIndexedColors(256, 0, colors, 0);
	}
}
