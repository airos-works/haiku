/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RPI_EDID_TIMING_H
#define RPI_EDID_TIMING_H

#include <stdint.h>
#include <string.h>

// Raspberry Pi firmware SET_TIMING (the Linux vc4 firmware KMS ABI).
struct firmware_timing {
	uint8_t display, padding;
	uint16_t video_id_code;
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint16_t vrefresh, padding2;
	uint32_t flags;
};
static_assert(sizeof(firmware_timing) == 36, "firmware timing ABI");

#define RPI_TIMING_ASPECT_16_9		(2u << 4)
#define RPI_TIMING_DVI				(1u << 9)

// The HDMI pixel clock (kHz) the firmware drives when it does not say:
// both outputs manage 4K at 30 Hz, 60 Hz needs hdmi_enable_4kp60.
#define RPI_DEFAULT_MAX_PIXEL_CLOCK	300000

static inline bool
valid_edid_block(const uint8_t* data, bool base)
{
	static const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
	if (base && memcmp(data, header, sizeof(header)) != 0)
		return false;
	uint8_t sum = 0;
	for (unsigned i = 0; i < 128; i++)
		sum += data[i];
	return sum == 0;
}

/*!	The size and timing a detailed timing descriptor describes, whether the
	Pi can show it or not.
*/
static inline bool
decode_detailed_timing(const uint8_t* d, firmware_timing& timing)
{
	firmware_timing t = {};
	t.clock = (d[0] | (d[1] << 8)) * 10;
	// Progressive digital separate sync.
	if (t.clock == 0 || (d[17] & 0x98) != 0x18)
		return false;
	t.hdisplay = d[2] | ((d[4] & 0xf0) << 4);
	t.htotal = t.hdisplay + (d[3] | ((d[4] & 15) << 8));
	t.vdisplay = d[5] | ((d[7] & 0xf0) << 4);
	t.vtotal = t.vdisplay + (d[6] | ((d[7] & 15) << 8));
	t.hsync_start = t.hdisplay + (d[8] | ((d[11] & 0xc0) << 2));
	t.hsync_end = t.hsync_start + (d[9] | ((d[11] & 0x30) << 4));
	t.vsync_start = t.vdisplay + ((d[10] >> 4) | ((d[11] & 0x0c) << 2));
	t.vsync_end = t.vsync_start + ((d[10] & 15) | ((d[11] & 3) << 4));
	if (t.hdisplay == 0 || t.vdisplay == 0 || t.htotal == 0 || t.vtotal == 0)
		return false;
	t.flags = ((d[17] & 2) ? 1 : 0) | ((d[17] & 4) ? 2 : 0);
	t.vrefresh = (uint16_t)(((uint64_t)t.clock * 1000u
		+ (uint32_t)t.htotal * t.vtotal / 2) / ((uint32_t)t.htotal * t.vtotal));
	timing = t;
	return true;
}

/*!	Whether the Pi 4's HDMI can send \a t: within the output's pixel clock,
	no odd horizontal timings (Linux's firmware KMS rejects them on the
	BCM2711), and a sane size and refresh rate (at most 85 Hz, as there).
*/
static inline bool
usable_timing(const firmware_timing& t, uint32_t maxClock)
{
	return t.clock != 0 && t.clock <= maxClock
		&& t.hdisplay >= 320 && t.vdisplay >= 200 && t.hdisplay <= 4096
		&& t.vdisplay <= 2160 && t.hsync_start > t.hdisplay
		&& t.hsync_end > t.hsync_start && t.hsync_end <= t.htotal
		&& t.vsync_start > t.vdisplay && t.vsync_end > t.vsync_start
		&& t.vsync_end <= t.vtotal
		&& ((t.hdisplay | t.hsync_start | t.hsync_end | t.htotal) & 1) == 0
		&& t.vrefresh >= 23 && t.vrefresh <= 85;
}

/*!	The CTA-861 video codes a monitor most likely offers in place of a
	detailed timing the Pi cannot send: its 4K modes are often only codes.
*/
static inline bool
cta_timing(unsigned code, firmware_timing& timing)
{
	static const struct {
		uint8_t code;
		uint32_t clock;
		uint16_t h[4];
		uint16_t v[4];
	} kTimings[] = {
		{4, 74250, {1280, 1390, 1430, 1650}, {720, 725, 730, 750}},
		{16, 148500, {1920, 2008, 2052, 2200}, {1080, 1084, 1089, 1125}},
		{31, 148500, {1920, 2448, 2492, 2640}, {1080, 1084, 1089, 1125}},
		{93, 297000, {3840, 5116, 5204, 5500}, {2160, 2168, 2178, 2250}},
		{94, 297000, {3840, 4896, 4984, 5280}, {2160, 2168, 2178, 2250}},
		{95, 297000, {3840, 4016, 4104, 4400}, {2160, 2168, 2178, 2250}},
		{96, 594000, {3840, 4896, 4984, 5280}, {2160, 2168, 2178, 2250}},
		{97, 594000, {3840, 4016, 4104, 4400}, {2160, 2168, 2178, 2250}},
	};
	for (unsigned i = 0; i < sizeof(kTimings) / sizeof(kTimings[0]); i++) {
		if (kTimings[i].code != code)
			continue;
		firmware_timing t = {};
		t.video_id_code = code;
		t.clock = kTimings[i].clock;
		t.hdisplay = kTimings[i].h[0];
		t.hsync_start = kTimings[i].h[1];
		t.hsync_end = kTimings[i].h[2];
		t.htotal = kTimings[i].h[3];
		t.vdisplay = kTimings[i].v[0];
		t.vsync_start = kTimings[i].v[1];
		t.vsync_end = kTimings[i].v[2];
		t.vtotal = kTimings[i].v[3];
		t.vrefresh = (uint16_t)(((uint64_t)t.clock * 1000u
			+ (uint32_t)t.htotal * t.vtotal / 2)
			/ ((uint32_t)t.htotal * t.vtotal));
		t.flags = 1 | 2 | RPI_TIMING_ASPECT_16_9;
		timing = t;
		return true;
	}
	return false;
}

/*!	Picks the timing to drive a monitor with. The monitor's preferred timing
	(its first detailed timing) when the Pi can send it; otherwise one at the
	same size, the fastest the Pi manages (a 4K monitor preferring 60 Hz gets
	30 Hz), and only failing that the largest other one, so that the desktop
	is never shown smaller than the monitor's panel when it need not be.
*/
static inline bool
preferred_edid_timing(const uint8_t* edid, unsigned length,
	firmware_timing& timing, uint32_t maxClock = RPI_DEFAULT_MAX_PIXEL_CLOCK)
{
	if (length < 128 || !valid_edid_block(edid, true))
		return false;

	firmware_timing preferred;
	bool havePreferred = decode_detailed_timing(edid + 54, preferred);
	bool found = false;
	firmware_timing best = {};
	auto consider = [&](const firmware_timing& t) {
		if (!usable_timing(t, maxClock))
			return;
		if (found) {
			bool fits = havePreferred && t.hdisplay == preferred.hdisplay
				&& t.vdisplay == preferred.vdisplay;
			bool bestFits = havePreferred
				&& best.hdisplay == preferred.hdisplay
				&& best.vdisplay == preferred.vdisplay;
			uint32_t area = (uint32_t)t.hdisplay * t.vdisplay;
			uint32_t bestArea = (uint32_t)best.hdisplay * best.vdisplay;
			if (fits != bestFits) {
				if (!fits)
					return;
			} else if (area != bestArea) {
				if (area < bestArea)
					return;
			} else if (t.vrefresh <= best.vrefresh)
				return;
		}
		best = t;
		found = true;
	};

	if (havePreferred && usable_timing(preferred, maxClock)) {
		best = preferred;
		found = true;
	} else {
		for (unsigned offset = 54; offset + 18 <= 126; offset += 18) {
			firmware_timing t;
			if (decode_detailed_timing(edid + offset, t))
				consider(t);
		}
	}

	bool hdmi = false;
	if (length >= 256 && edid[126] != 0
		&& valid_edid_block(edid + 128, false) && edid[128] == 2) {
		const uint8_t* cea = edid + 128;
		unsigned end = cea[2];
		if (end >= 4 && end <= 127) {
			bool preferredFound = found;
			for (unsigned i = 4; i < end;) {
				unsigned size = cea[i] & 31;
				if (i + 1 + size > end)
					break;
				// HDMI vendor-specific data block, IEEE OUI 00-0c-03.
				if ((cea[i] >> 5) == 3 && size >= 3 && cea[i + 1] == 3
					&& cea[i + 2] == 12 && cea[i + 3] == 0)
					hdmi = true;
				// Video data block: short video descriptors, codes 129 to
				// 192 being 1 to 64 marked native.
				if ((cea[i] >> 5) == 2 && !preferredFound) {
					for (unsigned j = 1; j <= size; j++) {
						unsigned code = cea[i + j];
						if (code >= 129 && code <= 192)
							code &= 127;
						firmware_timing t;
						if (cta_timing(code, t))
							consider(t);
					}
				}
				i += size + 1;
			}
			for (unsigned i = end; !preferredFound && i + 18 <= 127; i += 18) {
				firmware_timing t;
				if (decode_detailed_timing(cea + i, t))
					consider(t);
			}
		}
	}
	if (!found && (edid[35] & 0x20) != 0) {
		// EDID explicitly advertises VGA 640x480 at 60 Hz.
		best = {0, 0, 0, 25175, 640, 656, 752, 800, 0, 480, 490,
			492, 525, 0, 60, 0, 0};
		found = true;
	}
	if (!found)
		return false;
	if (!hdmi) {
		// DVI has no infoframes to carry a video code or an aspect ratio.
		best.flags = (best.flags & 3) | RPI_TIMING_DVI;
		best.video_id_code = 0;
	}
	timing = best;
	return true;
}

#endif
