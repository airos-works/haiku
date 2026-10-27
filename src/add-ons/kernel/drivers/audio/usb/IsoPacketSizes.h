/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_ISO_PACKET_SIZES_H_
#define _USB_AUDIO_ISO_PACKET_SIZES_H_

/*	How usb_audio cuts a stream into isochronous packets, one per USB frame
	(1 ms at full speed). 44.1 kHz is 44.1 samples per frame: nine packets of
	44 samples and one of 45 in every 10 frames. Packing 44 into every frame
	plays 44.0 kHz and starves the device.

	A buffer is a whole number of these patterns, so all buffers have the same
	packet sizes and an exact number of samples. The packet count per buffer is
	also capped, because a full-speed device behind a high-speed hub runs on
	the EHCI controller's 128-frame isochronous schedule, and two buffers have
	to fit in it.

	No kernel dependencies: tools/cubie-a7s/ehci-sitd-test.cpp checks it on
	the host.
*/

#include <stdint.h>


namespace usb_audio_packets {


inline uint32_t
gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t rest = a % b;
		a = b;
		b = rest;
	}
	return a;
}


// The number of packets after which the packet sizes repeat.
inline uint32_t
pattern_packets(uint32_t rate, uint32_t packetsPerSecond)
{
	if (rate == 0 || packetsPerSecond == 0)
		return 1;
	return packetsPerSecond / gcd(rate, packetsPerSecond);
}


// Samples in packet index of a buffer that starts on a pattern boundary.
inline uint32_t
samples_in_packet(uint32_t rate, uint32_t packetsPerSecond, uint32_t index)
{
	uint64_t before = (uint64_t)index * rate / packetsPerSecond;
	uint64_t after = (uint64_t)(index + 1) * rate / packetsPerSecond;
	return (uint32_t)(after - before);
}


// The largest packet, in samples.
inline uint32_t
max_samples_in_packet(uint32_t rate, uint32_t packetsPerSecond)
{
	return (rate + packetsPerSecond - 1) / packetsPerSecond;
}


// Packets per buffer: whole patterns, holding at most maxSamples samples and
// at most maxPackets packets where that leaves at least one pattern.
inline uint32_t
packets_per_buffer(uint32_t rate, uint32_t packetsPerSecond,
	uint32_t maxSamples, uint32_t maxPackets)
{
	uint32_t pattern = pattern_packets(rate, packetsPerSecond);
	uint64_t packets = (uint64_t)maxSamples * packetsPerSecond / rate;
	if (packets > maxPackets)
		packets = maxPackets;
	packets -= packets % pattern;
	if (packets == 0)
		packets = pattern;
	return (uint32_t)packets;
}


// Samples in a buffer of packets packets (a multiple of the pattern).
inline uint32_t
samples_per_buffer(uint32_t rate, uint32_t packetsPerSecond, uint32_t packets)
{
	return (uint32_t)((uint64_t)packets * rate / packetsPerSecond);
}


}	// namespace usb_audio_packets


#endif	// _USB_AUDIO_ISO_PACKET_SIZES_H_
