/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Register access of the SDIO backend: MAC registers through the MAC
 * domain, the SDIO local registers through theirs (rtwn_sdio_reg.h).
 *
 * How wide an access may be follows what Linux's drivers do (staging
 * rtl8723bs hal/sdio_ops.c sdio_read32()/sdio_write32()/sdio_local_read();
 * rtw88 sdio.c rtw_sdio_read32()/rtw_sdio_writel()): bytes and 16-bit values
 * go as CMD52s, low byte first; an aligned 32-bit value goes as one 4-byte
 * CMD53 once the MAC is on (some registers, the BB ones among them, take
 * the value when their last byte is written, so it has to arrive whole),
 * except below 0x100 (the system registers, reachable in any power state);
 * everything else, and everything while the MAC is off, as CMD52s.
 *
 * Every transfer runs with the softc lock held (the bus serializes them
 * anyway). After RTWN_SDIO_MAX_ERRORS failures in a row the card counts as
 * gone: nothing is sent any more, reads return all ones and writes ENXIO,
 * so that a card that stopped answering does not keep the driver spinning
 * through timeouts.
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

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/sdio/sdio_subr.h>

#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>
#include <dev/rtwn/sdio/rtwn_sdio_frame.h>

static int
rtwn_sdio_check(struct rtwn_sdio_softc *sd, int error, const char *what,
    uint32_t ioaddr)
{
	struct rtwn_softc *sc = &sd->sd_sc;

	if (error == 0) {
		sd->sd_errors = 0;
		return (0);
	}

	if (++sd->sd_errors <= 4) {
		RTWN_SDIO_LOG(sc, "%s at I/O address %#x failed, error %d\n",
		    what, ioaddr, error);
	}
	if (sd->sd_errors >= RTWN_SDIO_MAX_ERRORS && !sd->sd_gone) {
		sd->sd_gone = 1;
		RTWN_SDIO_LOG(sc, "%d transfers in a row failed: the card does "
		    "not answer, no more I/O to it\n", sd->sd_errors);
	}
	return (error);
}

static int
rtwn_sdio_cmd52_read(struct rtwn_sdio_softc *sd, uint32_t ioaddr,
    uint8_t *buf, int len)
{
	int error = 0, i;

	if (sd->sd_gone)
		return (ENXIO);
	for (i = 0; i < len; i++) {
		buf[i] = sdio_read_1(sd->sd_func, ioaddr + i, &error);
		if (rtwn_sdio_check(sd, error, "CMD52 read", ioaddr + i) != 0)
			return (error);
	}
	return (0);
}

static int
rtwn_sdio_cmd52_write(struct rtwn_sdio_softc *sd, uint32_t ioaddr,
    const uint8_t *buf, int len)
{
	int error = 0, i;

	if (sd->sd_gone)
		return (ENXIO);
	for (i = 0; i < len; i++) {
		sdio_write_1(sd->sd_func, ioaddr + i, buf[i], &error);
		if (rtwn_sdio_check(sd, error, "CMD52 write", ioaddr + i) != 0)
			return (error);
	}
	return (0);
}

static int
rtwn_sdio_cmd53_read(struct rtwn_sdio_softc *sd, uint32_t ioaddr,
    void *buf, size_t len)
{
	if (sd->sd_gone)
		return (ENXIO);
	return (rtwn_sdio_check(sd,
	    sdio_read_extended(sd->sd_func, ioaddr, true, buf, len),
	    "CMD53 read", ioaddr));
}

static int
rtwn_sdio_cmd53_write(struct rtwn_sdio_softc *sd, uint32_t ioaddr,
    const void *buf, size_t len)
{
	if (sd->sd_gone)
		return (ENXIO);
	return (rtwn_sdio_check(sd,
	    sdio_write_extended(sd->sd_func, ioaddr, true, buf, len),
	    "CMD53 write", ioaddr));
}

/* Whether a 4-byte access at \a reg (MAC) may be one CMD53. */
static int
rtwn_sdio_mac_dword_cmd53(struct rtwn_sdio_softc *sd, uint16_t reg)
{
	return (sd->sd_mac_on && reg >= 0x100 && (reg & 3) == 0);
}

/*
 * SDIO local registers.
 */

int
rtwn_sdio_read_local(struct rtwn_softc *sc, uint32_t reg, void *buf, int len)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t ioaddr = rtwn_sdio_local_addr(reg);

	if (sd->sd_mac_on && len == 4 && (reg & 3) == 0)
		return (rtwn_sdio_cmd53_read(sd, ioaddr, buf, 4));
	return (rtwn_sdio_cmd52_read(sd, ioaddr, buf, len));
}

int
rtwn_sdio_write_local(struct rtwn_softc *sc, uint32_t reg, const void *buf,
    int len)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t ioaddr = rtwn_sdio_local_addr(reg);

	if (sd->sd_mac_on && len == 4 && (reg & 3) == 0)
		return (rtwn_sdio_cmd53_write(sd, ioaddr, buf, 4));
	return (rtwn_sdio_cmd52_write(sd, ioaddr, buf, len));
}

uint8_t
rtwn_sdio_read_local_1(struct rtwn_softc *sc, uint32_t reg)
{
	uint8_t val;

	if (rtwn_sdio_read_local(sc, reg, &val, 1) != 0)
		return (0xff);
	return (val);
}

uint16_t
rtwn_sdio_read_local_2(struct rtwn_softc *sc, uint32_t reg)
{
	uint8_t buf[2];

	if (rtwn_sdio_read_local(sc, reg, buf, 2) != 0)
		return (0xffff);
	return (buf[0] | (buf[1] << 8));
}

uint32_t
rtwn_sdio_read_local_4(struct rtwn_softc *sc, uint32_t reg)
{
	uint32_t val;

	if (rtwn_sdio_read_local(sc, reg, &val, 4) != 0)
		return (0xffffffff);
	return (le32toh(val));
}

int
rtwn_sdio_write_local_1(struct rtwn_softc *sc, uint32_t reg, uint8_t val)
{
	return (rtwn_sdio_write_local(sc, reg, &val, 1));
}

int
rtwn_sdio_write_local_4(struct rtwn_softc *sc, uint32_t reg, uint32_t val)
{
	val = htole32(val);
	return (rtwn_sdio_write_local(sc, reg, &val, 4));
}

/*
 * Polls an SDIO local register until (value & mask) == want, for at most
 * \a timeout_us. Returns ETIMEDOUT (or the bus error) otherwise.
 */
int
rtwn_sdio_poll_local_1(struct rtwn_softc *sc, uint32_t reg, uint8_t mask,
    uint8_t want, int timeout_us)
{
	uint8_t val = 0;
	int waited, error;

	for (waited = 0; ; waited += 50) {
		error = rtwn_sdio_read_local(sc, reg, &val, 1);
		if (error != 0)
			return (error);
		if ((val & mask) == want)
			return (0);
		if (waited >= timeout_us)
			break;
		DELAY(50);
	}
	RTWN_SDIO_LOG(sc, "local register %#x stays %#x (mask %#x, waiting "
	    "for %#x) after %d us\n", reg, val, mask, want, timeout_us);
	return (ETIMEDOUT);
}

/*
 * MAC registers: the rtwn_softc methods.
 */

int
rtwn_sdio_write_1(struct rtwn_softc *sc, uint16_t reg, uint8_t val)
{
	return (rtwn_sdio_cmd52_write(RTWN_SDIO_SOFTC(sc),
	    rtwn_sdio_mac_addr(reg), &val, 1));
}

int
rtwn_sdio_write_2(struct rtwn_softc *sc, uint16_t reg, uint16_t val)
{
	uint8_t buf[2] = { val & 0xff, val >> 8 };

	return (rtwn_sdio_cmd52_write(RTWN_SDIO_SOFTC(sc),
	    rtwn_sdio_mac_addr(reg), buf, 2));
}

int
rtwn_sdio_write_4(struct rtwn_softc *sc, uint16_t reg, uint32_t val)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t le = htole32(val);

	if (rtwn_sdio_mac_dword_cmd53(sd, reg))
		return (rtwn_sdio_cmd53_write(sd, rtwn_sdio_mac_addr(reg), &le,
		    4));
	return (rtwn_sdio_cmd52_write(sd, rtwn_sdio_mac_addr(reg),
	    (const uint8_t *)&le, 4));
}

uint8_t
rtwn_sdio_read_1(struct rtwn_softc *sc, uint16_t reg)
{
	uint8_t val;

	if (rtwn_sdio_cmd52_read(RTWN_SDIO_SOFTC(sc), rtwn_sdio_mac_addr(reg),
	    &val, 1) != 0)
		return (0xff);
	return (val);
}

uint16_t
rtwn_sdio_read_2(struct rtwn_softc *sc, uint16_t reg)
{
	uint8_t buf[2];

	if (rtwn_sdio_cmd52_read(RTWN_SDIO_SOFTC(sc), rtwn_sdio_mac_addr(reg),
	    buf, 2) != 0)
		return (0xffff);
	return (buf[0] | (buf[1] << 8));
}

uint32_t
rtwn_sdio_read_4(struct rtwn_softc *sc, uint16_t reg)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t le;
	int error;

	if (rtwn_sdio_mac_dword_cmd53(sd, reg))
		error = rtwn_sdio_cmd53_read(sd, rtwn_sdio_mac_addr(reg), &le, 4);
	else
		error = rtwn_sdio_cmd52_read(sd, rtwn_sdio_mac_addr(reg),
		    (uint8_t *)&le, 4);
	if (error != 0)
		return (0xffffffff);
	return (le32toh(le));
}

void
rtwn_sdio_delay(struct rtwn_softc *sc, int usec)
{
	/*
	 * Spin for short waits; sleep (keeping the lock: the interrupt
	 * handler and the TX task wait, which is what the init sequences
	 * want) for the long ones, the calibration's 200 ms among them.
	 */
	if (usec < 1000)
		DELAY(usec);
	else
		pause("rtwnsd", MAX(1, howmany(usec, 1000) * hz / 1000));
}

/*
 * Firmware download: rtwn_fw_loadpage() hands over the image in pieces of
 * up to 196 bytes for the 4 KiB window at 0x1000. They go one dword per
 * 4-byte CMD53, the rest byte by byte, as the vendor driver writes the
 * window (staging rtl8723bs hal/rtl8723b_hal_init.c _BlockWrite(): "use
 * 4-byte write to download FW", rtw_write32() per dword; rtw88's legacy
 * download writes dwords too). A longer CMD53 into the MAC domain does not
 * land: the first laptop that ran this (#133) wrote 196-byte CMD53s without
 * a bus error and never got the checksum report.
 */
int
rtwn_sdio_fw_write_block(struct rtwn_softc *sc, const uint8_t *buf,
    uint16_t reg, int len)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	int error;

	while (len >= 4 && (reg & 3) == 0) {
		if (sd->sd_mac_on)
			error = rtwn_sdio_cmd53_write(sd,
			    rtwn_sdio_mac_addr(reg), buf, 4);
		else
			error = rtwn_sdio_cmd52_write(sd,
			    rtwn_sdio_mac_addr(reg), buf, 4);
		if (error != 0)
			return (error);
		buf += 4;
		reg += 4;
		len -= 4;
	}
	if (len == 0)
		return (0);
	return (rtwn_sdio_cmd52_write(sd, rtwn_sdio_mac_addr(reg), buf, len));
}

/* TX and RX FIFOs: one CMD53, \a len already padded (rtwn_sdio_xfer_len). */
int
rtwn_sdio_fifo_write(struct rtwn_softc *sc, uint32_t ioaddr, const void *buf,
    size_t len)
{
	return (rtwn_sdio_cmd53_write(RTWN_SDIO_SOFTC(sc), ioaddr, buf, len));
}

int
rtwn_sdio_fifo_read(struct rtwn_softc *sc, uint32_t ioaddr, void *buf,
    size_t len)
{
	return (rtwn_sdio_cmd53_read(RTWN_SDIO_SOFTC(sc), ioaddr, buf, len));
}
