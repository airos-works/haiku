/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the GNU General Public License, version 2.
 *
 * The vendor setup of the Cirrus Logic CS8409 in Apple's iMacs 18,x/19,1 and
 * MacBook Pros 13,x/14,x for Haiku's hda driver: the speaker amplifiers and
 * the CS42L83 headphone codec sit on the CS8409's own I2C bus and must be
 * powered up the way macOS does it. The sequences in apple/ are from
 * https://github.com/davidjo/snd_hda_macbookpro (GPL-2.0, see apple/LICENSE
 * and apple/UPSTREAM), recorded from macOS.
 */


#include "linux_shim.h"

#include <stdio.h>
#include <stdlib.h>


static void cs_8409_really_update_stream_format(struct hda_codec* codec,
	hda_nid_t nid, int update_format_id, int update_stream_id,
	unsigned int new_channel_id);

#include "apple/patch_cirrus_new84.h"


static void dump_amplifiers(struct hda_codec* codec);


typedef struct cs8409_apple {
	struct hda_codec			codec;
	struct cs8409_apple_spec	spec;
	hda_codec_hook_ops			ops;
	bool						amplifiers_dumped;
} cs8409_apple;


static bool
is_supported(uint32 subsystemID)
{
	switch (subsystemID) {
		case 0x106b3300:	// MacBook Pro 13,1/14,1
		case 0x106b3600:	// MacBook Pro 14,2
		case 0x106b3900:	// MacBook Pro 14,3
		case 0x106b0e00:	// iMac 18,1
		case 0x106b0f00:	// iMac 18,2
		case 0x106b1000:	// iMac 18,3, 19,1
			return true;
	}
	return false;
}


static status_t
cs8409_attach(const hda_codec_hook_ops* ops, void** _cookie)
{
	if (ops->vendor_id != 0x10138409 || !is_supported(ops->subsystem_id))
		return B_NOT_SUPPORTED;

	cs8409_apple* device = (cs8409_apple*)calloc(1, sizeof(cs8409_apple));
	if (device == NULL)
		return B_NO_MEMORY;

	device->ops = *ops;
	struct hda_codec* codec = &device->codec;
	struct cs8409_apple_spec* spec = &device->spec;
	codec->ops = &device->ops;
	codec->spec = spec;
	codec->core.afg = ops->afg;
	codec->core.subsystem_id = ops->subsystem_id;
	codec->core.vendor_id = ops->vendor_id;
	codec->core.start_nid = 0x01;
	codec->core.num_nodes = 0x47;

	INIT_LIST_HEAD(&spec->unsol_list);
	spec->vendor_nid = CS8409_VENDOR_NID;
	spec->beep_nid = CS8409_BEEP_NID;
	spec->use_data = 0;
	spec->stream_channels = 2;
	spec->headset_play_format_setup_needed = 1;
	spec->headset_capture_format_setup_needed = 1;
	spec->nid_0x02.nid = 0x02;
	spec->nid_0x03.nid = 0x03;
	spec->nid_0x0a.nid = 0x0a;
	spec->nid_0x22.nid = 0x22;
	spec->nid_0x23.nid = 0x23;
	spec->nid_0x1a.nid = 0x1a;

	if (ops->subsystem_id == 0x106b0e00 || ops->subsystem_id == 0x106b0f00
		|| ops->subsystem_id == 0x106b1000) {
		// the iMacs swap the internal microphone and the line in
		spec->intmike_nid = CS8409_CS42L83_IMAC_MIC_PIN_NID;
		spec->linein_nid = CS8409_CS42L83_IMAC_LINEIN_PIN_NID;
		spec->intmike_adc_nid = 0x23;
		spec->linein_amp_nid = 0x22;
		spec->reg9_intmike_dmic_mo = 0x0080;
		spec->reg82_intmike_dmic_scl = 0x0002;
		spec->reg82_linein_dmic_scl = 0x0001;
	} else {
		spec->intmike_nid = CS8409_CS42L83_MACBOOK_MIC_PIN_NID;
		spec->linein_nid = CS8409_CS42L83_MACBOOK_LINEIN_PIN_NID;
		spec->intmike_adc_nid = 0x22;
		spec->linein_amp_nid = 0x23;
		spec->reg9_intmike_dmic_mo = 0x0020;
		spec->reg82_intmike_dmic_scl = 0x0001;
		spec->reg82_linein_dmic_scl = 0x0002;
	}

	dprintf("hda_cs8409: Apple CS8409, subsystem %08" B_PRIx32
		", running macOS's start-up sequence\n", ops->subsystem_id);
	bigtime_t start = system_time();
	int status = cs_8409_boot_setup(codec);
	spec->headset_phase = 2;
	ktime_get_real_ts64(&spec->last_play_time);
	ktime_get_real_ts64(&spec->first_play_time);
	dprintf("hda_cs8409: start-up sequence done (%d) in %" B_PRId64 " ms\n",
		status, (system_time() - start) / 1000);

	*_cookie = device;
	return B_OK;
}


static void
cs8409_detach(void* cookie)
{
	free(cookie);
}


static uint32
cs8409_fixed_rate(void* cookie)
{
	return 44100;
}


/*	The registers of the iMacs' TAS5764 speaker amplifiers, read back over the
	CS8409's I2C bus: power control (0xfd = on), volume control (0x80 + the
	TDM channel), volume, and fault status (0x18, low bits are errors).
*/
static void
dump_amplifiers(struct hda_codec* codec)
{
	static const unsigned int kAmplifiers[] = { 0xd8, 0xda, 0xdc, 0xde };
	static const unsigned int kRegisters[] = { 0x00, 0x01, 0x02, 0x03, 0x04,
		0x06, 0x08 };
	dprintf("hda_cs8409: AFG power %#x, GPIO data %#x mask %#x direction %#x\n",
		snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_POWER_STATE, 0),
		snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_GPIO_DATA, 0),
		snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_GPIO_MASK, 0),
		snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_GPIO_DIRECTION, 0));
	if (codec->core.subsystem_id != 0x106b0e00
		&& codec->core.subsystem_id != 0x106b0f00
		&& codec->core.subsystem_id != 0x106b1000)
		return;
	for (int i = 0; i < 4; i++) {
		char line[128];
		int length = snprintf(line, sizeof(line), "hda_cs8409: amplifier %#x:",
			kAmplifiers[i]);
		for (int r = 0; r < 7 && length < (int)sizeof(line); r++) {
			unsigned int value = cs_8409_vendor_i2cRead(codec, kAmplifiers[i],
				kRegisters[r], 0);
			length += snprintf(line + length, sizeof(line) - length,
				" %02x=%02x", kRegisters[r], value & 0xff);
		}
		dprintf("%s\n", line);
	}
}


/*	What Linux's playback prepare hook does: remember the stream for the
	converters the speaker amplifiers and the headphone codec listen to, then
	power the amplifiers up.
*/
static void
cs8409_playback_start(void* cookie, uint32 stream, uint32 format,
	uint32 channels)
{
	cs8409_apple* device = (cs8409_apple*)cookie;
	struct hda_codec* codec = &device->codec;
	struct cs8409_apple_spec* spec = &device->spec;

	cs_8409_store_stream_format(codec, 0x02, stream, format);
	cs_8409_store_stream_format(codec, 0x03, stream, format);
	cs_8409_store_stream_format(codec, 0x0a, stream, format);
	spec->stream_channels = channels == 4 ? 4 : 2;
	spec->playing = 1;

	bigtime_t start = system_time();
	cs_8409_play_setup(codec);
	dprintf("hda_cs8409: playback on (stream %" B_PRIu32 ", format %#"
		B_PRIx32 ", %" B_PRIu32 " channels) in %" B_PRId64 " ms\n", stream,
		format, channels, (system_time() - start) / 1000);
	if (!device->amplifiers_dumped) {
		// what the amplifiers say the first time is what tells whether they
		// came up
		device->amplifiers_dumped = true;
		dump_amplifiers(codec);
	}
}


static void
cs8409_playback_stop(void* cookie)
{
	cs8409_apple* device = (cs8409_apple*)cookie;
	cs_8409_play_cleanup(&device->codec);
	device->spec.playing = 0;
	dprintf("hda_cs8409: playback off\n");
}


static void
cs8409_record_start(void* cookie, uint32 stream, uint32 format,
	uint32 channels)
{
	cs8409_apple* device = (cs8409_apple*)cookie;
	struct hda_codec* codec = &device->codec;
	struct cs8409_apple_spec* spec = &device->spec;

	cs_8409_store_stream_format(codec, spec->intmike_adc_nid, stream, format);
	spec->capturing = 1;
	cs_8409_capture_setup(codec);
	dprintf("hda_cs8409: recording on (stream %" B_PRIu32 ", format %#"
		B_PRIx32 ")\n", stream, format);

}


/*	The internal microphone's ADC: hda would put every input converter on the
	record stream, and the CS8409 sends nothing on it then. */
static uint32
cs8409_record_converter(void* cookie)
{
	return ((cs8409_apple*)cookie)->spec.intmike_adc_nid;
}


static void
cs8409_record_stop(void* cookie)
{
	cs8409_apple* device = (cs8409_apple*)cookie;
	cs_8409_capture_cleanup(&device->codec);
	device->spec.capturing = 0;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;
	}
	return B_ERROR;
}


static hda_codec_hooks_module_info sCS8409AppleModule = {
	{
		HDA_CODEC_HOOKS_PREFIX "/cs8409_apple/v1",
		0,
		std_ops
	},
	cs8409_attach,
	cs8409_detach,
	cs8409_fixed_rate,
	cs8409_playback_start,
	cs8409_playback_stop,
	cs8409_record_start,
	cs8409_record_stop,
	cs8409_record_converter
};


module_info* modules[] = {
	(module_info*)&sCS8409AppleModule,
	NULL
};
