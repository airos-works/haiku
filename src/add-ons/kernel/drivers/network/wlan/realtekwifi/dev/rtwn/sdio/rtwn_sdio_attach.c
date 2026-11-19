/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * rtwn on SDIO: the attachment of Realtek's 802.11n SDIO chips (so far the
 * RTL8723BS) to the generic rtwn code, the way usb/ and pci/ attach theirs.
 *
 * The card is set up by the MMC bus (Haiku's bus_managers/mmc); the
 * compatibility layer hands us its function 1 (dev/sdio/sdio_subr.h). The
 * driver enables the function, sets 512-byte blocks, powers the chip on to
 * read its efuse (the chip sits in "card disabled" until then; the vendor
 * driver does the same), powers it off again and installs the card
 * interrupt handler. rtwn_init() powers it on for real when the interface
 * comes up.
 *
 * Every step is logged with the prefix "rtwnN: sdio:", the steps where
 * things can go wrong as well as the results, so a syslog from a machine
 * we do not have shows how far it got.
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/sdio_subr.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_nop.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>
#include <dev/rtwn/sdio/rtwn_sdio_attach.h>
#include <dev/rtwn/sdio/rtwn_sdio_frame.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

static device_probe_t	rtwn_sdio_probe;
static device_attach_t	rtwn_sdio_attach;
static device_detach_t	rtwn_sdio_detach;
static device_suspend_t	rtwn_sdio_suspend;
static device_resume_t	rtwn_sdio_resume;

static const struct rtwn_sdio_ident *
rtwn_sdio_lookup(device_t dev)
{
	struct sdio_func *func = sdio_get_function(dev);
	size_t i;

	if (func == NULL || func->fn != 1)
		return (NULL);
	for (i = 0; i < nitems(rtwn_sdio_ident_table); i++) {
		if (rtwn_sdio_ident_table[i].vendor == func->vendor &&
		    rtwn_sdio_ident_table[i].device == func->device)
			return (&rtwn_sdio_ident_table[i]);
	}
	return (NULL);
}

static int
rtwn_sdio_probe(device_t dev)
{
	if (rtwn_sdio_lookup(dev) == NULL)
		return (ENXIO);

	device_set_desc(dev, "Realtek RTL8723BS SDIO 802.11n");
	return (BUS_PROBE_DEFAULT);
}

static int
rtwn_sdio_ms_ticks(int ms)
{
	return ((int)MAX(1, ms * hz / 1000));
}

static void
rtwn_sdio_log_status(struct rtwn_softc *sc, uint32_t hisr, uint16_t rxlen)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);

	RTWN_SDIO_LOG(sc, "status after %d s: %u card interrupts, %u polls "
	    "with work%s, HISR %#x HIMR %#x RX0_REQ_LEN %u; RX %u reads %u "
	    "packets; TX %u frames %u dropped, pages free %u/%u/%u/%u\n",
	    (int)((ticks - sd->sd_status_start) / hz), sd->sd_intrs,
	    sd->sd_polls, sd->sd_polling ? " (polling)" : "", hisr,
	    sd->sd_himr, rxlen, sd->sd_rx_reads, sd->sd_rx_pkts,
	    sd->sd_tx_frames, sd->sd_tx_dropped, sd->sd_free_pages[0],
	    sd->sd_free_pages[1], sd->sd_free_pages[2], sd->sd_free_pages[3]);
}

/*
 * While the interface is up: checks once a second that work pending at the
 * chip brought a card interrupt, and serves the chip itself from then on
 * if it did not (the card interrupt depends on the host controller's
 * handling of DAT1, untested on most of the hosts this may run on); logs
 * the counters for the first seconds. In its own thread, so that the TX
 * task waiting for pages does not hold it up.
 */
static void
rtwn_sdio_status_task(void *arg, int pending)
{
	struct rtwn_softc *sc = arg;
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t hisr;
	uint16_t rxlen;
	int now, poll;

	RTWN_LOCK(sc);
	if (!sd->sd_xfers || sd->sd_gone) {
		RTWN_UNLOCK(sc);
		return;
	}
	now = ticks;
	poll = sd->sd_polling;
	if (now - sd->sd_status_check >= hz * 9 / 10) {
		sd->sd_status_check = now;
		hisr = rtwn_sdio_read_local_4(sc, RTWN_SDIO_HISR);
		rxlen = rtwn_sdio_read_local_2(sc, RTWN_SDIO_RX0_REQ_LEN);
		/*
		 * Work at two checks in a row and not one card interrupt in
		 * between: they do not come. (Once could be work that came
		 * just now.)
		 */
		if (hisr != 0xffffffff && (hisr & sd->sd_himr) != 0 &&
		    sd->sd_intrs == sd->sd_check_intrs)
			sd->sd_stuck_checks++;
		else
			sd->sd_stuck_checks = 0;
		if (!sd->sd_polling && sd->sd_stuck_checks >= 2) {
			sd->sd_polling = 1;
			poll = 1;
			RTWN_SDIO_LOG(sc, "HISR %#x has had work (HIMR %#x) for "
			    "a second with no card interrupt: serving the chip "
			    "every %d ms from now on\n", hisr, sd->sd_himr,
			    RTWN_SDIO_POLL_MS);
		}
		sd->sd_check_intrs = sd->sd_intrs;
		if (now - sd->sd_status_start <= RTWN_SDIO_STATUS_LOG_S * hz &&
		    now - sd->sd_status_next >= 0) {
			sd->sd_status_next = now + RTWN_SDIO_STATUS_EVERY_S * hz;
			rtwn_sdio_log_status(sc, hisr, rxlen);
		}
	}
	RTWN_UNLOCK(sc);

	if (poll)
		(void)rtwn_sdio_poll(sc);

	RTWN_LOCK(sc);
	if (sd->sd_xfers && !sd->sd_gone) {
		taskqueue_enqueue_timeout(sd->sd_status_tq,
		    &sd->sd_status_task, sd->sd_polling ?
		    rtwn_sdio_ms_ticks(RTWN_SDIO_POLL_MS) :
		    rtwn_sdio_ms_ticks(1000));
	}
	RTWN_UNLOCK(sc);
}

static void
rtwn_sdio_start_xfers(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint16_t th;

	RTWN_ASSERT_LOCKED(sc);

	/*
	 * Thresholds for the AVAL interrupt: half of what a queue can have
	 * (its own pages and the public ones), in both bytes, as the vendor
	 * driver sets them (sdio_halinit.c _init_available_page_threshold()).
	 */
	th = (sc->npubqpages + sc->nhqpages + 1) >> 1;
	rtwn_write_2(sc, 0x218, th | (th << 8));
	th = (sc->npubqpages + sc->nnqpages + 1) >> 1;
	rtwn_write_2(sc, 0x21a, th | (th << 8));
	th = (sc->npubqpages + sc->nlqpages + 1) >> 1;
	rtwn_write_2(sc, 0x21c, th | (th << 8));

	/* SDIO TX control: no RX DMA timer masking (the vendor's value). */
	rtwn_sdio_write_local_4(sc, RTWN_SDIO_TX_CTRL, 0);

	rtwn_sdio_update_free_pages(sc);
	sd->sd_oqt_free = rtwn_sdio_read_local_1(sc, RTWN_SDIO_OQT_FREE_PG);
	sd->sd_tx_stall_ms = 0;

	/* Firmware events from before: acknowledge, or it sends no more. */
	rtwn_write_1(sc, R92C_C2H_EVT_CLEAR, 0);

	sd->sd_himr = RTWN_SDIO_INT_RX_REQUEST;
	rtwn_sdio_write_local_4(sc, RTWN_SDIO_HISR, RTWN_SDIO_INT_W1C);
	rtwn_sdio_write_local_4(sc, RTWN_SDIO_HIMR, sd->sd_himr);
	sd->sd_xfers = 1;

	RTWN_SDIO_LOG(sc, "running: firmware %s (version %u), TX pages free "
	    "high %u normal %u low %u public %u, OQT %u, HIMR %#x\n",
	    (sc->sc_flags & RTWN_FW_LOADED) ? "running" : "NOT loaded",
	    sc->fwver, sd->sd_free_pages[0], sd->sd_free_pages[1],
	    sd->sd_free_pages[2], sd->sd_free_pages[3], sd->sd_oqt_free,
	    sd->sd_himr);

	sd->sd_status_start = ticks;
	sd->sd_status_check = sd->sd_status_start;
	sd->sd_status_next = sd->sd_status_start + 2 * hz;
	sd->sd_check_intrs = sd->sd_intrs;
	sd->sd_stuck_checks = 0;
	taskqueue_enqueue_timeout(sd->sd_status_tq, &sd->sd_status_task,
	    rtwn_sdio_ms_ticks(sd->sd_polling ? RTWN_SDIO_POLL_MS : 1000));
}

static void
rtwn_sdio_abort_xfers(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);

	RTWN_ASSERT_LOCKED(sc);

	if (sd->sd_xfers) {
		RTWN_SDIO_LOG(sc, "stopping: %u RX reads, %u RX packets, %u TX "
		    "frames, %u dropped, %u card interrupts, %u polls with "
		    "work\n", sd->sd_rx_reads, sd->sd_rx_pkts, sd->sd_tx_frames,
		    sd->sd_tx_dropped, sd->sd_intrs, sd->sd_polls);
	}
	sd->sd_xfers = 0;
	/* Not waited for: it takes the lock, and ends by itself now. */
	taskqueue_cancel_timeout(sd->sd_status_tq, &sd->sd_status_task, NULL);
	sd->sd_himr = 0;
	rtwn_sdio_write_local_4(sc, RTWN_SDIO_HIMR, 0);
	wakeup(sd);
}

static void
rtwn_sdio_reset_lists(struct rtwn_softc *sc, struct ieee80211vap *vap)
{
	rtwn_sdio_reset_tx_list(sc, vap);
}

static uint16_t
rtwn_sdio_get_qmap(struct rtwn_softc *sc)
{
	/* VO, management, high: high; VI: normal; BE, BK: low */
	return (R92C_TRXDMA_CTRL_QMAP_3EP);
}

static void
rtwn_sdio_attach_methods(struct rtwn_softc *sc)
{
	sc->sc_write_1		= rtwn_sdio_write_1;
	sc->sc_write_2		= rtwn_sdio_write_2;
	sc->sc_write_4		= rtwn_sdio_write_4;
	sc->sc_read_1		= rtwn_sdio_read_1;
	sc->sc_read_2		= rtwn_sdio_read_2;
	sc->sc_read_4		= rtwn_sdio_read_4;
	sc->sc_delay		= rtwn_sdio_delay;
	sc->sc_tx_start		= rtwn_sdio_tx_start;
	sc->sc_start_xfers	= rtwn_sdio_start_xfers;
	sc->sc_reset_lists	= rtwn_sdio_reset_lists;
	sc->sc_abort_xfers	= rtwn_sdio_abort_xfers;
	sc->sc_fw_write_block	= rtwn_sdio_fw_write_block;
	sc->sc_get_qmap		= rtwn_sdio_get_qmap;
	sc->sc_set_desc_addr	= rtwn_nop_softc;
	sc->sc_drop_incorrect_tx = rtwn_nop_softc;
	sc->sc_beacon_update_begin = rtwn_nop_softc_vap;
	sc->sc_beacon_update_end = rtwn_nop_softc_vap;
	sc->sc_beacon_unload	= rtwn_nop_softc_int;

	sc->bcn_check_interval	= 100;
}

/* What the chip says about itself before anything is powered. */
static int
rtwn_sdio_check_chip(struct rtwn_softc *sc)
{
	uint32_t sys_cfg;

	sys_cfg = rtwn_read_4(sc, R92C_SYS_CFG);
	RTWN_SDIO_LOG(sc, "SYS_CFG %#010x: cut %u, %s, %s\n", sys_cfg,
	    MS(sys_cfg, R92C_SYS_CFG_CHIP_VER_RTL),
	    (sys_cfg & R92C_SYS_CFG_VENDOR_UMC) ? "UMC" : "TSMC",
	    (sys_cfg & R92C_SYS_CFG_TRP_VAUX_EN) ? "test chip" : "normal chip");
	if (sys_cfg == 0xffffffff || sys_cfg == 0) {
		RTWN_SDIO_LOG(sc, "the chip does not answer register reads\n");
		return (ENXIO);
	}

	/* No card interrupts until the interface is up. */
	RTWN_SDIO_LOG(sc, "HIMR %#010x HISR %#010x HSUS_CTRL %#04x\n",
	    rtwn_sdio_read_local_4(sc, RTWN_SDIO_HIMR),
	    rtwn_sdio_read_local_4(sc, RTWN_SDIO_HISR),
	    rtwn_sdio_read_local_1(sc, RTWN_SDIO_HSUS_CTRL));
	return (rtwn_sdio_write_local_4(sc, RTWN_SDIO_HIMR, 0));
}

static int
rtwn_sdio_attach(device_t self)
{
	struct rtwn_sdio_softc *sd = device_get_softc(self);
	struct rtwn_softc *sc = &sd->sd_sc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct sdio_func *func = sdio_get_function(self);
	const struct rtwn_sdio_ident *ident = rtwn_sdio_lookup(self);
	uint16_t blksize;
	int error, attached = 0;

	sd->sd_func = func;
	sd->sd_chip = ident->chip;
	sc->sc_dev = self;
	ic->ic_name = device_get_nameunit(self);

	RTWN_SDIO_LOG(sc, "attaching card %04x:%04x function %u: %u-bit bus, "
	    "%u kHz%s, CIS block size %u\n", func->vendor, func->device,
	    func->fn, func->bus_width, func->clock,
	    func->high_speed ? " (high speed)" : "", func->max_blksize);

	rtwn_sysctlattach(sc);
	mtx_init(&sc->sc_mtx, ic->ic_name, MTX_NETWORK_LOCK, MTX_DEF);

	rtwn_sdio_attach_methods(sc);
	rtwn_sdio_attach_private(sd, sd->sd_chip);

	error = sdio_enable_func(func);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "cannot enable function %u, error %d\n",
		    func->fn, error);
		goto fail;
	}

	blksize = RTWN_SDIO_BLOCK_SIZE;
	if (func->max_blksize != 0 && func->max_blksize < blksize)
		blksize = func->max_blksize;
	error = sdio_set_block_size(func, blksize);
	sd->sd_blksize = func->cur_blksize;
	if (error != 0 || sd->sd_blksize == 0) {
		RTWN_SDIO_LOG(sc, "cannot set the block size to %u, error %d\n",
		    blksize, error);
		if (error == 0)
			error = ENXIO;
		goto fail;
	}
	if ((func->capabilities & 0x02) == 0) {
		/* no multi-block CMD53 (CCCR SMB): one block per command */
		RTWN_SDIO_LOG(sc, "the card has no multi-block transfers\n");
	}
	RTWN_SDIO_LOG(sc, "function %u enabled, %u-byte blocks\n", func->fn,
	    sd->sd_blksize);

	sd->sd_rx_buf = malloc(RTWN_SDIO_RXBUFSZ, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (sd->sd_rx_buf == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = rtwn_sdio_alloc_tx_list(sc);
	if (error != 0)
		goto fail;

	TASK_INIT(&sd->sd_tx_task, 0, rtwn_sdio_tx_task, sc);
	sd->sd_tq = taskqueue_create("rtwn_sdio", M_NOWAIT,
	    taskqueue_thread_enqueue, &sd->sd_tq);
	if (sd->sd_tq == NULL) {
		error = ENOMEM;
		goto fail;
	}
	taskqueue_start_threads(&sd->sd_tq, 1, PI_NET, "%s tx",
	    device_get_nameunit(self));

	sd->sd_status_tq = taskqueue_create("rtwn_sdio_status", M_NOWAIT,
	    taskqueue_thread_enqueue, &sd->sd_status_tq);
	if (sd->sd_status_tq == NULL) {
		error = ENOMEM;
		goto fail;
	}
	TIMEOUT_TASK_INIT(sd->sd_status_tq, &sd->sd_status_task, 0,
	    rtwn_sdio_status_task, sc);
	taskqueue_start_threads(&sd->sd_status_tq, 1, PI_NET, "%s status",
	    device_get_nameunit(self));

	/* Read what the chip is and power it on to read its efuse. */
	RTWN_LOCK(sc);
	error = rtwn_sdio_check_chip(sc);
	if (error == 0)
		error = rtwn_power_on(sc);
	RTWN_UNLOCK(sc);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "power on failed, error %d\n", error);
		goto fail;
	}

	error = rtwn_attach(sc);
	attached = 1;
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "rtwn_attach() failed, error %d\n", error);
		goto fail;
	}

	/*
	 * Off until the interface comes up, as the vendor driver leaves it,
	 * with the antenna switched to Bluetooth meanwhile (0x67 = 0, also
	 * the vendor's: sdio_halinit.c _ReadAdapterInfo8723BS()).
	 */
	RTWN_LOCK(sc);
	rtwn_write_1(sc, 0x67, 0);
	rtwn_power_off(sc);
	RTWN_UNLOCK(sc);

	error = sdio_setup_intr(func, rtwn_sdio_intr, sc);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "cannot install the card interrupt handler, "
		    "error %d\n", error);
		goto fail;
	}
	sd->sd_intr = 1;

	RTWN_SDIO_LOG(sc, "attached, MAC %s\n", ether_sprintf(ic->ic_macaddr));
	return (0);

fail:
	RTWN_SDIO_LOG(sc, "attach failed, error %d; the card is left alone\n",
	    error);
	if (sd->sd_mac_on) {
		RTWN_LOCK(sc);
		rtwn_power_off(sc);
		RTWN_UNLOCK(sc);
	}
	if (attached)
		rtwn_detach(sc);
	if (sd->sd_status_tq != NULL) {
		taskqueue_drain_timeout(sd->sd_status_tq, &sd->sd_status_task);
		taskqueue_free(sd->sd_status_tq);
		sd->sd_status_tq = NULL;
	}
	if (sd->sd_tq != NULL) {
		taskqueue_drain(sd->sd_tq, &sd->sd_tx_task);
		taskqueue_free(sd->sd_tq);
		sd->sd_tq = NULL;
	}
	rtwn_sdio_free_tx_list(sc);
	free(sd->sd_rx_buf, M_DEVBUF);
	sd->sd_rx_buf = NULL;
	if (sc->sc_detach_private != NULL)
		rtwn_detach_private(sc);
	sdio_disable_func(func);
	mtx_destroy(&sc->sc_mtx);
	return (ENXIO);
}

static int
rtwn_sdio_detach(device_t self)
{
	struct rtwn_sdio_softc *sd = device_get_softc(self);
	struct rtwn_softc *sc = &sd->sd_sc;

	/* Without the lock: this waits for a running handler, which takes it. */
	if (sd->sd_intr) {
		sdio_teardown_intr(sd->sd_func);
		sd->sd_intr = 0;
	}

	/* Generic detach; stops the device (rtwn_stop(): power off). */
	rtwn_detach(sc);

	if (sd->sd_status_tq != NULL) {
		taskqueue_drain_timeout(sd->sd_status_tq, &sd->sd_status_task);
		taskqueue_free(sd->sd_status_tq);
		sd->sd_status_tq = NULL;
	}
	if (sd->sd_tq != NULL) {
		taskqueue_drain(sd->sd_tq, &sd->sd_tx_task);
		taskqueue_free(sd->sd_tq);
		sd->sd_tq = NULL;
	}

	RTWN_LOCK(sc);
	rtwn_sdio_reset_tx_list(sc, NULL);
	RTWN_UNLOCK(sc);
	rtwn_sdio_free_tx_list(sc);
	free(sd->sd_rx_buf, M_DEVBUF);
	sd->sd_rx_buf = NULL;

	rtwn_detach_private(sc);
	sdio_disable_func(sd->sd_func);
	mtx_destroy(&sc->sc_mtx);

	RTWN_SDIO_LOG(sc, "detached\n");
	return (0);
}

static int
rtwn_sdio_suspend(device_t self)
{
	struct rtwn_sdio_softc *sd = device_get_softc(self);

	rtwn_suspend(&sd->sd_sc);
	return (0);
}

static int
rtwn_sdio_resume(device_t self)
{
	struct rtwn_sdio_softc *sd = device_get_softc(self);

	rtwn_resume(&sd->sd_sc);
	return (0);
}

static device_method_t rtwn_sdio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rtwn_sdio_probe),
	DEVMETHOD(device_attach,	rtwn_sdio_attach),
	DEVMETHOD(device_detach,	rtwn_sdio_detach),
	DEVMETHOD(device_suspend,	rtwn_sdio_suspend),
	DEVMETHOD(device_resume,	rtwn_sdio_resume),

	DEVMETHOD_END
};

static driver_t rtwn_sdio_driver = {
	"rtwn",
	rtwn_sdio_methods,
	sizeof(struct rtwn_sdio_softc)
};

DRIVER_MODULE(rtwn_sdio, sdiob, rtwn_sdio_driver, NULL, NULL);
MODULE_VERSION(rtwn_sdio, 1);
MODULE_DEPEND(rtwn_sdio, wlan, 1, 1, 1);
MODULE_DEPEND(rtwn_sdio, rtwn, 2, 2, 2);
