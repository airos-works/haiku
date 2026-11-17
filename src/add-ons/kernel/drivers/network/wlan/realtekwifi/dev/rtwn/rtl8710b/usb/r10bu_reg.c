/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710BU register access: the address translation between rtwn and the
 * USB register methods.
 *
 * - The MAC registers 0x00..0xff answer at 0x8000 + address over USB
 *   (rtl8xxxu adds the offset in rtl8xxxu_read*()/rtl8xxxu_write*()).
 * - rtwn_load_firmware() drives R92C_MCUFWDL (0x80..0x83); the RTL8710B's
 *   8051 firmware control is R10B_8051FW_CTRL (0x90..0x93), with the same
 *   bit layout in its low byte and the page field, while 0x80 is its
 *   host interrupt mask. Generic accesses to 0x80..0x83 go to 0x90..0x93;
 *   the chip code reaches R10B_HIMR0 with r10bu_write_4_direct().
 * - rtwn_read_chipid() reads R92C_SYS_CFG (0xf0, 32 bits) to refuse test
 *   chips; the RTL8710B has its system configuration in SYSON
 *   SYSTEM_CFG0 and 0xf0 is its WL_STATUS. That read is answered from
 *   SYSON (only the test chip bit means anything to rtwn; the vendor is
 *   read again in r10b_efuse_preread()). 8-bit accesses to 0xf0 are
 *   WL_STATUS.
 * - Firmware blocks go out four bytes per control transfer, as rtl8xxxu
 *   writes this chip's firmware (its writeN_block_size).
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
#include <dev/rtwn/rtl8710b/r10b_var.h>
#include <dev/rtwn/rtl8710b/usb/r10bu.h>

#define R10BU_FW_BLOCK_SIZE	4

static uint16_t
r10bu_bus_addr(uint16_t addr)
{
	return (addr < R10B_USB_LOW_REG_END ? addr | R10B_USB_LOW_REG_BASE :
	    addr);
}

static uint16_t
r10bu_addr(uint16_t addr)
{
	if (addr >= R92C_MCUFWDL && addr < R92C_MCUFWDL + 4)
		addr += R10B_8051FW_CTRL - R92C_MCUFWDL;
	return (r10bu_bus_addr(addr));
}

static int
r10bu_write_1(struct rtwn_softc *sc, uint16_t addr, uint8_t val)
{
	return (R10B_SOFTC(sc)->raw_write_1(sc, r10bu_addr(addr), val));
}

static int
r10bu_write_2(struct rtwn_softc *sc, uint16_t addr, uint16_t val)
{
	return (R10B_SOFTC(sc)->raw_write_2(sc, r10bu_addr(addr), val));
}

static int
r10bu_write_4(struct rtwn_softc *sc, uint16_t addr, uint32_t val)
{
	return (R10B_SOFTC(sc)->raw_write_4(sc, r10bu_addr(addr), val));
}

static uint8_t
r10bu_read_1(struct rtwn_softc *sc, uint16_t addr)
{
	return (R10B_SOFTC(sc)->raw_read_1(sc, r10bu_addr(addr)));
}

static uint16_t
r10bu_read_2(struct rtwn_softc *sc, uint16_t addr)
{
	return (R10B_SOFTC(sc)->raw_read_2(sc, r10bu_addr(addr)));
}

static uint32_t
r10bu_read_4(struct rtwn_softc *sc, uint16_t addr)
{
	uint32_t cfg0;

	if (addr == R92C_SYS_CFG) {
		cfg0 = r10b_syson_read(sc, R10B_SYSON_SYSTEM_CFG0);
		if (cfg0 == 0xffffffff)
			return (0);	/* r10b_efuse_preread() reports it */
		return ((cfg0 & R10B_SYSON_SYSTEM_CFG0_TEST) ?
		    R92C_SYS_CFG_TRP_VAUX_EN : 0);
	}
	return (R10B_SOFTC(sc)->raw_read_4(sc, r10bu_addr(addr)));
}

static int
r10bu_fw_write_block(struct rtwn_softc *sc, const uint8_t *buf, uint16_t reg,
    int len)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	int error, n;

	while (len > 0) {
		n = MIN(len, R10BU_FW_BLOCK_SIZE);
		error = rs->raw_fw_write_block(sc, buf, reg, n);
		if (error != 0)
			return (error);
		buf += n;
		reg += n;
		len -= n;
	}
	return (0);
}

/* A MAC register by its RTL8710B address, without the MCUFWDL redirect. */
int
r10bu_write_4_direct(struct rtwn_softc *sc, uint16_t addr, uint32_t val)
{
	return (R10B_SOFTC(sc)->raw_write_4(sc, r10bu_bus_addr(addr), val));
}

/* Called from the attachment once the bus methods and sc_priv are set. */
void
r10bu_attach_reg_methods(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);

	rs->raw_write_1 = sc->sc_write_1;
	rs->raw_write_2 = sc->sc_write_2;
	rs->raw_write_4 = sc->sc_write_4;
	rs->raw_read_1 = sc->sc_read_1;
	rs->raw_read_2 = sc->sc_read_2;
	rs->raw_read_4 = sc->sc_read_4;
	rs->raw_fw_write_block = sc->sc_fw_write_block;

	sc->sc_write_1 = r10bu_write_1;
	sc->sc_write_2 = r10bu_write_2;
	sc->sc_write_4 = r10bu_write_4;
	sc->sc_read_1 = r10bu_read_1;
	sc->sc_read_2 = r10bu_read_2;
	sc->sc_read_4 = r10bu_read_4;
	sc->sc_fw_write_block = r10bu_fw_write_block;
}
