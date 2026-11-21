/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Optional modules with the vendor setup some HD-audio codecs need beyond the
 * HDA specification, such as the Cirrus Logic CS8409 of Apple's Macs, whose
 * speaker amplifiers sit on the codec's own I2C bus. The hda driver offers
 * each codec to the modules in HDA_CODEC_HOOKS_PREFIX; the one that takes it
 * runs at codec start-up and when its streams start and stop.
 */
#ifndef _HDA_CODEC_HOOKS_H
#define _HDA_CODEC_HOOKS_H


#include <module.h>


#define HDA_CODEC_HOOKS_PREFIX	"media/hda_codec_hooks"


typedef struct hda_codec_hook_ops {
	void*		cookie;
	uint32		vendor_id;		/* codec vendor << 16 | product */
	uint32		subsystem_id;
	uint32		afg;			/* audio function group node */

	/* commands are nid << 20 | verb << 8 | payload, without the address */
	status_t	(*send)(void* cookie, const uint32* commands,
					uint32* responses, uint32 count);
} hda_codec_hook_ops;


typedef struct hda_codec_hooks_module_info {
	module_info	info;

	/* B_OK when the module drives this codec; it sets the codec up then */
	status_t	(*attach)(const hda_codec_hook_ops* ops, void** _cookie);
	void		(*detach)(void* cookie);

	/* the only sample rate the codec supports, or 0 for any */
	uint32		(*fixed_rate)(void* cookie);

	/* called before a stream runs, after its converters got the stream
	   number and the HDA stream format; and after it stopped */
	void		(*playback_start)(void* cookie, uint32 stream, uint32 format,
					uint32 channels);
	void		(*playback_stop)(void* cookie);
	void		(*record_start)(void* cookie, uint32 stream, uint32 format,
					uint32 channels);
	void		(*record_stop)(void* cookie);

	/* the one input converter to record through, or 0 for all of them */
	uint32		(*record_converter)(void* cookie);
} hda_codec_hooks_module_info;


#endif	/* _HDA_CODEC_HOOKS_H */
