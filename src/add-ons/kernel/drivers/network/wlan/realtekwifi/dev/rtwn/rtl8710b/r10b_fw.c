/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710B firmware download hooks for rtwn_load_firmware().
 *
 * rtwn_load_firmware() drives R92C_MCUFWDL; on this chip that register is
 * R10B_8051FW_CTRL (0x90), with the same low bits and page field, and
 * usb/r10bu_reg.c redirects the generic accesses there. What is left is
 * the MCU reset and the download enable, which differ: no CPU enable bit
 * in SYS_FUNC, bit 8 set before a download, the MCU released by bit 24.
 * As Linux's rtl8xxxu does for this chip (core.c
 * rtl8xxxu_download_firmware(), rtl8xxxu_start_firmware(); 8710b.c
 * rtl8710b_reset_8051()).
 */

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/linker.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>

#ifndef RTWN_WITHOUT_UCODE
void
r10b_fw_reset(struct rtwn_softc *sc, int reason)
{
	/*
	 * Every reason (checksum done, stale firmware before a download,
	 * shutdown) is the same MCU reset pulse.
	 */
	rtwn_setbits_1(sc, R10B_8051FW_CTRL + 3, R10B_8051FW_CTRL_MCU_EN >> 24,
	    0);
	rtwn_delay(sc, 50);
	rtwn_setbits_1(sc, R10B_8051FW_CTRL + 3, 0,
	    R10B_8051FW_CTRL_MCU_EN >> 24);
}

void
r10b_fw_download_enable(struct rtwn_softc *sc, int enable)
{
	if (enable) {
		rtwn_setbits_1(sc, R10B_8051FW_CTRL, 0, R92C_MCUFWDL_EN);
		rtwn_setbits_4(sc, R10B_8051FW_CTRL,
		    R10B_8051FW_CTRL_ROM_DLEN, 0);
		/* Must be set before the download. */
		rtwn_setbits_1(sc, R10B_8051FW_CTRL + 1, 0,
		    R10B_8051FW_CTRL_DL_PREP >> 8);
	} else
		rtwn_setbits_2(sc, R10B_8051FW_CTRL, R92C_MCUFWDL_EN, 0);
}
#endif
