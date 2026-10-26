/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DP_SINK_H
#define _DP_SINK_H


/*!	What every DisplayPort source driver does with the sink before it trains
	the link, whatever its AUX channel: the sink (or the branch device in
	front of it: a USB-C hub's or an adapter's DisplayPort to HDMI converter)
	powered up, and a converter to HDMI left in its DVI-compatible output.

	In HDMI mode (DP_PROTOCOL_CONVERTER_CONTROL_0's HDMI_DVI_OUTPUT_CONFIG,
	which i915 sets) a converter waits for the AVI InfoFrames its source sends
	with it; our drivers send none, and a hub's converter (0060ad "cHDMIb")
	then put out no picture at all, its monitor in power save. A converter
	that comes up in HDMI mode, or was left in it, is set back.

	Header only, for kernel drivers and accelerants alike: \a read(address)
	returns the DPCD byte or a negative value when the AUX transaction failed,
	\a write(address, value) returns whether it went through.
	dp_prepare_sink() returns true when it woke the sink up: the sink then
	needs 1 ms before its link is trained (DP 1.2 2.5.3.3.1).
*/


#define DP_SINK_DPCD_REV						0x0000
#define DP_SINK_DOWNSTREAMPORT_PRESENT			0x0005
#define DP_SINK_DFP_PRESENT						(1 << 0)
#define DP_SINK_DETAILED_CAP_INFO				(1 << 4)
#define DP_SINK_DOWNSTREAM_PORT_0				0x0080
#define DP_SINK_DFP_TYPE_MASK					0x07
#define DP_SINK_DFP_TYPE_HDMI					0x03
#define DP_SINK_SET_POWER						0x0600
#define DP_SINK_SET_POWER_MASK					0x07
#define DP_SINK_SET_POWER_D0					0x01
#define DP_SINK_PROTOCOL_CONVERTER_CONTROL_0	0x3050
#define DP_SINK_HDMI_DVI_OUTPUT_CONFIG			(1 << 0)


template<typename Read, typename Write>
static inline bool
dp_prepare_sink(Read read, Write write)
{
	int revision = read(DP_SINK_DPCD_REV);
	if (revision < 0)
		return false;

	// D0 (DP 1.1 and later)
	bool woken = false;
	if (revision >= 0x11) {
		int power = read(DP_SINK_SET_POWER);
		if (power >= 0
			&& (power & DP_SINK_SET_POWER_MASK) != DP_SINK_SET_POWER_D0) {
			woken = write(DP_SINK_SET_POWER,
				(power & ~DP_SINK_SET_POWER_MASK) | DP_SINK_SET_POWER_D0);
		}
	}

	// a converter to HDMI in its DVI-compatible output
	int present = read(DP_SINK_DOWNSTREAMPORT_PRESENT);
	if (present < 0 || (present & DP_SINK_DFP_PRESENT) == 0
		|| (present & DP_SINK_DETAILED_CAP_INFO) == 0)
		return woken;
	int port = read(DP_SINK_DOWNSTREAM_PORT_0);
	if (port < 0 || (port & DP_SINK_DFP_TYPE_MASK) != DP_SINK_DFP_TYPE_HDMI)
		return woken;
	int control = read(DP_SINK_PROTOCOL_CONVERTER_CONTROL_0);
	if (control >= 0 && (control & DP_SINK_HDMI_DVI_OUTPUT_CONFIG) != 0) {
		write(DP_SINK_PROTOCOL_CONVERTER_CONTROL_0,
			control & ~DP_SINK_HDMI_DVI_OUTPUT_CONFIG);
	}
	return woken;
}


#endif	// _DP_SINK_H
