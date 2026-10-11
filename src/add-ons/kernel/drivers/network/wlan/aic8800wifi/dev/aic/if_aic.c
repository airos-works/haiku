/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	aic8800wifi: the AIC8800D80's FullMAC firmware (the Radxa Cubie A7S's
	Quectel FCU760K) under OpenBSD's net80211, the way broadcomfmac runs
	OpenBSD's bwfm.

	The firmware scans, associates, encrypts and aggregates; net80211 picks
	the network, runs the WPA 4-way handshake (the firmware leaves it to the
	host and only wants the keys) and talks to Haiku's user interface and
	wpa_supplicant's HAIKU_JOIN. Scan results are the frames the firmware
	heard and go into net80211 as received beacons; the firmware's connect
	and disconnect messages drive the state machine; data leaves as Ethernet
	frames behind a descriptor and comes back as 802.11 frames that this
	driver turns into Ethernet ones.

	The protocol facts are from cubie/evidence/wifi (DESIGN.md sections 2
	and 3, fdrv-protocol.md); none of the vendor's code is used.

	Threads: net80211's hooks run under Giant and never wait for the
	firmware; they leave work for the driver's thread, which runs it under
	Giant and gives Giant up while a command waits for its confirmation
	(the transport matches confirmations on its own threads). Received
	frames are converted on the transport's receive threads and handed to
	the stack under Giant at the end of each USB transfer. */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/endian.h>
#include <sys/task.h>
#include <sys/device.h>
#include <sys/bus.h>
#include <machine/bus.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ifq.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#undef DPRINTF

#include <driver_settings.h>

#include "aic_usb.h"
#include "dev/aic/if_aicreg.h"
#include "dev/aic/if_aicvar.h"


#define DEVNAME(sc)		gDriverName
#define AIC_COMMAND_TIMEOUT	4000000
#define AIC_EAPOL_FENCE		500000
#define AIC_SCAN_TIMEOUT	(20 * 1000000LL)
#define AIC_EVENTS_MAX		256
#define AIC_REORDER_TIMEOUT	50000

#define AIC_TYPE_DATA_TX	0x01
#define ETHERTYPE_EAPOL_BYTES(p)	((p)[0] == 0x88 && (p)[1] == 0x8e)

static int aic_debug;
static int aic_watch;

#define DPRINTF(x...)	do { if (aic_debug) printf(x); } while (0)


static int	aic_probe(device_t);
static int	aic_attach(device_t);
static int	aic_detach(device_t);

static void	aic_start(struct ifnet*);
static void	aic_init(struct ifnet*);
static void	aic_stop(struct ifnet*);
static int	aic_ioctl(struct ifnet*, u_long, caddr_t);
static int	aic_media_change(struct ifnet*);
static int	aic_newstate(struct ieee80211com*, enum ieee80211_state, int);
static int	aic_send_mgmt(struct ieee80211com*, struct ieee80211_node*, int,
				int, int);
static int	aic_set_key(struct ieee80211com*, struct ieee80211_node*,
				struct ieee80211_key*);
static void	aic_delete_key(struct ieee80211com*, struct ieee80211_node*,
				struct ieee80211_key*);
static int	aic_bgscan(struct ieee80211com*);

static void	aic_cb_message(void*, uint16, const uint8*, size_t);
static void	aic_cb_data(void*, int, const uint8*, size_t);
static void	aic_cb_data_done(void*, int);
static void	aic_cb_tx_confirm(void*, uint32, uint32);
static void	aic_cb_tx_ready(void*);
static void	aic_cb_gone(void*);
static void	aic_cb_back(void*);

static const struct aic_usb_callbacks aic_callbacks = {
	aic_cb_message,
	aic_cb_data,
	aic_cb_data_done,
	aic_cb_tx_confirm,
	aic_cb_tx_ready,
	aic_cb_gone,
	aic_cb_back,
};


/* The channels the firmware is told about and scans: the world domain the
   vendor's driver uses, 2.4 GHz 1 to 13 and 5 GHz 36 to 165. */
static const uint8_t aic_channels_2ghz[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
static const uint8_t aic_channels_5ghz[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
	132, 136, 140, 144, 149, 153, 157, 161, 165
};
#define AIC_CHANNEL_POWER	20


/* the vendor's default transmit power table ("v3"), which the user
   configuration file amends */
static const int8_t aic_txpower_default[AIC_TXPWR_ENTRIES] = {
	1,
	20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16,
	20, 20, 20, 20, 18, 18, 16, 16, 16, 16,
	20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15,
	-128, -128, -128, -128, 20, 20, 20, 20, 18, 18, 16, 16,
	20, 20, 20, 20, 18, 18, 16, 16, 16, 15,
	20, 20, 20, 20, 18, 18, 16, 16, 16, 15, 14, 14
};


//	#pragma mark - helpers


static inline void
put16(uint8_t* p, uint16_t value)
{
	p[0] = value & 0xff;
	p[1] = value >> 8;
}


static inline void
put32(uint8_t* p, uint32_t value)
{
	p[0] = value & 0xff;
	p[1] = (value >> 8) & 0xff;
	p[2] = (value >> 16) & 0xff;
	p[3] = value >> 24;
}


static inline uint16_t
get16(const uint8_t* p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}


static inline uint32_t
get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}


/*	One firmware command and its confirmation. Giant, if held, is given up
	for the wait, as tsleep() does: the confirmation does not need it, but
	received frames and the timers do. */
static int
aic_cmd(struct aic_softc* sc, uint16_t id, uint16_t task,
	const void* parameters, uint16_t length, uint16_t confirmId,
	void* confirm, size_t capacity)
{
	int giant = mtx_owned(&Giant);
	status_t status;

	if (giant)
		mtx_unlock(&Giant);
	status = aic_usb_request(id, task, parameters, length, confirmId,
		confirm, capacity, NULL, AIC_COMMAND_TIMEOUT);
	if (giant)
		mtx_lock(&Giant);

	if (status == B_TIMED_OUT && ++sc->sc_timeouts >= 2
		&& !sc->sc_recovering) {
		// The firmware no longer answers. It does not come back by
		// itself: the chip goes back to its ROM, the USB side loads it
		// again, and the driver starts over when it is back.
		printf("%s: the firmware does not answer; restarting the chip\n",
			DEVNAME(sc));
		sc->sc_recovering = 1;
		aic_usb_reboot_chip();
	} else if (status == B_OK)
		sc->sc_timeouts = 0;

	return status == B_OK ? 0 : EIO;
}


static void
aic_wake(struct aic_softc* sc)
{
	int32 count;

	if (get_sem_count(sc->sc_wake, &count) == B_OK && count <= 0)
		release_sem_etc(sc->sc_wake, 1, B_DO_NOT_RESCHEDULE);
}


/*	Leaves work for the driver's thread. Called under Giant. */
static int
aic_queue_work(struct aic_softc* sc, enum aic_work_type type,
	const void* data, size_t length)
{
	struct aic_work* work;

	if (length > AIC_WORK_DATA)
		return EINVAL;

	mtx_enter(&sc->sc_lock);
	if (sc->sc_work_count == AIC_WORK_COUNT) {
		mtx_leave(&sc->sc_lock);
		printf("%s: work ring full, dropping %d\n", DEVNAME(sc), type);
		return ENOBUFS;
	}
	work = &sc->sc_work[sc->sc_work_head];
	work->type = type;
	work->length = length;
	if (length > 0)
		memcpy(work->data, data, length);
	sc->sc_work_head = (sc->sc_work_head + 1) % AIC_WORK_COUNT;
	sc->sc_work_count++;
	mtx_leave(&sc->sc_lock);

	aic_wake(sc);
	return 0;
}


//	#pragma mark - firmware set up


static int
aic_parse_index(const char* text, size_t length, const char* const* names,
	int count)
{
	int i;

	if (names != NULL) {
		for (i = 0; i < count; i++) {
			if (strlen(names[i]) == length
				&& strncmp(names[i], text, length) == 0)
				return i;
		}
		return -1;
	}

	if (length == 0 || length > 2)
		return -1;
	i = 0;
	while (length-- > 0) {
		if (*text < '0' || *text > '9')
			return -1;
		i = i * 10 + (*text++ - '0');
	}
	return i < count ? i : -1;
}


static int
aic_parse_number(const char* text)
{
	int negative = *text == '-';
	int value = 0;

	if (negative)
		text++;
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return negative ? -value : value;
}


/*	aic_userconfig_8800d80.txt: "key=value" lines, of which this driver
	uses the transmit power levels (the only enabled section of the file the
	vendor ships) and the losses subtracted from them. */
static void
aic_load_userconfig(struct aic_softc* sc)
{
	static const char* const legacy[12] = {
		"1m", "2m", "5m5", "11m", "6m", "9m", "12m", "18m", "24m", "36m",
		"48m", "54m"
	};
	static const struct {
		const char*	prefix;
		const char*	suffix;
		int			offset;
		int			count;
		int			legacy;
	} groups[] = {
		{ "lvl_11b_11ag_", "_2g4", AIC_TXPWR_11B_11AG_2G4, 12, 1 },
		{ "lvl_11n_11ac_mcs", "_2g4", AIC_TXPWR_11N_11AC_2G4, 10, 0 },
		{ "lvl_11ax_mcs", "_2g4", AIC_TXPWR_11AX_2G4, 12, 0 },
		{ "lvl_11a_", "_5g", AIC_TXPWR_11A_5G, 12, 1 },
		{ "lvl_11n_11ac_mcs", "_5g", AIC_TXPWR_11N_11AC_5G, 10, 0 },
		{ "lvl_11ax_mcs", "_5g", AIC_TXPWR_11AX_5G, 12, 0 },
	};
	int loss2g = 0, loss5g = 0, lossEnable2g = 0, lossEnable5g = 0;
	uint8* data;
	size_t size;
	char* line;
	int i;

	memcpy(sc->sc_txpower, aic_txpower_default, sizeof(sc->sc_txpower));
	if (aic_usb_read_file("aic_userconfig_8800d80.txt", &data, &size)
			!= B_OK) {
		printf("%s: no user configuration, default power levels\n",
			DEVNAME(sc));
		return;
	}

	for (line = (char*)data; line != NULL && *line != '\0';) {
		char* next = strchr(line, '\n');
		char* equals;
		size_t keyLength;
		int value;

		if (next != NULL)
			*next++ = '\0';
		equals = strchr(line, '=');
		if (line[0] == '#' || equals == NULL) {
			line = next;
			continue;
		}
		keyLength = equals - line;
		value = aic_parse_number(equals + 1);

		if (keyLength == 6 && strncmp(line, "enable", 6) == 0)
			sc->sc_txpower[AIC_TXPWR_ENABLE] = value;
		else if (strncmp(line, "loss_enable_2g4", keyLength) == 0)
			lossEnable2g = value;
		else if (strncmp(line, "loss_value_2g4", keyLength) == 0)
			loss2g = value;
		else if (strncmp(line, "loss_enable_5g", keyLength) == 0)
			lossEnable5g = value;
		else if (strncmp(line, "loss_value_5g", keyLength) == 0)
			loss5g = value;
		else {
			for (i = 0; i < (int)nitems(groups); i++) {
				size_t prefix = strlen(groups[i].prefix);
				size_t suffix = strlen(groups[i].suffix);
				int index;

				if (keyLength <= prefix + suffix
					|| strncmp(line, groups[i].prefix, prefix) != 0
					|| strncmp(line + keyLength - suffix, groups[i].suffix,
						suffix) != 0)
					continue;
				index = aic_parse_index(line + prefix,
					keyLength - prefix - suffix,
					groups[i].legacy ? legacy : NULL, groups[i].count);
				if (index >= 0)
					sc->sc_txpower[groups[i].offset + index] = value;
				break;
			}
		}
		line = next;
	}
	free(data, M_DEVBUF, 0);

	for (i = AIC_TXPWR_11B_11AG_2G4; lossEnable2g && i < AIC_TXPWR_11A_5G;
			i++)
		sc->sc_txpower[i] -= loss2g;
	for (i = AIC_TXPWR_11A_5G; lossEnable5g && i < AIC_TXPWR_ENTRIES; i++) {
		if (sc->sc_txpower[i] != -128)
			sc->sc_txpower[i] -= loss5g;
	}
}


static void
aic_put_channel(uint8_t* p, uint8_t channel, int band)
{
	put16(p, ieee80211_ieee2mhz(channel,
		band == AIC_BAND_5GHZ ? IEEE80211_CHAN_5GHZ : IEEE80211_CHAN_2GHZ));
	p[2] = band;
	p[3] = 0;
	p[4] = AIC_CHANNEL_POWER;
}


/*	The station configuration and the channel list; sent after every
	reset. */
static int
aic_configure(struct aic_softc* sc)
{
	uint8_t config[AIC_ME_CONFIG_SIZE];
	uint8_t channels[AIC_CHAN_CONFIG_SIZE];
	int i;

	// 1x1 HT: LDPC, 20/40 MHz, short guard intervals, one RX STBC stream,
	// 7935-byte A-MSDUs; A-MPDUs up to 64 KiB with 16 us spacing; MCS 0-7
	// and 32, 150 Mb/s at most.
	memset(config, 0, sizeof(config));
	put16(config + AIC_ME_HT_CAPA_INFO, 0x0001 | 0x0002 | 0x0020 | 0x0040
		| 0x0100 | 0x0800);
	config[AIC_ME_HT_AMPDU_PARAM] = 0x03 | (0x07 << 2);
	config[AIC_ME_HT_MCS + 0] = 0xff;
	config[AIC_ME_HT_MCS + 4] = 0x01;
	put16(config + AIC_ME_HT_MCS + 10, 150);
	config[AIC_ME_HT_MCS + 12] = 0x01;
	put16(config + AIC_ME_TX_LIFETIME, 1000);
	config[AIC_ME_PHY_BW_MAX] = 1;
	config[AIC_ME_HT_SUPPORTED] = 1;
	if (sc->sc_vht) {
		// 1x1 VHT at 80 MHz (5 GHz): 1 MiB A-MPDUs, 7991-byte A-MSDUs,
		// RX LDPC, short guard interval, one RX STBC stream; MCS 0-9 on
		// one stream, 433 Mb/s at most
		put32(config + AIC_ME_VHT_CAPA_INFO, (7 << 23) | 0x01 | 0x10
			| 0x20 | 0x100);
		put16(config + AIC_ME_VHT_RX_MCS_MAP, 0xfffe);
		put16(config + AIC_ME_VHT_RX_HIGHEST, 433);
		put16(config + AIC_ME_VHT_TX_MCS_MAP, 0xfffe);
		put16(config + AIC_ME_VHT_TX_HIGHEST, 433);
		config[AIC_ME_VHT_SUPPORTED] = 1;
		config[AIC_ME_PHY_BW_MAX] = 2;
	}
	// Power save off: the boards are fed from the mains, and asleep the
	// chip answers late.

	config[AIC_ME_PS_ON] = 0;
	config[AIC_ME_ANT_DIV_ON] = 1;
	if (aic_cmd(sc, AIC_ME_CONFIG_REQ, AIC_TASK_ME, config, sizeof(config),
			AIC_ME_CONFIG_CFM, NULL, 0) != 0) {
		printf("%s: ME_CONFIG failed\n", DEVNAME(sc));
		return EIO;
	}

	memset(channels, 0, sizeof(channels));
	for (i = 0; i < (int)nitems(aic_channels_2ghz); i++) {
		aic_put_channel(channels + AIC_CHAN_CONFIG_2G4 + i * AIC_CHAN_SIZE,
			aic_channels_2ghz[i], AIC_BAND_2GHZ);
	}
	for (i = 0; i < (int)nitems(aic_channels_5ghz); i++) {
		aic_put_channel(channels + AIC_CHAN_CONFIG_5G + i * AIC_CHAN_SIZE,
			aic_channels_5ghz[i], AIC_BAND_5GHZ);
	}
	channels[AIC_CHAN_CONFIG_2G4_COUNT] = nitems(aic_channels_2ghz);
	channels[AIC_CHAN_CONFIG_5G_COUNT] = nitems(aic_channels_5ghz);
	if (aic_cmd(sc, AIC_ME_CHAN_CONFIG_REQ, AIC_TASK_ME, channels,
			sizeof(channels), AIC_ME_CHAN_CONFIG_CFM, NULL, 0) != 0) {
		printf("%s: ME_CHAN_CONFIG failed\n", DEVNAME(sc));
		return EIO;
	}
	return 0;
}


/*	The firmware's set up after it was started, up to the MAC address:
	everything but the interface. */
static int
aic_preinit(struct aic_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	uint8_t parameters[AIC_TXPWR_LVL_SIZE];
	uint8_t confirm[64];
	uint32_t chip;

	put32(parameters, 0x40500000);
	if (aic_cmd(sc, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG, parameters, 4,
			AIC_DBG_MEM_READ_CFM, confirm, 8) != 0) {
		printf("%s: the firmware does not answer\n", DEVNAME(sc));
		return EIO;
	}
	chip = get32(confirm + 4);

	// the host's stack runs (there is no efuse data to apply), and the
	// firmware reports whether 5 GHz is there
	parameters[0] = 1;
	parameters[1] = 0;
	parameters[2] = 0x20;
	parameters[3] = 0;
	if (aic_cmd(sc, AIC_MM_SET_STACK_START_REQ, AIC_TASK_MM, parameters,
			AIC_STACK_START_SIZE, AIC_MM_SET_STACK_START_CFM, confirm, 2)
			!= 0)
		return EIO;
	sc->sc_5ghz = confirm[0];

	memset(confirm, 0, sizeof(confirm));
	parameters[0] = 0;
	if (aic_cmd(sc, AIC_MM_GET_FW_VERSION_REQ, AIC_TASK_MM, parameters, 1,
			AIC_MM_GET_FW_VERSION_CFM, confirm, sizeof(confirm)) != 0)
		return EIO;
	confirm[63] = '\0';
	printf("%s: chip %#010x, firmware \"%.*s\"%s\n", DEVNAME(sc), chip,
		confirm[0] < 63 ? confirm[0] : 63, (const char*)confirm + 1,
		sc->sc_5ghz ? ", 5 GHz" : "");

	aic_load_userconfig(sc);
	if (sc->sc_txpower[AIC_TXPWR_ENABLE] != 0) {
		memset(parameters, 0, sizeof(parameters));
		memcpy(parameters, sc->sc_txpower, sizeof(sc->sc_txpower));
		if (aic_cmd(sc, AIC_MM_SET_TXPWR_LVL_REQ, AIC_TASK_MM, parameters,
				AIC_TXPWR_LVL_SIZE, AIC_MM_SET_TXPWR_LVL_CFM, NULL, 0) != 0)
			return EIO;
	}

	// RF calibration: both bands, the vendor's constants for this chip
	memset(parameters, 0, sizeof(parameters));
	put32(parameters + 0, 0x0f8f);
	put32(parameters + 4, 0x0f0f);
	put32(parameters + 8, 0x0c34c008);
	put32(parameters + 12, 0);
	put32(parameters + 16, 0x264203);
	if (aic_cmd(sc, AIC_MM_SET_RF_CALIB_REQ, AIC_TASK_MM, parameters,
			AIC_RF_CALIB_SIZE, AIC_MM_SET_RF_CALIB_CFM, confirm, 16) != 0)
		return EIO;

	put32(parameters, 1);
	memset(confirm, 0, 6);
	if (aic_cmd(sc, AIC_MM_GET_MAC_ADDR_REQ, AIC_TASK_MM, parameters, 4,
			AIC_MM_GET_MAC_ADDR_CFM, confirm, 6) != 0)
		return EIO;
	if (get32(confirm) == 0) {
		// no efuse address: a locally administered one
		uint32_t random = (uint32_t)system_time() ^ chip;
		confirm[0] = 0x8a;
		confirm[1] = 0x00;
		confirm[2] = 0x33;
		confirm[3] = random >> 16;
		confirm[4] = random >> 8;
		confirm[5] = random;
	}
	memcpy(ic->ic_myaddr, confirm, IEEE80211_ADDR_LEN);

	if (aic_cmd(sc, AIC_MM_RESET_REQ, AIC_TASK_MM, NULL, 0, AIC_MM_RESET_CFM,
			NULL, 0) != 0)
		return EIO;

	memset(confirm, 0, AIC_VERSION_SIZE);
	if (aic_cmd(sc, AIC_MM_VERSION_REQ, AIC_TASK_MM, NULL, 0,
			AIC_MM_VERSION_CFM, confirm, AIC_VERSION_SIZE) != 0)
		return EIO;
	sc->sc_phy_2 = get32(confirm + AIC_VERSION_PHY_2);
	DPRINTF("%s: lmac %#x, phy %#x, features %#x, %u stations\n",
		DEVNAME(sc), get32(confirm + AIC_VERSION_LMAC),
		get32(confirm + AIC_VERSION_PHY_1),
		get32(confirm + AIC_VERSION_FEATURES),
		get16(confirm + AIC_VERSION_MAX_STA));

	return aic_configure(sc);
}


//	#pragma mark - interface up and down


static void
aic_init(struct ifnet* ifp)
{
	struct aic_softc* sc = ifp->if_softc;
	struct ieee80211com* ic = &sc->sc_ic;
	uint8_t parameters[AIC_START_SIZE];
	uint8_t confirm[4];

	if (!sc->sc_attached || aic_usb_gone())
		return;

	// the MAC: no PHY settings, a U-APSD timeout of 300 ms, 20 ppm clock
	memset(parameters, 0, sizeof(parameters));
	put32(parameters + AIC_START_UAPSD_TIMEOUT, 300);
	put16(parameters + AIC_START_LP_CLK_ACCURACY, 20);
	if (aic_cmd(sc, AIC_MM_START_REQ, AIC_TASK_MM, parameters,
			AIC_START_SIZE, AIC_MM_START_CFM, NULL, 0) != 0) {
		printf("%s: MM_START failed\n", DEVNAME(sc));
		return;
	}
	sc->sc_started = 1;

	// share the antenna with Bluetooth
	memset(parameters, 0, AIC_COEX_SIZE);
	parameters[0] = 1;
	parameters[2] = 1;
	aic_cmd(sc, AIC_MM_SET_COEX_REQ, AIC_TASK_MM, parameters, AIC_COEX_SIZE,
		AIC_MM_SET_COEX_CFM, NULL, 0);

	IEEE80211_ADDR_COPY(ic->ic_myaddr, IF_LLADDR(ifp));
	memset(parameters, 0, AIC_ADD_IF_SIZE);
	parameters[0] = AIC_IF_TYPE_STA;
	memcpy(parameters + 2, ic->ic_myaddr, IEEE80211_ADDR_LEN);
	memset(confirm, 0xff, sizeof(confirm));
	if (aic_cmd(sc, AIC_MM_ADD_IF_REQ, AIC_TASK_MM, parameters,
			AIC_ADD_IF_SIZE, AIC_MM_ADD_IF_CFM, confirm, 2) != 0
		|| confirm[0] != 0) {
		printf("%s: MM_ADD_IF failed (%u)\n", DEVNAME(sc), confirm[0]);
		return;
	}
	sc->sc_vif = confirm[1];

	parameters[0] = 0;
	aic_cmd(sc, AIC_ME_SET_PS_MODE_REQ, AIC_TASK_ME, parameters, 1,
		AIC_ME_SET_PS_MODE_CFM, NULL, 0);

	DPRINTF("%s: up, interface %d\n", DEVNAME(sc), sc->sc_vif);

	ic->ic_bss->ni_chan = ic->ic_ibss_chan;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ieee80211_begin_scan(ifp);
}


static void aic_reorder_flush(struct aic_softc*, int, struct mbuf_list*);


static void
aic_forget_association(struct aic_softc* sc)
{
	struct mbuf_list held = MBUF_LIST_INITIALIZER();

	aic_reorder_flush(sc, 1, &held);
	ml_purge(&held);
	sc->sc_connected = 0;
	sc->sc_ap = -1;
	sc->sc_key_tasks = 0;
	memset(sc->sc_hw_key, 0xff, sizeof(sc->sc_hw_key));
	if (sc->sc_early_eapol != NULL) {
		m_freem(sc->sc_early_eapol);
		sc->sc_early_eapol = NULL;
	}
}


static void
aic_stop(struct ifnet* ifp)
{
	struct aic_softc* sc = ifp->if_softc;
	struct ieee80211com* ic = &sc->sc_ic;
	uint8_t parameters[AIC_DISCONNECT_SIZE];

	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	// nothing the hooks left behind may run on a stopped interface
	mtx_enter(&sc->sc_lock);
	sc->sc_work_head = sc->sc_work_tail = sc->sc_work_count = 0;
	mtx_leave(&sc->sc_lock);

	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);

	if (aic_usb_gone()) {
		aic_forget_association(sc);
		sc->sc_vif = -1;
		sc->sc_started = 0;
		sc->sc_scanning = 0;
		return;
	}

	if (sc->sc_scanning) {
		aic_cmd(sc, AIC_SCANU_CANCEL_REQ, AIC_TASK_SCANU, NULL, 0,
			AIC_SCANU_CANCEL_CFM, NULL, 0);
		sc->sc_scanning = 0;
	}
	if (sc->sc_connected && sc->sc_vif >= 0) {
		sc->sc_connected = 0;
		put16(parameters, IEEE80211_REASON_AUTH_LEAVE);
		parameters[2] = sc->sc_vif;
		parameters[3] = 0;
		aic_cmd(sc, AIC_SM_DISCONNECT_REQ, AIC_TASK_SM, parameters,
			AIC_DISCONNECT_SIZE, AIC_SM_DISCONNECT_CFM, NULL, 0);
	}
	aic_forget_association(sc);

	if (sc->sc_vif >= 0) {
		parameters[0] = sc->sc_vif;
		aic_cmd(sc, AIC_MM_REMOVE_IF_REQ, AIC_TASK_MM, parameters, 1,
			AIC_MM_REMOVE_IF_CFM, NULL, 0);
		sc->sc_vif = -1;
	}
	if (sc->sc_started) {
		aic_cmd(sc, AIC_MM_RESET_REQ, AIC_TASK_MM, NULL, 0,
			AIC_MM_RESET_CFM, NULL, 0);
		sc->sc_started = 0;
		aic_configure(sc);
	}
	DPRINTF("%s: down\n", DEVNAME(sc));
}


static int
aic_ioctl(struct ifnet* ifp, u_long cmd, caddr_t data)
{
	struct aic_softc* sc = ifp->if_softc;
	struct ieee80211com* ic = &sc->sc_ic;
	int error = 0;

	switch (cmd) {
		case SIOCSIFADDR:
			ifp->if_flags |= IFF_UP;
			/* FALLTHROUGH */
		case SIOCSIFFLAGS:
			if ((ifp->if_flags & IFF_UP) != 0) {
				if ((ifp->if_flags & IFF_RUNNING) == 0)
					aic_init(ifp);
			} else if ((ifp->if_flags & IFF_RUNNING) != 0)
				aic_stop(ifp);
			break;

		case SIOCADDMULTI:
		case SIOCDELMULTI:
			// the firmware passes all multicast frames; the stack filters
			break;

		default:
			error = ieee80211_ioctl(ifp, cmd, data);
			break;
	}

	if (error == ENETRESET) {
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING))
				== (IFF_UP | IFF_RUNNING)) {
			aic_stop(ifp);
			aic_init(ifp);
		}
		error = 0;
	}
	return error;
}


static int
aic_media_change(struct ifnet* ifp)
{
	int error = ieee80211_media_change(ifp);
	if (error != ENETRESET)
		return error;

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) {
		aic_stop(ifp);
		aic_init(ifp);
	}
	return 0;
}


//	#pragma mark - scanning


static void
aic_do_scan(struct aic_softc* sc, const uint8_t* ssid, size_t ssidLength)
{
	uint8_t request[AIC_SCAN_SIZE];
	uint8_t confirm[4];
	int count = 0, i;

	if (sc->sc_vif < 0 || sc->sc_scanning)
		return;

	memset(request, 0, sizeof(request));
	for (i = 0; i < (int)nitems(aic_channels_2ghz); i++) {
		aic_put_channel(request + AIC_SCAN_CHANNELS + count++ * AIC_CHAN_SIZE,
			aic_channels_2ghz[i], AIC_BAND_2GHZ);
	}
	for (i = 0; sc->sc_5ghz && i < (int)nitems(aic_channels_5ghz); i++) {
		aic_put_channel(request + AIC_SCAN_CHANNELS + count++ * AIC_CHAN_SIZE,
			aic_channels_5ghz[i], AIC_BAND_5GHZ);
	}
	request[AIC_SCAN_CHANNEL_COUNT] = count;

	// any network, and the wanted one by name (a hidden one answers only
	// to that)
	request[AIC_SCAN_SSID_COUNT] = 1;
	if (ssidLength > 0 && ssidLength <= IEEE80211_NWID_LEN) {
		request[AIC_SCAN_SSIDS + AIC_SSID_SIZE] = ssidLength;
		memcpy(request + AIC_SCAN_SSIDS + AIC_SSID_SIZE + 1, ssid,
			ssidLength);
		request[AIC_SCAN_SSID_COUNT] = 2;
	}
	memset(request + AIC_SCAN_BSSID, 0xff, IEEE80211_ADDR_LEN);
	request[AIC_SCAN_VIF] = sc->sc_vif;

	memset(confirm, 0xff, sizeof(confirm));
	if (aic_cmd(sc, AIC_SCANU_START_REQ, AIC_TASK_SCANU, request,
			sizeof(request), AIC_SCANU_ACCEPTED_CFM, confirm,
			sizeof(confirm)) != 0) {
		printf("%s: the scan was not accepted\n", DEVNAME(sc));
		return;
	}
	sc->sc_scanning = 1;
	sc->sc_scan_started = system_time();
	sc->sc_scan_results = 0;
	sc->sc_scan_results_5ghz = 0;
}


static void
aic_scan_result(struct aic_softc* sc, const uint8_t* result, size_t length)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;
	struct ieee80211_rxinfo rxi;
	struct ieee80211_frame* wh;
	struct ieee80211_node* ni;
	struct mbuf* m;
	size_t frameLength;
	uint16_t frequency;
	int band;

	if (ic->ic_state != IEEE80211_S_SCAN
		&& !(ic->ic_state == IEEE80211_S_RUN
			&& (ic->ic_flags & IEEE80211_F_BGSCAN) != 0))
		return;
	if (length < AIC_RESULT_FRAME)
		return;

	frameLength = get16(result + AIC_RESULT_LENGTH);
	if (frameLength > length - AIC_RESULT_FRAME)
		frameLength = length - AIC_RESULT_FRAME;
	if (frameLength < sizeof(struct ieee80211_frame) + 12)
		return;
	frequency = get16(result + AIC_RESULT_FREQUENCY);
	band = result[AIC_RESULT_BAND];

	// the beacon or probe response as the firmware heard it
	m = MCLGETL(NULL, M_DONTWAIT, frameLength);
	if (m == NULL)
		return;
	memcpy(mtod(m, uint8_t*), result + AIC_RESULT_FRAME, frameLength);
	m->m_pkthdr.len = m->m_len = frameLength;
	wh = mtod(m, struct ieee80211_frame*);
	sc->sc_scan_results++;
	if (band == AIC_BAND_5GHZ)
		sc->sc_scan_results_5ghz++;

	ni = ieee80211_find_rxnode(ic, wh);
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = (int8_t)result[AIC_RESULT_RSSI];
	rxi.rxi_chan = ieee80211_mhz2ieee(frequency,
		band == AIC_BAND_5GHZ ? IEEE80211_CHAN_5GHZ : IEEE80211_CHAN_2GHZ);
	ieee80211_input(ifp, m, ni, &rxi);
	ieee80211_release_node(ic, ni);
}


static void
aic_scan_done(struct aic_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;

	if (!sc->sc_scanning)
		return;
	sc->sc_scanning = 0;
	if (aic_debug && system_time() - sc->sc_scan_logged > 30000000) {
		// net80211 scans back to back while it finds nothing to join
		sc->sc_scan_logged = system_time();
		printf("%s: scan done, %d results (%d at 5 GHz) in %" B_PRIdBIGTIME
			" ms\n", DEVNAME(sc), sc->sc_scan_results,
			sc->sc_scan_results_5ghz,
			(system_time() - sc->sc_scan_started) / 1000);
	}

	if (ic->ic_state == IEEE80211_S_SCAN
		|| (ic->ic_state == IEEE80211_S_RUN
			&& (ic->ic_flags & IEEE80211_F_BGSCAN) != 0))
		ieee80211_end_scan(ifp);
}


static int
aic_bgscan(struct ieee80211com* ic)
{
	struct aic_softc* sc = ic->ic_softc;

	if (sc->sc_vif < 0)
		return ENXIO;

	// the firmware scans next to the association by itself
	return aic_queue_work(sc, AIC_WORK_SCAN, NULL, 0);
}


//	#pragma mark - connecting


static void
aic_do_connect(struct aic_softc* sc, uint8_t* request, size_t length)
{
	struct ieee80211com* ic = &sc->sc_ic;
	uint8_t parameters[AIC_DISCONNECT_SIZE];
	uint8_t confirm[4];

	if (sc->sc_vif < 0 || ic->ic_state != IEEE80211_S_AUTH)
		return;

	if (sc->sc_scanning) {
		aic_cmd(sc, AIC_SCANU_CANCEL_REQ, AIC_TASK_SCANU, NULL, 0,
			AIC_SCANU_CANCEL_CFM, NULL, 0);
		sc->sc_scanning = 0;
	}

	// a connection the firmware still has would make it refuse this one
	if (sc->sc_connected) {
		sc->sc_connected = 0;
		put16(parameters, IEEE80211_REASON_AUTH_LEAVE);
		parameters[2] = sc->sc_vif;
		parameters[3] = 0;
		aic_cmd(sc, AIC_SM_DISCONNECT_REQ, AIC_TASK_SM, parameters,
			AIC_DISCONNECT_SIZE, AIC_SM_DISCONNECT_CFM, NULL, 0);
	}
	aic_forget_association(sc);
	if (ic->ic_state != IEEE80211_S_AUTH)
		return;

	request[AIC_CONNECT_VIF] = sc->sc_vif;
	memset(confirm, 0xff, sizeof(confirm));
	if (aic_cmd(sc, AIC_SM_CONNECT_REQ, AIC_TASK_SM, request, length,
			AIC_SM_CONNECT_CFM, confirm, 1) != 0 || confirm[0] != 0) {
		printf("%s: the connect request failed (%u)\n", DEVNAME(sc),
			confirm[0]);
		if (ic->ic_state == IEEE80211_S_AUTH)
			ieee80211_begin_scan(&ic->ic_if);
		return;
	}
	sc->sc_connecting = 1;
}


/*	net80211 has chosen a network: the firmware connects to it, with the
	RSN element net80211 will expect in the handshake. */
static void
aic_connect(struct aic_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_node* ni = ic->ic_bss;
	uint8_t request[AIC_CONNECT_SIZE];
	uint32_t flags = 0;
	size_t ieLength = 0;
	const uint8_t* ssid = ni->ni_essid;
	size_t ssidLength = ni->ni_esslen;

	if (ssidLength == 0) {
		ssid = ic->ic_des_essid;
		ssidLength = ic->ic_des_esslen;
	}

	memset(request, 0, sizeof(request));
	request[AIC_CONNECT_SSID] = ssidLength;
	memcpy(request + AIC_CONNECT_SSID + 1, ssid, ssidLength);
	memcpy(request + AIC_CONNECT_BSSID, ni->ni_bssid, IEEE80211_ADDR_LEN);
	if (ni->ni_chan != NULL && ni->ni_chan != IEEE80211_CHAN_ANYC
		&& ni->ni_chan->ic_freq != 0) {
		put16(request + AIC_CONNECT_CHANNEL, ni->ni_chan->ic_freq);
		request[AIC_CONNECT_CHANNEL + 2]
			= IEEE80211_IS_CHAN_5GHZ(ni->ni_chan)
				? AIC_BAND_5GHZ : AIC_BAND_2GHZ;
		request[AIC_CONNECT_CHANNEL + 4] = AIC_CHANNEL_POWER;
	} else
		put16(request + AIC_CONNECT_CHANNEL, 0xffff);

	if ((ic->ic_flags & IEEE80211_F_RSNON) != 0) {
		uint8_t* end;
		if (ni->ni_rsnprotos == IEEE80211_PROTO_RSN)
			end = ieee80211_add_rsn(request + AIC_CONNECT_IE, ic, ni);
		else
			end = ieee80211_add_wpa(request + AIC_CONNECT_IE, ic, ni);
		ieLength = end - (request + AIC_CONNECT_IE);
		flags = AIC_CONTROL_PORT_HOST | AIC_WPA_WPA2_IN_USE;
		if (ni->ni_rsncipher == IEEE80211_CIPHER_TKIP)
			flags |= AIC_DISABLE_HT;
	}
	put32(request + AIC_CONNECT_FLAGS, flags);
	request[AIC_CONNECT_ETHERTYPE] = 0x88;
	request[AIC_CONNECT_ETHERTYPE + 1] = 0x8e;
	put16(request + AIC_CONNECT_IE_LENGTH, ieLength);
	request[AIC_CONNECT_AUTH_TYPE] = 0;
		// open system
	request[AIC_CONNECT_UAPSD] = 0;
		// no power save, so no U-APSD either

	printf("%s: connecting to %s on %u MHz\n", DEVNAME(sc),
		ether_sprintf(ni->ni_bssid),
		ni->ni_chan != NULL ? ni->ni_chan->ic_freq : 0);
	aic_queue_work(sc, AIC_WORK_CONNECT, request, sizeof(request));
}


static void
aic_connected(struct aic_softc* sc, const uint8_t* indication, size_t length)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_node* ni = ic->ic_bss;
	uint8_t parameters[2];
	struct mbuf* m;
	uint16_t status;

	if (length < AIC_CONNECTED_MIN)
		return;
	sc->sc_connecting = 0;
	status = get16(indication + AIC_CONNECTED_STATUS);

	if (ic->ic_state != IEEE80211_S_AUTH) {
		if (status == 0) {
			// net80211 has given up on it meanwhile
			sc->sc_connected = 1;
			sc->sc_ap = indication[AIC_CONNECTED_AP];
			aic_queue_work(sc, AIC_WORK_DISCONNECT, NULL, 0);
		}
		return;
	}

	if (status != 0) {
		printf("%s: association refused (status %u)\n", DEVNAME(sc), status);
		ni->ni_fails++;
		ieee80211_begin_scan(&ic->ic_if);
		return;
	}

	sc->sc_connected = 1;
	sc->sc_ap = indication[AIC_CONNECTED_AP];
	sc->sc_qos = indication[AIC_CONNECTED_QOS];
	sc->sc_key_tasks = 0;
	ni->ni_associd = get16(indication + AIC_CONNECTED_AID) | 0xc000;
	printf("%s: associated with %s (station %d, aid %u%s)\n", DEVNAME(sc),
		ether_sprintf(indication + AIC_CONNECTED_BSSID), sc->sc_ap,
		ni->ni_associd & 0x3fff, sc->sc_qos ? ", QoS" : "");

	// OpenBSD's state machine wants ASSOC between AUTH and RUN
	ieee80211_new_state(ic, IEEE80211_S_ASSOC, -1);
	ieee80211_new_state(ic, IEEE80211_S_RUN, -1);

	if ((ic->ic_flags & IEEE80211_F_RSNON) == 0) {
		parameters[0] = sc->sc_ap;
		parameters[1] = 1;
		aic_cmd(sc, AIC_ME_SET_CONTROL_PORT_REQ, AIC_TASK_ME, parameters, 2,
			AIC_ME_SET_CONTROL_PORT_CFM, NULL, 0);
		ni->ni_port_valid = 1;
		return;
	}

	// The AP's first handshake message may have overtaken the firmware's
	// news of the association.
	m = sc->sc_early_eapol;
	sc->sc_early_eapol = NULL;
	if (m != NULL && ic->ic_state == IEEE80211_S_RUN)
		ieee80211_eapol_key_input(ic, m, ni);
	else if (m != NULL)
		m_freem(m);
}


static void
aic_disconnected(struct aic_softc* sc, const uint8_t* indication,
	size_t length)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;

	if (!sc->sc_connected) {
		// one we asked for
		return;
	}

	printf("%s: disconnected (reason %u)\n", DEVNAME(sc),
		length >= 2 ? get16(indication) : 0);
	aic_forget_association(sc);
	if ((ifp->if_flags & IFF_RUNNING) != 0
		&& ic->ic_state > IEEE80211_S_SCAN)
		ieee80211_begin_scan(ifp);
}


static void
aic_do_disconnect(struct aic_softc* sc)
{
	uint8_t parameters[AIC_DISCONNECT_SIZE];

	if (!sc->sc_connected || sc->sc_vif < 0)
		return;
	sc->sc_connected = 0;
	put16(parameters, IEEE80211_REASON_AUTH_LEAVE);
	parameters[2] = sc->sc_vif;
	parameters[3] = 0;
	aic_cmd(sc, AIC_SM_DISCONNECT_REQ, AIC_TASK_SM, parameters,
		AIC_DISCONNECT_SIZE, AIC_SM_DISCONNECT_CFM, NULL, 0);
	aic_forget_association(sc);
}


static int
aic_newstate(struct ieee80211com* ic, enum ieee80211_state nstate, int arg)
{
	struct aic_softc* sc = ic->ic_softc;
	struct ifnet* ifp = &ic->ic_if;
	uint8_t ssid[1 + IEEE80211_NWID_LEN];

	if ((ifp->if_flags & IFF_DEBUG) != 0) {
		printf("%s: %s -> %s\n", DEVNAME(sc),
			ieee80211_state_name[ic->ic_state],
			ieee80211_state_name[nstate]);
	}

	switch (nstate) {
		case IEEE80211_S_INIT:
			if (sc->sc_scanning)
				aic_queue_work(sc, AIC_WORK_SCAN_CANCEL, NULL, 0);
			if (sc->sc_connected || sc->sc_connecting)
				aic_queue_work(sc, AIC_WORK_DISCONNECT, NULL, 0);
			break;

		case IEEE80211_S_SCAN:
			// a connection being made is given up
			if (ic->ic_state > IEEE80211_S_SCAN)
				aic_queue_work(sc, AIC_WORK_DISCONNECT, NULL, 0);
			ssid[0] = ic->ic_des_esslen;
			memcpy(ssid + 1, ic->ic_des_essid, IEEE80211_NWID_LEN);
			aic_queue_work(sc, AIC_WORK_SCAN, ssid, sizeof(ssid));
			if (ic->ic_state == IEEE80211_S_SCAN)
				return 0;
			ieee80211_set_link_state(ic, LINK_STATE_DOWN);
			ieee80211_free_allnodes(ic, 1);
			ic->ic_state = nstate;
			return 0;

		case IEEE80211_S_AUTH:
			ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
			aic_connect(sc);
			ic->ic_state = nstate;
			if ((ic->ic_flags & IEEE80211_F_RSNON) != 0)
				ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
			return 0;

		default:
			break;
	}

	return sc->sc_newstate(ic, nstate, arg);
}


static int
aic_send_mgmt(struct ieee80211com* ic, struct ieee80211_node* ni, int type,
	int arg1, int arg2)
{
	// the firmware sends its own management frames
	return 0;
}


//	#pragma mark - keys


static int
aic_set_key(struct ieee80211com* ic, struct ieee80211_node* ni,
	struct ieee80211_key* k)
{
	struct aic_softc* sc = ic->ic_softc;
	struct aic_key key;

	if ((k->k_flags & IEEE80211_KEY_IGTK) != 0)
		return 0;
			// no management frame protection

	memset(&key, 0, sizeof(key));
	key.pairwise = (k->k_flags & IEEE80211_KEY_GROUP) == 0;
	key.cipher = k->k_cipher;
	key.id = k->k_id;
	key.length = min(k->k_len, sizeof(key.key));
	memcpy(key.key, k->k_key, key.length);
	if (aic_queue_work(sc, AIC_WORK_SET_KEY, &key, sizeof(key)) != 0)
		return ENOBUFS;
	sc->sc_key_tasks++;
	return EBUSY;
}


static void
aic_delete_key(struct ieee80211com* ic, struct ieee80211_node* ni,
	struct ieee80211_key* k)
{
	struct aic_softc* sc = ic->ic_softc;
	struct aic_key key;

	memset(&key, 0, sizeof(key));
	key.pairwise = (k->k_flags & IEEE80211_KEY_GROUP) == 0;
	key.id = k->k_id;
	aic_queue_work(sc, AIC_WORK_DELETE_KEY, &key, sizeof(key));
}


/*	Waits for the firmware to have sent the EAPOL frames queued so far:
	net80211 queues message 4 of the handshake before it installs the
	pairwise key, and messages overtake frames inside the firmware. */
static void
aic_eapol_fence(struct aic_softc* sc)
{
	bigtime_t deadline = system_time() + AIC_EAPOL_FENCE;
	int giant = mtx_owned(&Giant);

	if (giant)
		mtx_unlock(&Giant);
	while ((sc->sc_eapol_confirmed & 0xffff) != (sc->sc_eapol_sent & 0xffff)
		&& system_time() < deadline && !aic_usb_gone()) {
		acquire_sem_etc(sc->sc_eapol_wait, 1, B_ABSOLUTE_TIMEOUT, deadline);
	}
	if (giant)
		mtx_lock(&Giant);

	if ((sc->sc_eapol_confirmed & 0xffff) != (sc->sc_eapol_sent & 0xffff))
		printf("%s: EAPOL frame not confirmed in time\n", DEVNAME(sc));
}


static void
aic_do_set_key(struct aic_softc* sc, const struct aic_key* key)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_node* ni = ic->ic_bss;
	uint8_t request[AIC_KEY_ADD_SIZE];
	uint8_t confirm[2];
	int cipher;

	sc->sc_key_tasks--;
	if (!sc->sc_connected || sc->sc_vif < 0)
		return;

	switch (key->cipher) {
		case IEEE80211_CIPHER_WEP40:
			cipher = AIC_CIPHER_WEP40;
			break;
		case IEEE80211_CIPHER_WEP104:
			cipher = AIC_CIPHER_WEP104;
			break;
		case IEEE80211_CIPHER_TKIP:
			cipher = AIC_CIPHER_TKIP;
			break;
		case IEEE80211_CIPHER_CCMP:
			cipher = AIC_CIPHER_CCMP;
			break;
		default:
			printf("%s: cipher %#x not supported\n", DEVNAME(sc), key->cipher);
			ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
			return;
	}

	if (key->pairwise)
		aic_eapol_fence(sc);

	memset(request, 0, sizeof(request));
	request[AIC_KEY_INDEX] = key->pairwise ? 0 : key->id;
	request[AIC_KEY_STA] = key->pairwise ? sc->sc_ap : 0xff;
	request[AIC_KEY_LENGTH] = key->length;
	memcpy(request + AIC_KEY_DATA, key->key, key->length);
	if (cipher == AIC_CIPHER_TKIP && key->length == 32) {
		// The firmware wants the station's own transmit MIC key first;
		// the key hierarchy has the authenticator's there.
		memcpy(request + AIC_KEY_DATA + 16, key->key + 24, 8);
		memcpy(request + AIC_KEY_DATA + 24, key->key + 16, 8);
	}
	request[AIC_KEY_CIPHER] = cipher;
	request[AIC_KEY_VIF] = sc->sc_vif;
	request[AIC_KEY_PAIRWISE] = key->pairwise;

	memset(confirm, 0xff, sizeof(confirm));
	if (aic_cmd(sc, AIC_MM_KEY_ADD_REQ, AIC_TASK_MM, request, sizeof(request),
			AIC_MM_KEY_ADD_CFM, confirm, sizeof(confirm)) != 0
		|| confirm[0] != 0) {
		printf("%s: installing the %s key failed (%u)\n", DEVNAME(sc),
			key->pairwise ? "pairwise" : "group", confirm[0]);
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		return;
	}
	sc->sc_hw_key[key->pairwise ? 4 : (key->id & 3)] = confirm[1];
	DPRINTF("%s: %s key %d installed (%u)\n", DEVNAME(sc),
		key->pairwise ? "pairwise" : "group", key->id, confirm[1]);

	if (sc->sc_key_tasks > 0 || ni->ni_port_valid
		|| ic->ic_state != IEEE80211_S_RUN
		|| cipher == AIC_CIPHER_WEP40 || cipher == AIC_CIPHER_WEP104)
		return;

	// the last key of the handshake: open the port
	request[0] = sc->sc_ap;
	request[1] = 1;
	if (aic_cmd(sc, AIC_ME_SET_CONTROL_PORT_REQ, AIC_TASK_ME, request, 2,
			AIC_ME_SET_CONTROL_PORT_CFM, NULL, 0) != 0) {
		printf("%s: opening the port failed\n", DEVNAME(sc));
		return;
	}
	printf("%s: keys installed, port open\n", DEVNAME(sc));
	ni->ni_port_valid = 1;
	ieee80211_set_link_state(ic, LINK_STATE_UP);
}


static void
aic_do_delete_key(struct aic_softc* sc, const struct aic_key* key)
{
	int slot = key->pairwise ? 4 : (key->id & 3);
	uint8_t parameters[1];

	if (sc->sc_hw_key[slot] < 0 || sc->sc_hw_key[slot] > 0xff)
		return;
	parameters[0] = sc->sc_hw_key[slot];
	sc->sc_hw_key[slot] = -1;
	if (sc->sc_vif >= 0 && sc->sc_connected) {
		aic_cmd(sc, AIC_MM_KEY_DEL_REQ, AIC_TASK_MM, parameters, 1,
			AIC_MM_KEY_DEL_CFM, NULL, 0);
	}
}


//	#pragma mark - sending


static int
aic_tx(struct aic_softc* sc, struct mbuf* m)
{
	uint8_t* frame = sc->sc_tx_buffer;
	struct ether_header eh;
	size_t payload, length;

	if (m->m_pkthdr.len < (int)sizeof(eh)
		|| m->m_pkthdr.len - sizeof(eh) > AIC_USB_TX_MAX - AIC_TX_HEADER - 8)
		return EINVAL;

	m_copydata(m, 0, sizeof(eh), (caddr_t)&eh);
	payload = m->m_pkthdr.len - sizeof(eh);

	memset(frame, 0, AIC_TX_HEADER);
	m_copydata(m, sizeof(eh), payload, (caddr_t)(frame + AIC_TX_HEADER));

	put16(frame + AIC_TX_LENGTH, payload);
	if (ETHERTYPE_EAPOL_BYTES((uint8_t*)&eh.ether_type)) {
		// the handshake waits for the firmware to have sent it
		sc->sc_eapol_sent = (sc->sc_eapol_sent + 1) & 0xffff;
		put32(frame + AIC_TX_STATUS_ADDRESS,
			AIC_TX_NEED_CONFIRM | sc->sc_eapol_sent);
	}
	memcpy(frame + AIC_TX_DESTINATION, eh.ether_dhost, ETHER_ADDR_LEN);
	memcpy(frame + AIC_TX_SOURCE, eh.ether_shost, ETHER_ADDR_LEN);
	memcpy(frame + AIC_TX_ETHERTYPE, &eh.ether_type, 2);
	frame[AIC_TX_AC] = AIC_AC_BE;
	frame[AIC_TX_TID] = sc->sc_qos ? 0 : 0xff;
	frame[AIC_TX_VIF] = sc->sc_vif;
	frame[AIC_TX_STA] = sc->sc_ap;

	// whole words, and never a multiple of the packet size (no zero-length
	// packet can be asked for to end the transfer)
	length = (AIC_TX_HEADER + payload + 3) & ~(size_t)3;
	if (length % 512 == 0)
		length += 4;
	memset(frame + AIC_TX_HEADER + payload, 0,
		length - AIC_TX_HEADER - payload);
	put16(frame, length & 0x0fff);
	frame[2] = AIC_TYPE_DATA_TX;
	frame[3] = 0;

	return aic_usb_send(frame, length) == B_OK ? 0 : EIO;
}


static void
aic_start(struct ifnet* ifp)
{
	struct aic_softc* sc = ifp->if_softc;
	struct ieee80211com* ic = &sc->sc_ic;
	struct mbuf* m;

	if ((ifp->if_flags & IFF_RUNNING) == 0 || ifq_is_oactive(&ifp->if_snd))
		return;

	for (;;) {
		if (ic->ic_state != IEEE80211_S_RUN
			|| (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY) != 0
			|| sc->sc_ap < 0)
			break;
		if (!aic_usb_send_space()) {
			ifq_set_oactive(&ifp->if_snd);
			break;
		}

		m = ifq_dequeue(&ifp->if_snd);
		if (m == NULL)
			break;
		if (aic_tx(sc, m) != 0)
			ifp->if_oerrors++;
		m_freem(m);
	}
}


//	#pragma mark - receiving


/*	One MSDU as an Ethernet frame, for the stack. */
static void
aic_rx_msdu(struct aic_softc* sc, struct mbuf_list* list,
	const uint8_t* destination, const uint8_t* source, const uint8_t* payload,
	size_t length)
{
	struct ifnet* ifp = &sc->sc_ic.ic_if;
	const uint8_t* body = payload;
	uint8_t type[2];
	struct mbuf* m;
	size_t total;

	// an LLC/SNAP header carries the Ethernet type
	if (length >= 8 && payload[0] == 0xaa && payload[1] == 0xaa
		&& payload[2] == 0x03 && payload[3] == 0 && payload[4] == 0
		&& (payload[5] == 0 || payload[5] == 0xf8)) {
		type[0] = payload[6];
		type[1] = payload[7];
		body += 8;
		length -= 8;
	} else {
		type[0] = length >> 8;
		type[1] = length & 0xff;
	}

	total = ETHER_HDR_LEN + length;
	if (total > MJUM9BYTES - ETHER_ALIGN)
		return;
	m = MCLGETL(NULL, M_DONTWAIT, total + ETHER_ALIGN);
	if (m == NULL) {
		ifp->if_ierrors++;
		return;
	}
	m->m_data += ETHER_ALIGN;
	memcpy(mtod(m, uint8_t*), destination, ETHER_ADDR_LEN);
	memcpy(mtod(m, uint8_t*) + ETHER_ADDR_LEN, source, ETHER_ADDR_LEN);
	memcpy(mtod(m, uint8_t*) + 2 * ETHER_ADDR_LEN, type, 2);
	memcpy(mtod(m, uint8_t*) + ETHER_HDR_LEN, body, length);
	m->m_pkthdr.len = m->m_len = total;
	m->m_pkthdr.rcvif = ifp;
	ml_enqueue(list, m);
}


/*	The A-MPDU reorder buffer. The firmware negotiates block acks itself and
	hands frames over in the order they arrived; the host puts them back in
	sequence order: a window of 64 per TID, frames held at most 50 ms. Under
	sc_rx_lock. */
static void
aic_reorder_take(struct aic_reorder* r, int slot, struct mbuf_list* out)
{
	ml_enlist(out, &r->slots[slot]);
	r->filled[slot] = 0;
	r->held--;
}


static void
aic_reorder_release(struct aic_reorder* r, struct mbuf_list* out)
{
	while (r->filled[r->head % AIC_REORDER_WINDOW]) {
		aic_reorder_take(r, r->head % AIC_REORDER_WINDOW, out);
		r->head = (r->head + 1) & 0xfff;
	}
	if (r->held == 0)
		r->since = 0;
}


static void
aic_reorder_input(struct aic_softc* sc, int tid, uint16_t sequence,
	struct mbuf_list* msdus, struct mbuf_list* out)
{
	struct aic_reorder* r = &sc->sc_reorder[tid];
	uint16_t delta;
	int slot;

	mtx_enter(&sc->sc_rx_lock);
	if (!r->active) {
		r->active = 1;
		r->head = sequence;
		r->behind = 0;
	}

	delta = (sequence - r->head) & 0xfff;
	if (delta >= 2048) {
		// Behind the window: a retransmission. A run of them means the
		// sender's numbers started over (a new block ack session); the
		// window follows them then.
		if (++r->behind < AIC_REORDER_WINDOW / 2) {
			sc->sc_reorder_behind++;
			mtx_leave(&sc->sc_rx_lock);
			ml_purge(msdus);
			return;
		}
		sc->sc_reorder_restart++;
		while (r->held > 0) {
			while (!r->filled[r->head % AIC_REORDER_WINDOW])
				r->head = (r->head + 1) & 0xfff;
			aic_reorder_release(r, out);
		}
		r->head = sequence;
		delta = 0;
	}
	r->behind = 0;
	if (delta >= AIC_REORDER_WINDOW) {
		// ahead of it: what the window leaves behind goes up as it is
		uint16_t head = (sequence - AIC_REORDER_WINDOW + 1) & 0xfff;
		while (r->head != head) {
			slot = r->head % AIC_REORDER_WINDOW;
			if (r->filled[slot])
				aic_reorder_take(r, slot, out);
			else
				sc->sc_reorder_skipped++;
			r->head = (r->head + 1) & 0xfff;
		}
	}

	slot = sequence % AIC_REORDER_WINDOW;
	if (r->filled[slot]) {
		sc->sc_reorder_dup++;
		mtx_leave(&sc->sc_rx_lock);
		ml_purge(msdus);
		return;
	}
	ml_enlist(&r->slots[slot], msdus);
	r->filled[slot] = 1;
	if (r->held++ == 0)
		r->since = system_time();
	aic_reorder_release(r, out);
	if (r->held > 0)
		sc->sc_reorder_held = 1;
	mtx_leave(&sc->sc_rx_lock);
}


/*	Releases what waited too long (or everything, \a all), skipping the
	frames that never came. */
static void
aic_reorder_flush(struct aic_softc* sc, int all, struct mbuf_list* out)
{
	bigtime_t now = system_time();
	int tid, held = 0;

	mtx_enter(&sc->sc_rx_lock);
	for (tid = 0; tid < 8; tid++) {
		struct aic_reorder* r = &sc->sc_reorder[tid];
		if (r->held > 0 && (all || now - r->since >= AIC_REORDER_TIMEOUT)) {
			if (!all)
				sc->sc_reorder_timeouts++;
			while (r->held > 0) {
				while (!r->filled[r->head % AIC_REORDER_WINDOW]) {
					r->head = (r->head + 1) & 0xfff;
					sc->sc_reorder_skipped++;
				}
				aic_reorder_release(r, out);
			}
		}
		if (all)
			r->active = 0;
		held |= r->held > 0;
	}
	sc->sc_reorder_held = held;
	mtx_leave(&sc->sc_rx_lock);
}


/*	A received 802.11 data frame, already decrypted by the firmware (the
	IV is still there). On a receive thread, without Giant. */
static void
aic_cb_data(void* cookie, int pipe, const uint8_t* packet, size_t length)
{
	struct aic_softc* sc = cookie;
	uint32_t flags = get32(packet + AIC_RX_FLAGS);
	uint32_t status = get32(packet + AIC_RX_STATUS);
	const uint8_t* frame = packet + AIC_RX_HEADER;
	size_t frameLength = length - AIC_RX_HEADER;
	const uint8_t *destination, *source;
	struct mbuf_list msdus = MBUF_LIST_INITIALIZER();
	size_t header = 24;
	int qos, direction, decrypt;

	if ((flags & AIC_RX_UPLOAD) == 0 || frameLength < 24)
		return;
	if ((flags & AIC_RX_IS_80211_MPDU) != 0) {
		// management: the firmware reports what matters as messages
		return;
	}
	if ((frame[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA
		|| (frame[0] & IEEE80211_FC0_SUBTYPE_NODATA) != 0)
		return;

	direction = frame[1] & IEEE80211_FC1_DIR_MASK;
	qos = (frame[0] & IEEE80211_FC0_SUBTYPE_QOS) != 0;
	if (direction == IEEE80211_FC1_DIR_DSTODS)
		header += IEEE80211_ADDR_LEN;
	switch (direction) {
		case IEEE80211_FC1_DIR_NODS:
			destination = frame + 4;
			source = frame + 10;
			break;
		case IEEE80211_FC1_DIR_FROMDS:
			destination = frame + 4;
			source = frame + 16;
			break;
		case IEEE80211_FC1_DIR_TODS:
			destination = frame + 16;
			source = frame + 10;
			break;
		default:
			destination = frame + 16;
			source = frame + 24;
			break;
	}

	{
		size_t qosOffset = header;
		int amsdu = 0;

		if (qos) {
			if (frameLength < header + 2)
				return;
			amsdu = (frame[qosOffset] & 0x80) != 0;
			header += 2;
			if ((frame[1] & IEEE80211_FC1_ORDER) != 0)
				header += 4;
		}

		if ((frame[1] & IEEE80211_FC1_PROTECTED) != 0) {
			decrypt = AIC_RX_DECRYPT(status);
			if (decrypt == AIC_DECRYPT_NONE)
				return;
			header += decrypt == AIC_DECRYPT_WEP ? 4
				: decrypt == AIC_DECRYPT_WAPI ? 18 : 8;
		}
		if (frameLength < header)
			return;

		if (!amsdu) {
			aic_rx_msdu(sc, &msdus, destination, source, frame + header,
				frameLength - header);
		} else {
			// an A-MSDU: subframes of {destination, source, length, MSDU},
			// each but the last padded to 4 bytes
			const uint8_t* p = frame + header;
			size_t left = frameLength - header;
			while (left >= ETHER_HDR_LEN) {
				size_t msdu = (p[12] << 8) | p[13];
				size_t step;
				if (msdu > left - ETHER_HDR_LEN)
					break;
				aic_rx_msdu(sc, &msdus, p, p + ETHER_ADDR_LEN,
					p + ETHER_HDR_LEN, msdu);
				step = (ETHER_HDR_LEN + msdu + 3) & ~(size_t)3;
				if (step >= left)
					break;
				p += step;
				left -= step;
			}
		}
		if (ml_empty(&msdus))
			return;

		// EAPOL frames are not held back (nor are they aggregated)
		if (qos && (flags & AIC_RX_NEED_REORDER) != 0
			&& !ETHERTYPE_EAPOL_BYTES(mtod(msdus.ml_head, uint8_t*)
				+ 2 * ETHER_ADDR_LEN)) {
			aic_reorder_input(sc, frame[qosOffset] & 0x7,
				(get16(frame + 22) >> 4) & 0xfff, &msdus, &sc->sc_rxq[pipe]);
		} else
			ml_enlist(&sc->sc_rxq[pipe], &msdus);
	}
}


/*	Received frames to the stack, or to net80211's handshake. Under Giant.
*/
static void
aic_rx_deliver(struct aic_softc* sc, struct mbuf_list* list)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf* m;

	while ((m = ml_dequeue(list)) != NULL) {
		const uint8_t* type = mtod(m, uint8_t*) + 2 * ETHER_ADDR_LEN;
		int eapol = ETHERTYPE_EAPOL_BYTES(type);

		if ((ifp->if_flags & IFF_RUNNING) == 0) {
			m_freem(m);
			continue;
		}
		if (ic->ic_state != IEEE80211_S_RUN) {
			if (eapol && (ic->ic_state == IEEE80211_S_AUTH
					|| ic->ic_state == IEEE80211_S_ASSOC)) {
				// before the firmware's news of the association
				if (sc->sc_early_eapol != NULL)
					m_freem(sc->sc_early_eapol);
				sc->sc_early_eapol = m;
			} else
				m_freem(m);
			continue;
		}

		if (eapol && (ic->ic_flags & IEEE80211_F_RSNON) != 0) {
			ifp->if_ipackets++;
			ieee80211_eapol_key_input(ic, m, ic->ic_bss);
			continue;
		}
		if ((ic->ic_flags & IEEE80211_F_RSNON) != 0
			&& !ic->ic_bss->ni_port_valid) {
			m_freem(m);
			continue;
		}
		ml_enqueue(&ml, m);
	}
	if (!ml_empty(&ml))
		if_input(ifp, &ml);
}


static void
aic_cb_data_done(void* cookie, int pipe)
{
	struct aic_softc* sc = cookie;

	if (!ml_empty(&sc->sc_rxq[pipe])) {
		mtx_lock(&Giant);
		aic_rx_deliver(sc, &sc->sc_rxq[pipe]);
		mtx_unlock(&Giant);
	}

	// frames held for reordering are released by the driver's thread
	if (sc->sc_reorder_held)
		aic_wake(sc);
}


static void
aic_cb_tx_confirm(void* cookie, uint32 status, uint32 index)
{
	struct aic_softc* sc = cookie;

	sc->sc_eapol_confirmed = index & 0xffff;
	release_sem_etc(sc->sc_eapol_wait, 1, B_DO_NOT_RESCHEDULE);
}


static void
aic_cb_tx_ready(void* cookie)
{
	struct aic_softc* sc = cookie;

	sc->sc_tx_restart = 1;
	aic_wake(sc);
}


static void
aic_cb_gone(void* cookie)
{
	struct aic_softc* sc = cookie;

	sc->sc_gone = 1;
	aic_wake(sc);
}


static void
aic_cb_back(void* cookie)
{
	struct aic_softc* sc = cookie;

	sc->sc_back = 1;
	aic_wake(sc);
}


/*	The chip has left the bus: whatever the firmware knew is gone. */
static void
aic_chip_gone(struct aic_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;

	printf("%s: the chip has left the bus\n", DEVNAME(sc));
	sc->sc_gone = 0;
	aic_forget_association(sc);
	sc->sc_scanning = 0;
	sc->sc_vif = -1;
	sc->sc_started = 0;
	if ((ifp->if_flags & IFF_RUNNING) != 0
		&& ic->ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
}


/*	The chip is back with fresh firmware: set it up as at attachment, and
	bring the interface back up if it was. */
static void
aic_chip_back(struct aic_softc* sc)
{
	struct ifnet* ifp = &sc->sc_ic.ic_if;
	int giant = mtx_owned(&Giant);

	sc->sc_back = 0;
	printf("%s: the chip is back\n", DEVNAME(sc));

	if (giant)
		mtx_unlock(&Giant);
	aic_usb_stop();
	if (aic_usb_start(&aic_callbacks, sc) != B_OK) {
		if (giant)
			mtx_lock(&Giant);
		printf("%s: cannot restart the transport\n", DEVNAME(sc));
		return;
	}
	if (giant)
		mtx_lock(&Giant);

	sc->sc_timeouts = 0;
	sc->sc_recovering = 0;
	sc->sc_vif = -1;
	sc->sc_started = 0;
	if (aic_preinit(sc) != 0) {
		printf("%s: setting the chip up again failed\n", DEVNAME(sc));
		return;
	}
	if ((ifp->if_flags & IFF_RUNNING) != 0) {
		ifp->if_flags &= ~IFF_RUNNING;
		aic_init(ifp);
	}
}


/*	A message no command waits for: for the driver's thread. */
static void
aic_cb_message(void* cookie, uint16 id, const uint8* parameters,
	size_t length)
{
	struct aic_softc* sc = cookie;
	struct aic_event* event;

	switch (id) {
		case AIC_SCANU_RESULT_IND:
		case AIC_SCANU_START_CFM:
		case AIC_SM_CONNECT_IND:
		case AIC_SM_DISCONNECT_IND:
		case AIC_ME_TKIP_MIC_FAILURE_IND:
		case AIC_MM_FW_PANIC_IND:
		case AIC_MM_FW_ASSERT_IND:
		case AIC_DBG_ERROR_IND:
			break;
		default:
			// the channel survey (0x4f) every 35 ms during scans, the
			// credit updates: nothing a station needs
			return;
	}

	if (length > AIC_EVENT_MAX)
		length = AIC_EVENT_MAX;
	event = malloc(sizeof(struct aic_event) + length, M_DEVBUF, M_NOWAIT);
	if (event == NULL)
		return;
	event->next = NULL;
	event->id = id;
	event->length = length;
	memcpy(event->data, parameters, length);

	mtx_enter(&sc->sc_lock);
	if (sc->sc_event_count >= AIC_EVENTS_MAX && id == AIC_SCANU_RESULT_IND) {
		mtx_leave(&sc->sc_lock);
		free(event, M_DEVBUF, 0);
		return;
	}
	*sc->sc_events_tail = event;
	sc->sc_events_tail = &event->next;
	sc->sc_event_count++;
	mtx_leave(&sc->sc_lock);

	aic_wake(sc);
}


//	#pragma mark - the driver's thread


static void
aic_handle_event(struct aic_softc* sc, struct aic_event* event)
{
	switch (event->id) {
		case AIC_SCANU_RESULT_IND:
			aic_scan_result(sc, event->data, event->length);
			break;
		case AIC_SCANU_START_CFM:
			aic_scan_done(sc);
			break;
		case AIC_SM_CONNECT_IND:
			aic_connected(sc, event->data, event->length);
			break;
		case AIC_SM_DISCONNECT_IND:
			aic_disconnected(sc, event->data, event->length);
			break;
		case AIC_ME_TKIP_MIC_FAILURE_IND:
			printf("%s: TKIP MIC failure\n", DEVNAME(sc));
			break;
		case AIC_MM_FW_PANIC_IND:
		case AIC_MM_FW_ASSERT_IND:
		case AIC_DBG_ERROR_IND:
			printf("%s: the firmware reports an error (%#06x)\n", DEVNAME(sc),
				event->id);
			break;
	}
}


static void
aic_do_work(struct aic_softc* sc, struct aic_work* work)
{
	switch (work->type) {
		case AIC_WORK_SCAN:
			if (work->length > 0)
				aic_do_scan(sc, work->data + 1, work->data[0]);
			else
				aic_do_scan(sc, NULL, 0);
			break;
		case AIC_WORK_SCAN_CANCEL:
			if (sc->sc_scanning) {
				aic_cmd(sc, AIC_SCANU_CANCEL_REQ, AIC_TASK_SCANU, NULL, 0,
					AIC_SCANU_CANCEL_CFM, NULL, 0);
				sc->sc_scanning = 0;
			}
			break;
		case AIC_WORK_CONNECT:
			aic_do_connect(sc, work->data, work->length);
			break;
		case AIC_WORK_DISCONNECT:
			sc->sc_connecting = 0;
			aic_do_disconnect(sc);
			break;
		case AIC_WORK_SET_KEY:
			aic_do_set_key(sc, (struct aic_key*)work->data);
			break;
		case AIC_WORK_DELETE_KEY:
			aic_do_delete_key(sc, (struct aic_key*)work->data);
			break;
	}
}


/*	The rate the firmware sends at and the signal, for the user interface
	(IEEE80211_IOC_HAIKU_TX_RATE reads them from the node). The rate word
	holds the MCS or legacy rate index (bits 0-6), the width (7-8), the
	guard interval (9-10) and the format (11-13: legacy, legacy duplicate,
	HT, HT greenfield, VHT, HE). */
static void
aic_update_link(struct aic_softc* sc)
{
	/* 100 kb/s: 1, 2, 5.5, 11 (CCK), 6 ... 54 (OFDM) */
	static const uint16_t legacy[12] = {
		10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540
	};
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_node* ni = ic->ic_bss;
	struct ieee80211_node* tree;
	uint8_t request[4];
	uint8_t confirm[32];
	uint32_t rate;
	int format, mcs, width, gi, nss = 1, mode;

	request[0] = sc->sc_ap;
	request[1] = 's';
	request[2] = 't';
	request[3] = 'a';
	memset(confirm, 0, sizeof(confirm));
	if (aic_cmd(sc, AIC_MM_GET_STA_INFO_REQ, AIC_TASK_MM, request,
			sizeof(request), AIC_MM_GET_STA_INFO_CFM, confirm,
			sizeof(confirm)) != 0 || ic->ic_state != IEEE80211_S_RUN)
		return;
	ni = ic->ic_bss;

	if ((int8_t)confirm[AIC_STA_INFO_RSSI] < 0) {
		ni->ni_rssi = confirm[AIC_STA_INFO_RSSI];
		tree = ieee80211_find_node(ic, ni->ni_macaddr);
		if (tree != NULL)
			tree->ni_rssi = ni->ni_rssi;
	}

	rate = get32(confirm + AIC_STA_INFO_RATE);
	mcs = rate & 0x7f;
	width = 20 << ((rate >> 7) & 0x3);
	gi = (rate >> 9) & 0x3;
	format = (rate >> 11) & 0x7;
	switch (format) {
		case 0:
		case 1:
			if (mcs >= (int)nitems(legacy))
				return;
			ni->ni_haiku_tx_mode = IEEE80211_HAIKU_TX_MODE_LEGACY;
			ni->ni_haiku_tx_mcs = 0;
			ni->ni_haiku_tx_nss = 1;
			ni->ni_haiku_tx_gi = 0;
			ni->ni_haiku_tx_width = 20;
			ni->ni_haiku_tx_kbps = legacy[mcs] * 100;
			return;
		case 2:
		case 3:
			mode = IEEE80211_HAIKU_TX_MODE_HT;
			nss = mcs / 8 + 1;
			mcs %= 8;
			gi = gi != 0 ? 4 : 8;
			// tell net80211 what the firmware negotiated
			ni->ni_flags |= IEEE80211_NODE_HT;
			if (ic->ic_curmode < IEEE80211_MODE_11N)
				ieee80211_setmode(ic, IEEE80211_MODE_11N);
			break;
		case 4:
			mode = IEEE80211_HAIKU_TX_MODE_VHT;
			nss = (mcs >> 4) + 1;
			mcs &= 0xf;
			gi = gi != 0 ? 4 : 8;
			ni->ni_flags |= IEEE80211_NODE_HT | IEEE80211_NODE_VHT;
			if (ic->ic_curmode < IEEE80211_MODE_11AC)
				ieee80211_setmode(ic, IEEE80211_MODE_11AC);
			break;
		default:
			mode = IEEE80211_HAIKU_TX_MODE_HE;
			nss = (mcs >> 4) + 1;
			mcs &= 0xf;
			gi = gi == 0 ? 8 : gi == 1 ? 16 : 32;
			break;
	}
	ni->ni_haiku_tx_mode = mode;
	ni->ni_haiku_tx_mcs = mcs;
	ni->ni_haiku_tx_nss = nss;
	ni->ni_haiku_tx_gi = gi;
	ni->ni_haiku_tx_width = width;
	ni->ni_haiku_tx_kbps = ieee80211_haiku_mcs_kbps(mode, mcs, nss, width,
		gi);
	if (rate != sc->sc_link_rate) {
		sc->sc_link_rate = rate;
		DPRINTF("%s: sending at MCS %d, %d stream(s), %d MHz, %u kb/s; "
			"signal %d dBm\n", DEVNAME(sc), mcs, nss, width,
			ni->ni_haiku_tx_kbps, (int8_t)confirm[AIC_STA_INFO_RSSI]);
	}
}


/*	Once a second: a scan that never ended, a scan net80211 waits for that
	was not accepted; every other second, the link's rate and signal. */
static void
aic_tick(struct aic_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;

	if ((ifp->if_flags & IFF_RUNNING) == 0 || sc->sc_vif < 0)
		return;

	if (sc->sc_scanning
		&& system_time() - sc->sc_scan_started > AIC_SCAN_TIMEOUT) {
		printf("%s: the scan did not end\n", DEVNAME(sc));
		aic_scan_done(sc);
	} else if (!sc->sc_scanning && ic->ic_state == IEEE80211_S_SCAN
		&& system_time() - sc->sc_scan_started > 5000000) {
		uint8_t ssid[1 + IEEE80211_NWID_LEN];
		ssid[0] = ic->ic_des_esslen;
		memcpy(ssid + 1, ic->ic_des_essid, IEEE80211_NWID_LEN);
		aic_do_scan(sc, ssid + 1, ssid[0]);
	}

	if (ic->ic_state == IEEE80211_S_RUN && sc->sc_connected
		&& system_time() - sc->sc_link_time >= 2000000) {
		sc->sc_link_time = system_time();
		aic_update_link(sc);
	}

	if (aic_watch && system_time() - sc->sc_stats_time >= 5000000) {
		int held = 0, tid;
		sc->sc_stats_time = system_time();
		for (tid = 0; tid < 8; tid++)
			held += sc->sc_reorder[tid].held;
		printf("%s: watch: state %d, reorder held %d (behind %u, duplicate "
			"%u, restarts %u, never came %u, timeouts %u), events %d, work "
			"%d, send queue %d, receive queue %d (%llu dropped)\n",
			DEVNAME(sc), ic->ic_state, held, sc->sc_reorder_behind,
			sc->sc_reorder_dup, sc->sc_reorder_restart,
			sc->sc_reorder_skipped, sc->sc_reorder_timeouts,
			sc->sc_event_count, sc->sc_work_count, ifp->if_snd.ifq_len,
			ifp->receive_queue.ifq_len, (unsigned long long)ifp->if_iqdrops);
	}
}


static status_t
aic_thread(void* arg)
{
	struct aic_softc* sc = arg;
	struct ifnet* ifp = &sc->sc_ic.ic_if;
	bigtime_t nextTick = system_time() + 1000000;

	while (sc->sc_run) {
		struct aic_event* event;
		struct aic_work work;
		int haveWork;

		acquire_sem_etc(sc->sc_wake, 1, B_ABSOLUTE_TIMEOUT,
			sc->sc_reorder_held
				? min_c(nextTick, system_time() + AIC_REORDER_TIMEOUT / 2)
				: nextTick);
		if (!sc->sc_run)
			break;

		mtx_lock(&Giant);
		if (sc->sc_reorder_held) {
			struct mbuf_list released = MBUF_LIST_INITIALIZER();
			aic_reorder_flush(sc, 0, &released);
			aic_rx_deliver(sc, &released);
		}
		for (;;) {
			mtx_enter(&sc->sc_lock);
			event = sc->sc_events;
			if (event != NULL) {
				sc->sc_events = event->next;
				if (sc->sc_events == NULL)
					sc->sc_events_tail = &sc->sc_events;
				sc->sc_event_count--;
			}
			haveWork = event == NULL && sc->sc_work_count > 0;
			if (haveWork) {
				work = sc->sc_work[sc->sc_work_tail];
				sc->sc_work_tail = (sc->sc_work_tail + 1) % AIC_WORK_COUNT;
				sc->sc_work_count--;
			}
			mtx_leave(&sc->sc_lock);

			if (event != NULL) {
				aic_handle_event(sc, event);
				free(event, M_DEVBUF, 0);
			} else if (haveWork)
				aic_do_work(sc, &work);
			else
				break;
		}

		if (sc->sc_tx_restart) {
			sc->sc_tx_restart = 0;
			ifq_clr_oactive(&ifp->if_snd);
			aic_start(ifp);
		}

		if (sc->sc_gone)
			aic_chip_gone(sc);
		if (sc->sc_back && !aic_usb_gone())
			aic_chip_back(sc);

		if (system_time() >= nextTick) {
			nextTick = system_time() + 1000000;
			aic_tick(sc);
		}
		mtx_unlock(&Giant);
	}
	return B_OK;
}


//	#pragma mark - attachment


static int
aic_probe(device_t dev)
{
	// the glue only reports a device when the firmware runs
	device_set_desc(dev, "AICSemi AIC8800D80");
	return 0;
}


static int
aic_attach(device_t dev)
{
	struct aic_softc* sc = device_get_softc(dev);
	struct ieee80211com* ic = &sc->sc_ic;
	struct ifnet* ifp = &ic->ic_if;
	void* settings;
	int i;

	sc->sc_dev = dev;
	sc->sc_vif = -1;
	sc->sc_ap = -1;
	memset(sc->sc_hw_key, 0xff, sizeof(sc->sc_hw_key));
	mtx_init(&sc->sc_lock, IPL_NONE);
	mtx_init(&sc->sc_rx_lock, IPL_NONE);
	for (i = 0; i < 8; i++) {
		int slot;
		for (slot = 0; slot < AIC_REORDER_WINDOW; slot++)
			ml_init(&sc->sc_reorder[i].slots[slot]);
	}
	sc->sc_events = NULL;
	sc->sc_events_tail = &sc->sc_events;
	ml_init(&sc->sc_rxq[0]);
	ml_init(&sc->sc_rxq[1]);

	sc->sc_vht = 1;
	settings = load_driver_settings("aic8800wifi");
	if (settings != NULL) {
		aic_debug = get_driver_boolean_parameter(settings, "debug", 0, 1);
		aic_watch = get_driver_boolean_parameter(settings, "watch", 0, 1);
		sc->sc_vht = get_driver_boolean_parameter(settings, "vht", 1, 1);
		unload_driver_settings(settings);
	}

	sc->sc_wake = create_sem(0, "aic8800wifi wake");
	sc->sc_eapol_wait = create_sem(0, "aic8800wifi eapol");
	sc->sc_tx_buffer = malloc(AIC_USB_TX_MAX, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc->sc_wake < 0 || sc->sc_eapol_wait < 0 || sc->sc_tx_buffer == NULL)
		goto fail;

	if (aic_usb_start(&aic_callbacks, sc) != B_OK) {
		printf("%s: cannot start the transport\n", DEVNAME(sc));
		goto fail;
	}
	if (aic_preinit(sc) != 0) {
		printf("%s: the firmware set up failed\n", DEVNAME(sc));
		aic_usb_stop();
		goto fail;
	}

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_WEP | IEEE80211_C_RSN | IEEE80211_C_SCANALL
		| IEEE80211_C_SCANALLBAND | IEEE80211_C_SHSLOT
		| IEEE80211_C_SHPREAMBLE;

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 0; i < (int)nitems(aic_channels_2ghz); i++) {
		uint8_t channel = aic_channels_2ghz[i];
		ic->ic_channels[channel].ic_freq = ieee80211_ieee2mhz(channel,
			IEEE80211_CHAN_2GHZ);
		ic->ic_channels[channel].ic_flags = IEEE80211_CHAN_CCK
			| IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ
			| IEEE80211_CHAN_HT;
	}
	if (sc->sc_5ghz) {
		ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
		for (i = 0; i < (int)nitems(aic_channels_5ghz); i++) {
			uint8_t channel = aic_channels_5ghz[i];
			ic->ic_channels[channel].ic_freq = ieee80211_ieee2mhz(channel,
				IEEE80211_CHAN_5GHZ);
			ic->ic_channels[channel].ic_flags = IEEE80211_CHAN_A
				| IEEE80211_CHAN_HT;
		}
	}
	ic->ic_ibss_chan = &ic->ic_channels[1];

	if_alloc_inplace(ifp, IFT_ETHER);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = aic_ioctl;
	ifp->if_start = aic_start;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp);
	IEEE80211_ADDR_COPY(IF_LLADDR(ifp), ic->ic_myaddr);

	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = aic_newstate;
	ic->ic_send_mgmt = aic_send_mgmt;
	ic->ic_set_key = aic_set_key;
	ic->ic_delete_key = aic_delete_key;
	ic->ic_bgscan_start = aic_bgscan;

	ieee80211_media_init(ifp, aic_media_change, ieee80211_media_status);

	sc->sc_run = 1;
	sc->sc_thread = spawn_kernel_thread(aic_thread, "aic8800wifi work",
		B_NORMAL_PRIORITY, sc);
	if (sc->sc_thread < 0) {
		sc->sc_run = 0;
		aic_usb_stop();
		goto fail;
	}
	resume_thread(sc->sc_thread);
	aic_usb_watch_thread(sc->sc_thread);
	aic_usb_watch_lock("Giant", &Giant.u.mutex_.lock, &Giant.u.mutex_.owner);
	aic_usb_watch_lock("sc", &sc->sc_lock.mtx.u.mutex_.lock,
		&sc->sc_lock.mtx.u.mutex_.owner);
	aic_usb_watch_lock("sc rx", &sc->sc_rx_lock.mtx.u.mutex_.lock,
		&sc->sc_rx_lock.mtx.u.mutex_.owner);

	sc->sc_attached = 1;
	printf("%s: address %s\n", DEVNAME(sc), ether_sprintf(ic->ic_myaddr));
	return 0;

fail:
	if (sc->sc_wake >= 0)
		delete_sem(sc->sc_wake);
	if (sc->sc_eapol_wait >= 0)
		delete_sem(sc->sc_eapol_wait);
	free(sc->sc_tx_buffer, M_DEVBUF, 0);
	sc->sc_tx_buffer = NULL;
	mtx_destroy(&sc->sc_lock.mtx);
	mtx_destroy(&sc->sc_rx_lock.mtx);
	return ENXIO;
}


/*	Haiku unloads the driver when its file is replaced or the system shuts
	down: nothing of the driver may run after this. */
static int
aic_detach(device_t dev)
{
	struct aic_softc* sc = device_get_softc(dev);
	struct ifnet* ifp = &sc->sc_ic.ic_if;
	struct aic_event* event;
	status_t result;

	if (!sc->sc_attached)
		return 0;

	mtx_lock(&Giant);
	if ((ifp->if_flags & IFF_RUNNING) != 0)
		aic_stop(ifp);
	timeout_del(&sc->sc_ic.ic_bgscan_timeout);
	mtx_unlock(&Giant);

	sc->sc_run = 0;
	release_sem(sc->sc_wake);
	wait_for_thread(sc->sc_thread, &result);

	aic_usb_stop();

	// The next driver load starts from a freshly loaded firmware, as after
	// a recovery: one that was set up and stopped before did not come back
	// reliably (both receive pipes failed after a reload). Bluetooth
	// restarts with the chip.
	aic_usb_reboot_chip();

	while ((event = sc->sc_events) != NULL) {
		sc->sc_events = event->next;
		free(event, M_DEVBUF, 0);
	}
	ml_purge(&sc->sc_rxq[0]);
	ml_purge(&sc->sc_rxq[1]);
	if (sc->sc_early_eapol != NULL)
		m_freem(sc->sc_early_eapol);

	delete_sem(sc->sc_wake);
	delete_sem(sc->sc_eapol_wait);
	free(sc->sc_tx_buffer, M_DEVBUF, 0);
	mtx_destroy(&sc->sc_lock.mtx);
	mtx_destroy(&sc->sc_rx_lock.mtx);
	sc->sc_attached = 0;
	return 0;
}


static device_method_t aic_methods[] = {
	DEVMETHOD(device_probe,		aic_probe),
	DEVMETHOD(device_attach,	aic_attach),
	DEVMETHOD(device_detach,	aic_detach),

	DEVMETHOD_END
};

static driver_t aic_driver = {
	"aic",
	aic_methods,
	sizeof(struct aic_softc)
};

DRIVER_MODULE(aic, usb, aic_driver, NULL, NULL);
