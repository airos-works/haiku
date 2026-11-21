/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the GNU General Public License, version 2:
 * this file only exists to build the GPL CS8409 sequences in apple/.
 *
 * Just enough of Linux's HD-audio codec API for the Apple CS8409 sequences
 * of snd_hda_macbookpro (apple/patch_cirrus_*.h) to run on Haiku's hda driver.
 */
#ifndef HDA_CS8409_APPLE_LINUX_SHIM_H
#define HDA_CS8409_APPLE_LINUX_SHIM_H


#include <KernelExport.h>
#include <OS.h>
#include <string.h>

#include "hda_codec_hooks.h"


typedef uint8 u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint16 hda_nid_t;

#define LINUX_VERSION_CODE			0x060000
#define KERNEL_VERSION(a, b, c)		(((a) << 16) + ((b) << 8) + (c))

#define USE_DATA_UNDEFINED

#include "apple/hda_verbs.h"

/* pre 5.13 numbering, as the sequences use it */
#define CS8409_IDX_DEV_CFG				0x01
#define CS8409_VENDOR_NID				0x47
#define CS8409_BEEP_NID					0x46
#define CS8409_CS42L83_HP_PIN_NID			0x2c
#define CS8409_CS42L83_HP_MIC_PIN_NID		0x3c
#define CS8409_CS42L83_MACBOOK_MIC_PIN_NID	0x44
#define CS8409_CS42L83_MACBOOK_LINEIN_PIN_NID	0x45
#define CS8409_CS42L83_IMAC_MIC_PIN_NID		0x45
#define CS8409_CS42L83_IMAC_LINEIN_PIN_NID	0x44
#define CS8409_CS42L83_MACBOOK_LINEIN_ADC_PIN_NID	0x23
#define CS8409_CS42L83_IMAC_LINEIN_ADC_PIN_NID	0x22
#define CS42L83_I2C_ADDR				0x90


struct list_head {
	struct list_head* next;
	struct list_head* prev;
};

struct mutex {
	int unused;
};

struct timespec64 {
	int64 tv_sec;
	long tv_nsec;
};

struct hda_verb {
	hda_nid_t nid;
	u32 verb;
	u32 param;
};

struct hda_cvt_setup_apple {
	hda_nid_t nid;
	u8 stream_tag;
	u8 channel_id;
	u16 format_id;
	unsigned char active;
	unsigned char dirty;
};

struct hda_gen_spec {
	int suppress_auto_mute;
	int no_primary_hp;
	int suppress_vmaster;
	int suppress_auto_mic;
	int num_adc_nids;
	int dyn_adc_switch;
	void* input_paths;
	void* input_mux;
	void* pcm_playback_hook;
	void* pcm_capture_hook;
	void* mic_autoswitch_hook;
	void* line_automute_hook;
	void* hp_automute_hook;
};

struct cs8409_apple_spec {
	struct hda_gen_spec gen;

	unsigned int gpio_mask;
	unsigned int gpio_dir;
	unsigned int gpio_data;
	hda_nid_t vendor_nid;
	hda_nid_t beep_nid;

	int block_unsol;
	struct list_head unsol_list;
	int unsol_items_prealloc[10];
	int unsol_items_prealloc_used[10];

	struct mutex unsol_lock;
	struct mutex i2ccodeblock_lock;
	struct mutex pcm_mutex;

	int codec_suspended;
	int subcodec_suspended;

	int intmike_nid;
	int linein_nid;
	int intmike_adc_nid;
	int linein_amp_nid;

	int reg9_intmike_dmic_mo;
	int reg9_linein_dmic_mo;
	int reg82_intmike_dmic_scl;
	int reg82_linein_dmic_scl;

	struct hda_cvt_setup_apple nid_0x02;
	struct hda_cvt_setup_apple nid_0x03;
	struct hda_cvt_setup_apple nid_0x0a;
	struct hda_cvt_setup_apple nid_0x22;
	struct hda_cvt_setup_apple nid_0x23;
	struct hda_cvt_setup_apple nid_0x1a;

	int jack_present;
	int headset_type;
	int have_mike;
	int have_buttons;
	int stream_channels;
	int playing;
	int capturing;
	int headset_play_format_setup_needed;
	int headset_capture_format_setup_needed;
	int headset_presetup_done;
	int use_data;
	int headset_phase;
	int headset_enable;
	int play_init;
	int capture_init;
	int play_init_count;
	int capture_init_count;
	struct timespec64 last_play_time;
	struct timespec64 first_play_time;
};

struct hda_codec_core {
	unsigned int afg;
	unsigned int subsystem_id;
	unsigned int vendor_id;
	int num_nodes;
	unsigned int start_nid;
};

/* converter setup cache: entries laid out like the sequences' hda_cvt_setup */
struct lx_cvt_setup {
	hda_nid_t nid;
	u8 stream_tag;
	u8 channel_id;
	u16 format_id;
	unsigned char active;
	unsigned char dirty;
};

#define LX_MAX_CVT_SETUPS	16

struct snd_array {
	unsigned int used;
	struct lx_cvt_setup list[LX_MAX_CVT_SETUPS];
};

#define snd_array_elem(array, index)	((void*)&(array)->list[(index)])
#define snd_array_for_each(array, idx, ptr) \
	for ((idx) = 0; (idx) < (int)(array)->used \
		&& ((ptr) = snd_array_elem(array, idx), 1); (idx)++)

static inline void*
snd_array_new(struct snd_array* array)
{
	if (array->used >= LX_MAX_CVT_SETUPS)
		return NULL;
	struct lx_cvt_setup* entry = &array->list[array->used++];
	memset(entry, 0, sizeof(*entry));
	return entry;
}

struct hda_codec {
	struct hda_codec_core core;
	void* spec;
	const hda_codec_hook_ops* ops;
	unsigned int (*power_filter)(struct hda_codec* codec, hda_nid_t nid,
		unsigned int powerState);
	void* bus;
	struct snd_array cvt_setups;
};

#define for_each_hda_codec_node(nid, codec) \
	for ((nid) = (codec)->core.start_nid; \
		(nid) < (codec)->core.start_nid + (codec)->core.num_nodes; (nid)++)


/* verbs */

static inline unsigned int
lx_send(struct hda_codec* codec, hda_nid_t nid, unsigned int verb,
	unsigned int parm)
{
	uint32 command = ((uint32)nid << 20) | (verb << 8) | parm;
	uint32 response = 0;
	if (codec->ops->send(codec->ops->cookie, &command, &response, 1) != B_OK)
		return (unsigned int)-1;
	return response;
}

static inline int
snd_hda_codec_write(struct hda_codec* codec, hda_nid_t nid, int flags,
	unsigned int verb, unsigned int parm)
{
	return lx_send(codec, nid, verb, parm) == (unsigned int)-1 ? -1 : 0;
}

static inline unsigned int
snd_hda_codec_read(struct hda_codec* codec, hda_nid_t nid, int flags,
	unsigned int verb, unsigned int parm)
{
	return lx_send(codec, nid, verb, parm);
}

static inline unsigned int
get_wcaps(struct hda_codec* codec, hda_nid_t nid)
{
	return snd_hda_codec_read(codec, nid, 0, AC_VERB_PARAMETERS,
		AC_PAR_AUDIO_WIDGET_CAP);
}

#define get_wcaps_type(wcaps)	(((wcaps) & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT)

static inline void
snd_hda_sequence_write(struct hda_codec* codec, const struct hda_verb* seq)
{
	for (; seq->nid != 0; seq++)
		snd_hda_codec_write(codec, seq->nid, 0, seq->verb, seq->param);
}


/* As Linux's: program a converter's stream and format, and cache them. */
static inline void
snd_hda_codec_setup_stream(struct hda_codec* codec, hda_nid_t nid,
	u32 streamTag, int channelID, int format)
{
	struct lx_cvt_setup* entry = NULL;
	for (unsigned int i = 0; i < codec->cvt_setups.used; i++) {
		if (codec->cvt_setups.list[i].nid == nid)
			entry = &codec->cvt_setups.list[i];
	}
	if (entry == NULL) {
		entry = (struct lx_cvt_setup*)snd_array_new(&codec->cvt_setups);
		if (entry == NULL)
			return;
		entry->nid = nid;
	}
	if (entry->stream_tag != streamTag || entry->channel_id != channelID) {
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CHANNEL_STREAMID,
			(streamTag << 4) | channelID);
		entry->stream_tag = streamTag;
		entry->channel_id = channelID;
	}
	if (format != 0 && entry->format_id != format) {
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_STREAM_FORMAT, format);
		entry->format_id = format;
	}
	entry->active = 1;
	entry->dirty = 0;
}

#define hweight_long(x)		__builtin_popcountl(x)

/* headphone jack reporting is ALSA's */
#define cs_8409_cs42l83_mark_jack(codec)		do { } while (0)
#define cs_8409_cs42l83_jack_report_sync(codec)	do { } while (0)

/* no suspend/resume on Haiku yet */
#define cs_8409_wait_for_resume(codec, msleeptim, trycount)	0


/* time */

static inline void msleep(unsigned int ms) { snooze((bigtime_t)ms * 1000); }
static inline void mdelay(unsigned int ms) { spin((bigtime_t)ms * 1000); }
static inline void usleep_range(unsigned long min, unsigned long max)
	{ snooze(min); }

static inline void
ktime_get_real_ts64(struct timespec64* ts)
{
	bigtime_t now = system_time();
	ts->tv_sec = now / 1000000;
	ts->tv_nsec = (now % 1000000) * 1000;
}

#define jiffies					(system_time() / 1000)
#define msecs_to_jiffies(ms)	((bigtime_t)(ms))
#define time_after_eq(a, b)		((int64)((a) - (b)) >= 0)


/* locks: the hda driver calls the hooks one at a time */

#define mutex_lock(m)			do { } while (0)
#define mutex_unlock(m)			do { } while (0)
#define mutex_is_locked(m)		0
#define mutex_init(m)			do { } while (0)


/* lists: the unsolicited responses are not used on Haiku */

#define INIT_LIST_HEAD(l)		do { (l)->next = (l); (l)->prev = (l); } while (0)
#define list_empty(l)			((l)->next == (l))
#define list_add_tail(n, l)		do { } while (0)
#define list_del_init(n)		do { } while (0)
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = NULL, n = NULL; pos != NULL; )


/* messages */

#define printk(fmt, args...)				dprintf("hda_cs8409: " fmt, ##args)
#define hda_codec_dev(codec)				(codec)
#define dev_info(dev, fmt, args...)			dprintf("hda_cs8409: " fmt, ##args)
#define dev_err(dev, fmt, args...)			dprintf("hda_cs8409: ERROR " fmt, ##args)
#define codec_info(codec, fmt, args...)		dprintf("hda_cs8409: " fmt, ##args)
#define codec_err(codec, fmt, args...)		dprintf("hda_cs8409: ERROR " fmt, ##args)
#define codec_dbg(codec, fmt, args...)		do { } while (0)

#define mycodec_info(...)		do { } while (0)
#define mycodec_i2c_info(...)	do { } while (0)
#define mydev_info(...)			do { } while (0)
#define mycodec_dbg(...)		do { } while (0)
#define myprintk_dbg(...)		do { } while (0)
#define myprintk(...)			do { } while (0)
#define myprintk_i2c_dbg(...)	do { } while (0)


#endif	/* HDA_CS8409_APPLE_LINUX_SHIM_H */
