/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RTWN_HOST_H
#define RTWN_HOST_H

/*
 * The least of the FreeBSD kernel and net80211 that rtwn's headers and the
 * files under test (sdio/rtwn_sdio_reg.c, rtl8723b/sdio/r23bs_init.c,
 * rtl8723b/r23b_pwr.c) need to compile on the host. Force-included; the
 * kernel headers they name are empty files (run.sh makes them).
 */

#include <endian.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

typedef struct device *device_t;
typedef char *caddr_t;

#define __packed		__attribute__((packed))
#define __aligned(x)		__attribute__((aligned(x)))
#define howmany(x, y)		(((x) + ((y) - 1)) / (y))
#define nitems(x)		(sizeof(x) / sizeof((x)[0]))
#ifndef MAX
#define MAX(a, b)		((a) > (b) ? (a) : (b))
#endif
#define NBBY			8
#define hz			1000
#define MALLOC_DECLARE(x)	extern int x

struct mtx { int dummy; };
struct mbufq { int dummy; };
struct callout { int dummy; };
struct task { int dummy; };
struct timeout_task { int dummy; };
struct taskqueue;
struct mbuf;
struct ieee80211_channel {
	uint8_t		ic_ieee;
	uint8_t		ic_extieee;
	uint16_t	ic_vht_ch_freq1;
};
#define IEEE80211_IS_CHAN_VHT(c)	0
struct ieee80211_scan_state;
struct ieee80211_bpf_params;
struct ieee80211_rx_stats;
struct ieee80211_radiotap_header { uint8_t pad[8]; };
struct ieee80211_key { int dummy; };
struct ieee80211vap { int dummy; };
struct ieee80211_node { int dummy; };
struct wmeParams { int dummy; };
enum ieee80211_state { IEEE80211_S_INIT };
#define IEEE80211_WEP_NKID	4
#define IEEE80211_ADDR_LEN	6
#define WME_NUM_AC		4

struct ieee80211com {
	const char	*ic_name;
	uint8_t		ic_macaddr[IEEE80211_ADDR_LEN];
};

#define mtx_assert(m, w)	do { } while (0)
#define MA_OWNED		0

/* The test's clock and log (sdio_backend_test.c). */
void	test_delay(int usec);
void	test_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void	*device_get_ivars(device_t dev);

#define DELAY(us)		test_delay(us)
#define pause(msg, ticks)	test_delay((ticks) * 1000)
#define device_printf(dev, ...)	test_log(__VA_ARGS__)

#endif	/* RTWN_HOST_H */
