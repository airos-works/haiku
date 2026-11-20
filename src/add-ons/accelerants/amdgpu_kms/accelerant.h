/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDGPU_KMS_ACCELERANT_H
#define AMDGPU_KMS_ACCELERANT_H


#include <Accelerant.h>
#include <OS.h>

#include "kms.h"


/*	The accelerant for amdgpu_drm, Linux's amdgpu and its Display Core as a
	Haiku add-on, through the kernel mode setting ioctls of its primary node
	(the /dev/graphics node is that node, the first one opening it is the
	DRM master).

	All monitors show regions of one frame buffer in VRAM, which the CPU
	writes through the PCI aperture: app_server sees one screen whose layout
	it sets with the display layout hooks, each region scaled to its
	monitor's mode by the display engine where it is not the same size.

	More than one GPU: the device app_server opened is the first card; the
	other graphics devices this accelerant serves (another GPU of the same
	or another DRM driver, /dev/graphics/nouveau_drm_1) are opened as well,
	and their monitors join the layout. Every card scans out a frame buffer
	of its own the size of the whole layout; app_server's flushes come
	through the frame buffer source hooks (multi_card.cpp), which copy what
	was drawn into every card whose monitors show it. So one desktop spans
	all the GPUs, and a window dragged from one monitor to another moves
	from one GPU to the other. */


static const uint32 kMaxOutputs = 16;
	// eDP, up to four DisplayPort or HDMI PHYs, and four USB4 (DPIA) links
	// on current AMD APUs; more on desktop GPUs
static const uint32 kMaxCards = 4;


// one GPU's display engine, through its DRM primary node
struct NouveauCopier;


struct Card {
	int					fd;
	bool				owned;			// opened here (not app_server's)
	bool				isAmdgpu;		// else another DRM driver (nouveau_drm):
										// KMS only, no device copies
	bool				isI915;			// i915_drm: copies by its blitter
										// (i915_copy.cpp)
	char				driver[32];

	Kms::Resources		resources;
	Kms::Plane			planes[Kms::kMaxObjects];
	uint32				planeCount;
	uint32				crtcPrimaryPlane[Kms::kMaxObjects];
	uint32				crtcCursorPlane[Kms::kMaxObjects];

	// properties (the same ID on every object of a type)
	uint32				connectorCrtcId;
	uint32				crtcModeId;
	uint32				crtcActive;
	uint32				planeFbId;
	uint32				planeCrtcId;
	uint32				planeSrcX, planeSrcY, planeSrcW, planeSrcH;
	uint32				planeCrtcX, planeCrtcY, planeCrtcW, planeCrtcH;

	Kms::Buffer			frameBuffer;
	uint32				modeBlobs[Kms::kMaxObjects];	// per CRTC index
	Kms::Buffer			cursor;
	uint32				cursorSize;		// DRM_CAP_CURSOR_WIDTH, at most 128

	NouveauCopier*		copier;			// nouveau's 2D engine for the flush
	bool				copierTried;
};


struct Output {
	uint32				id;				// the output's ID for app_server:
										// the connector on the first card
	uint32				card;			// index into gInfo->cards
	uint32				connector;		// DRM connector on its card
	char				name[B_DISPLAY_OUTPUT_NAME_LENGTH];
	bool				connected;
	bool				internal;		// a laptop's own panel
	uint32				possibleCrtcs;	// mask of CRTC indices
	uint32				widthMM;
	uint32				heightMM;
	drm_mode_modeinfo	modes[Kms::kMaxModes];
	uint32				modeCount;
	int32				preferred;		// index into modes, -1 if none
	uint8				edid[Kms::kEdidLength];
	uint32				edidLength;

	// how it is driven: the layout
	bool				enabled;
	bool				mirror;
	int32				x;				// region in frame buffer pixels
	int32				y;
	uint32				width;
	uint32				height;
	uint16				scale;
	uint16				renderScale;
	drm_mode_modeinfo	timing;			// the mode on the wire
	int32				crtcIndex;		// -1 when not enabled
};


struct Layout {
	Output				outputs[kMaxOutputs];
	uint32				width;			// the frame buffer
	uint32				height;
};


// for clones: what they need to draw
struct SharedInfo {
	area_id				frameBufferArea;
	uint32				frameBufferOffset;
	display_mode		mode;
	uint32				bytesPerRow;
	char				devicePath[B_PATH_NAME_LENGTH];
};


struct accelerant_info {
	int					device;			// the first card's, app_server's
	bool				isClone;
	bool				isAmdgpu;		// the first card is amdgpu's
	area_id				sharedArea;
	SharedInfo*			shared;
	area_id				clonedFrameBuffer;
	void*				cloneFrameBuffer;

	Card				cards[kMaxCards];
	uint32				cardCount;

	Output				outputs[kMaxOutputs];	// as now driven
	uint32				outputCount;
	Layout				pending;		// SET_DISPLAY_LAYOUT's, until its
	bool				hasPending;		// mode is set

	uint32				frameBufferGeneration;	// counts the first card's
												// frame buffer's replacements
	display_mode		mode;
	bool				modeSet;
	display_mode*		modeList;
	uint32				modeCount;
	uint32				dpmsMode;

	sem_id				retraceSemaphore;
	thread_id			retraceThread;
	volatile bool		quitRetrace;
	// per output (by index into outputs; connectors are stable)
	sem_id				outputRetraceSemaphores[kMaxOutputs];
	thread_id			outputRetraceThreads[kMaxOutputs];
	uint32				outputRetraceConnectors[kMaxOutputs];

	uint32				cursorSize;		// the cards' smallest
	uint32*				cursorImage;	// what is in the cards' cursor
										// buffers: video memory can be lost
	uint32				vramLost;		// AMDGPU_INFO_VRAM_LOST_COUNTER
	uint16				cursorHotX;
	uint16				cursorHotY;
	uint16				cursorX;
	uint16				cursorY;
	bool				cursorVisible;
	bool				cursorSet;
	bool				cursorRefused;	// by a monitor: app_server draws it

	port_id				changePort;
	uint32				changeCode;
	thread_id			hotplugThread;
	volatile bool		quitHotplug;
	volatile bool		changedAsleep;	// hot plug events while DPMS was off
	volatile bool		outputsChanged;	// monitors came or went: probe
	volatile uint32		probedGeneration;
								// the hot plug count before the last probe
};


extern accelerant_info* gInfo;


static inline Card&
card_of(const Output& output)
{
	return gInfo->cards[output.card];
}


static inline uint32
output_id(uint32 card, uint32 connector)
{
	return card == 0 ? connector : (card << 24) | connector;
}


// more GPUs (accelerant.cpp, multi_card.cpp)
status_t init_card(Card& card, int fd);
void uninit_card(Card& card);
void open_other_cards(void);
bool multi_card_copies(void);
status_t multi_card_create_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source);
void multi_card_delete_source(uint32 id);
status_t multi_card_copy(uint32 id, const fill_rect_params* list,
	uint32 count);
void multi_card_fill_from_first(void);
extern "C" bool amdgpu_frame_buffer_is_partial(void);

// copies by nouveau's 2D engine (nouveau_copy.cpp); rectangles with
// exclusive right and bottom edges
struct copy_rect {
	int32	left;
	int32	top;
	int32	right;
	int32	bottom;
};
NouveauCopier* nouveau_copy_init(int fd);
void nouveau_copy_uninit(NouveauCopier* copier);
status_t nouveau_copy_import(NouveauCopier* copier, void* address, size_t size,
	uint32& _handle, uint64& _va);
void nouveau_copy_release(NouveauCopier* copier, uint32 handle);
status_t nouveau_copy_rects(NouveauCopier* copier, uint32 sourceHandle,
	uint64 sourceVa, uint32 sourcePitch, uint32 sourceWidth,
	uint32 sourceHeight, const Kms::Buffer& target, const copy_rect* rects,
	uint32 count);


// outputs and modes (mode.cpp)
status_t probe_outputs(void);
uint32 hotplug_generation(void);
void refresh_outputs(void);
void stop_hotplug(void);
void default_layout(Layout& layout);
display_mode layout_mode(const Layout& layout);
bool is_layout_mode(const Layout& layout, const display_mode& mode);
status_t apply_layout(Layout& layout, bool testOnly);
status_t create_mode_list(void);
void mode_to_timing(const drm_mode_modeinfo& mode, display_timing& timing);
void timing_to_mode(const display_timing& timing, drm_mode_modeinfo& mode);
int32 find_output_mode(const Output& output, const display_timing& timing);
Output* primary_output(void);
void update_cursor(void);

// copies to the screen by an Intel GPU's blitter (i915_copy.cpp)
status_t i915_create_frame_buffer_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source);
void i915_delete_frame_buffer_source(uint32 id);
status_t i915_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count);
void uninit_i915_copies(void);
void lock_i915_copies(void);
void unlock_i915_copies(void);

// copies to the screen by the GPU (device_copy.cpp)
void uninit_device_copies(void);
void lock_device_copies(void);
void unlock_device_copies(void);
// for tests: AMDGPU_GEM_DOMAIN_VRAM or _GTT
uint32 device_copy_set_shadow_domain(uint32 domain);
uint32 device_copy_source_domain(uint32 id);


extern "C" {
// accelerant.cpp
status_t amdgpu_init_accelerant(int device);
ssize_t amdgpu_accelerant_clone_info_size(void);
void amdgpu_get_accelerant_clone_info(void* info);
status_t amdgpu_clone_accelerant(void* info);
void amdgpu_uninit_accelerant(void);
status_t amdgpu_get_accelerant_device_info(accelerant_device_info* info);
sem_id amdgpu_accelerant_retrace_semaphore(void);
sem_id amdgpu_get_display_output_retrace_semaphore(uint32 id);
status_t amdgpu_get_display_output_scan_line(uint32 id, int32* _line,
	int32* _lines, bigtime_t* _scanStart, bigtime_t* _period);

// mode.cpp
uint32 amdgpu_accelerant_mode_count(void);
status_t amdgpu_get_mode_list(display_mode* list);
status_t amdgpu_propose_display_mode(display_mode* target,
	const display_mode* low, const display_mode* high);
status_t amdgpu_set_display_mode(display_mode* mode);
status_t amdgpu_get_display_mode(display_mode* mode);
status_t amdgpu_get_preferred_display_mode(display_mode* mode);
status_t amdgpu_get_edid_info(void* info, size_t size, uint32* version);
status_t amdgpu_get_frame_buffer_config(frame_buffer_config* config);
status_t amdgpu_get_pixel_clock_limits(display_mode* mode, uint32* low,
	uint32* high);
uint32 amdgpu_dpms_capabilities(void);
uint32 amdgpu_dpms_mode(void);
status_t amdgpu_set_dpms_mode(uint32 mode);
status_t amdgpu_set_brightness(float brightness);
status_t amdgpu_get_brightness(float* brightness);
bool connected_outputs_changed(void);

// layout.cpp
uint32 amdgpu_display_output_count(void);
status_t amdgpu_get_display_outputs(display_output* outputs, uint32* count);
status_t amdgpu_get_display_output_modes(uint32 id, display_mode* modes,
	uint32* count);
status_t amdgpu_set_display_layout(const display_output_config* configs,
	uint32 count, display_mode* mode);
status_t amdgpu_set_display_change_port(port_id port, uint32 code);
status_t amdgpu_wait_for_display_restore(bigtime_t timeout);
void restore_cursor_image(void);

// cursor.cpp
status_t amdgpu_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX,
	uint16 hotY, color_space space, uint16 bytesPerRow, const uint8* data);
void amdgpu_move_cursor(uint16 x, uint16 y);
void amdgpu_show_cursor(bool visible);

// device_copy.cpp
status_t amdgpu_create_frame_buffer_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source);
void amdgpu_delete_frame_buffer_source(uint32 id);
status_t amdgpu_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count);
status_t amdgpu_create_frame_buffer_shadow(uint16 width, uint16 height,
	color_space space, frame_buffer_source* shadow);
status_t amdgpu_copy_frame_buffer_source(uint32 from, uint32 to,
	const fill_rect_params* list, uint32 count);
}


#endif	// AMDGPU_KMS_ACCELERANT_H
