/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710B chip identification, SYSON registers and eFuse.
 *
 * Behaviour follows what Linux's rtl8xxxu does for this chip (8710b.c:
 * rtl8710b_indirect_read32/write32(), rtl8710b_read_efuse8(),
 * rtl8710bu_identify_chip(), rtl8710b_read_efuse(),
 * rtl8710bu_parse_efuse()); the code is rtwn's own.
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
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>
#include <dev/rtwn/rtl8192c/r92c_rom_defs.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>
#include <dev/rtwn/rtl8710b/r10b_rom_image.h>
#include <dev/rtwn/rtl8710b/r10b_var.h>

#define R10B_INDIRECT_POLL	255
#define R10B_EFUSE_POLL		500

/*
 * One command through the USB host indirect window: address, then data
 * for a write, then the command; the busy bit clears when it is done.
 */
static int
r10b_indirect_wait(struct rtwn_softc *sc)
{
	uint32_t ctrl;
	int ntries;

	for (ntries = 0; ntries < R10B_INDIRECT_POLL; ntries++) {
		ctrl = rtwn_read_4(sc, R10B_INDIRECT_CTRL);
		/* All ones: the USB read failed (device gone), stop now. */
		if (ctrl == 0xffffffff)
			return (EIO);
		if (!(ctrl & R10B_INDIRECT_CTRL_BUSY))
			return (0);
	}
	return (ETIMEDOUT);
}

uint32_t
r10b_syson_read(struct rtwn_softc *sc, uint32_t addr)
{
	KASSERT((addr & 3) == 0, ("%s: unaligned 0x%x", __func__, addr));

	rtwn_write_4(sc, R10B_INDIRECT_ADDR, R10B_SYSON_BASE | addr);
	rtwn_write_4(sc, R10B_INDIRECT_CTRL, R10B_INDIRECT_CMD_REG_READ);
	if (r10b_indirect_wait(sc) != 0) {
		device_printf(sc->sc_dev,
		    "%s: SYSON register 0x%03x read timed out\n", __func__,
		    addr);
		return (0xffffffff);
	}
	return (rtwn_read_4(sc, R10B_INDIRECT_DATA));
}

int
r10b_syson_write(struct rtwn_softc *sc, uint32_t addr, uint32_t val)
{
	KASSERT((addr & 3) == 0, ("%s: unaligned 0x%x", __func__, addr));

	rtwn_write_4(sc, R10B_INDIRECT_ADDR, R10B_SYSON_BASE | addr);
	rtwn_write_4(sc, R10B_INDIRECT_DATA, val);
	rtwn_write_4(sc, R10B_INDIRECT_CTRL, R10B_INDIRECT_CMD_REG_WRITE);
	if (r10b_indirect_wait(sc) != 0) {
		device_printf(sc->sc_dev,
		    "%s: SYSON register 0x%03x write timed out\n", __func__,
		    addr);
		return (ETIMEDOUT);
	}
	return (0);
}

/* One byte of the physical eFuse. */
int
r10b_efuse_read_1(struct rtwn_softc *sc, uint16_t addr, uint8_t *val)
{
	uint32_t ctrl = 0;
	int ntries;

	rtwn_write_4(sc, R10B_INDIRECT_ADDR, addr);
	rtwn_write_4(sc, R10B_INDIRECT_CTRL, R10B_INDIRECT_CMD_EFUSE_READ);
	for (ntries = 0; ntries < R10B_EFUSE_POLL; ntries++) {
		ctrl = rtwn_read_4(sc, R10B_INDIRECT_CTRL);
		if (ctrl == 0xffffffff || !(ctrl & R10B_INDIRECT_CTRL_BUSY))
			break;
	}
	if (ntries == R10B_EFUSE_POLL || ctrl == 0xffffffff) {
		device_printf(sc->sc_dev,
		    "could not read efuse byte at address 0x%x\n", addr);
		return (ETIMEDOUT);
	}
	*val = rtwn_read_4(sc, R10B_INDIRECT_DATA) & 0xff;
	return (0);
}

static const char *
r10b_vendor_name(uint8_t vendor)
{
	switch (vendor) {
	case R10B_VENDOR_SMIC:
		return ("SMIC");
	case R10B_VENDOR_TSMC:
		return ("TSMC");
	case R10B_VENDOR_UMC:
		return ("UMC");
	default:
		return ("unknown");
	}
}

/*
 * Runs before the eFuse map is read (and before anything else touches the
 * chip): identifies the die, picks the firmware for its foundry and the
 * register tables for its package, and loads the table file. Any failure
 * here fails the attachment with a message saying why.
 */
int
r10b_efuse_preread(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	uint32_t cfg0, cfg2;
	uint8_t package = 0xff;

	cfg0 = r10b_syson_read(sc, R10B_SYSON_SYSTEM_CFG0);
	if (cfg0 == 0xffffffff)
		return (EIO);
	if (cfg0 & R10B_SYSON_SYSTEM_CFG0_TEST) {
		device_printf(sc->sc_dev, "RTL8710B test chip, not supported\n");
		return (EOPNOTSUPP);
	}
	rs->chip_cut = cfg0 & R10B_SYSON_SYSTEM_CFG0_CUT;
	/* Note: not the vendor encoding of R92C_SYS_CFG (SMIC and TSMC swap). */
	rs->vendor = (cfg0 & R10B_SYSON_SYSTEM_CFG0_VENDOR) >> 6;
	cfg2 = r10b_syson_read(sc, R10B_SYSON_SYSTEM_CFG2);
	rs->rom_rev = cfg2 & 0x0f;

	switch (rs->vendor) {
	case R10B_VENDOR_SMIC:
		sc->fwname = R10B_FWNAME_SMIC;
		break;
	case R10B_VENDOR_UMC:
		sc->fwname = R10B_FWNAME_UMC;
		break;
	default:
		/* There is a firmware image for SMIC and UMC dies only. */
		device_printf(sc->sc_dev,
		    "RTL8710B made by %s: no firmware for this die\n",
		    r10b_vendor_name(rs->vendor));
		return (ENXIO);
	}

	/*
	 * The package type selects the BB and RF tables. As the vendor
	 * driver does: an unprogrammed byte means the foundry's usual
	 * package (UMC: QFN48M-U, SMIC: QFN48M-S), anything else unknown
	 * means QFN48M-U, whose values are also the vendor's defaults.
	 */
	if (r10b_efuse_read_1(sc, R10B_EFUSE_PACKAGE_TYPE, &package) != 0)
		package = 0xff;
	if (package == 0xff) {
		package = rs->vendor == R10B_VENDOR_SMIC ?
		    R10B_PACKAGE_QFN48M_S : R10B_PACKAGE_QFN48M_U;
	} else if (package != R10B_PACKAGE_QFN48M_U &&
	    package != R10B_PACKAGE_QFN48M_S) {
		device_printf(sc->sc_dev,
		    "unknown package type 0x%02x, using QFN48M-U tables\n",
		    package);
		package = R10B_PACKAGE_QFN48M_U;
	}
	rs->package = package;

	device_printf(sc->sc_dev,
	    "RTL8710B (RTL8188GU), %s die, cut %c, ROM %u, package %s\n",
	    r10b_vendor_name(rs->vendor), 'A' + rs->chip_cut, rs->rom_rev,
	    package == R10B_PACKAGE_QFN48M_S ? "QFN48M-S" : "QFN48M-U");

	return (r10b_tables_load(sc));
}

/*
 * The logical eFuse map, from the physical eFuse through the indirect
 * window. The physical format is the usual Realtek one (section headers,
 * extended headers, word enable masks) that rtwn_efuse_read() parses for
 * the other chips.
 */
int
r10b_efuse_read(struct rtwn_softc *sc, uint8_t *rom, uint16_t size)
{
	struct r10b_rom *map = (struct r10b_rom *)rom;
	uint32_t ctrl;
	uint16_t addr = 0;
	uint8_t hdr, ext, msk, off, lo, hi;
	int i, error = 0;

	KASSERT(size == sizeof(struct r10b_rom), ("%s: size %u", __func__,
	    size));

	ctrl = r10b_syson_read(sc, R10B_SYSON_EEPROM_CTRL0);
	RTWN_DPRINTF(sc, RTWN_DEBUG_ROM, "%s: %s, boot from %s\n", __func__,
	    (ctrl & R10B_SYSON_EEPROM_EN) ? "EEPROM" : "eFuse",
	    (ctrl & R10B_SYSON_EEPROM_BOOT) ? "EEPROM" : "eFuse");

	memset(rom, 0xff, size);

	while (addr < sc->efuse_maxlen) {
		if ((error = r10b_efuse_read_1(sc, addr++, &hdr)) != 0)
			goto end;
		if (hdr == 0xff)
			break;
		if ((hdr & 0x1f) == 0x0f) {
			/* Extended header: section number in two parts. */
			if ((error = r10b_efuse_read_1(sc, addr++, &ext)) != 0)
				goto end;
			if ((ext & 0x0f) == 0x0f)
				continue;	/* no word enabled */
			off = ((ext & 0xf0) >> 1) | (hdr >> 5);
			msk = ext & 0x0f;
		} else {
			off = hdr >> 4;
			msk = hdr & 0x0f;
		}

		for (i = 0; i < 4; i++) {
			int pos = off * 8 + i * 2;

			if (msk & (1 << i))
				continue;
			if ((error = r10b_efuse_read_1(sc, addr++, &lo)) != 0 ||
			    (error = r10b_efuse_read_1(sc, addr++, &hi)) != 0)
				goto end;
			if (pos + 1 >= size) {
				device_printf(sc->sc_dev,
				    "%s: eFuse section 0x%x is outside the map\n",
				    __func__, off);
				error = EINVAL;
				goto end;
			}
			rom[pos] = lo;
			rom[pos + 1] = hi;
		}
	}

end:
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: error while reading ROM\n",
		    __func__);
		return (error);
	}

	if (le16toh(map->id) != R10B_EFUSE_ID) {
		device_printf(sc->sc_dev,
		    "eFuse map id 0x%04x is not an RTL8710B's (0x%04x)\n",
		    le16toh(map->id), R10B_EFUSE_ID);
		return (EINVAL);
	}

	return (0);
}

void
r10b_parse_rom(struct rtwn_softc *sc, uint8_t *buf)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	struct rtwn_r88e_txpwr *rt = &rs->txpwr;
	struct r10b_rom *rom = (struct r10b_rom *)buf;
	u_int i;

	/* TX power: the RTL8188F block and its defaults (r88f_parse_rom()). */
	rt->bw20_tx_pwr_diff = RTWN_SIGN4TO8(MS(rom->tx_pwr_diff, HIGH_PART));
	rt->ofdm_tx_pwr_diff = RTWN_SIGN4TO8(MS(rom->tx_pwr_diff, LOW_PART));
	for (i = 0; i < nitems(rom->ht40_tx_pwr); i++) {
		rt->ht40_tx_pwr[i] = rom->ht40_tx_pwr[i];
		if (rt->ht40_tx_pwr[i] > 0x3f)
			rt->ht40_tx_pwr[i] = 0x27;
	}
	for (i = 0; i < nitems(rom->cck_tx_pwr); i++) {
		rt->cck_tx_pwr[i] = rom->cck_tx_pwr[i];
		if (rt->cck_tx_pwr[i] > 0x3f)
			rt->cck_tx_pwr[i] = 0x22;
	}

	rs->xtal_k = rom->crystalcap;
	rs->rs.crystalcap = rom->crystalcap & 0x3f;
	rs->rs.regulatory = MS(rom->rf_board_opt, R92C_ROM_RF1_REGULATORY);
	rs->rs.board_type =
	    MS(RTWN_GET_ROM_VAR(rom->rf_board_opt, R92C_BOARD_TYPE_DONGLE),
		R92C_ROM_RF1_BOARD_TYPE);
	sc->thermal_meter = rom->thermal_meter;

	IEEE80211_ADDR_COPY(sc->sc_ic.ic_macaddr, rom->macaddr);

	RTWN_DPRINTF(sc, RTWN_DEBUG_ROM,
	    "%s: regulatory %d, crystal 0x%02x, thermal 0x%02x, "
	    "eFuse version %u, USB %04x:%04x\n", __func__, rs->rs.regulatory,
	    rom->crystalcap, rom->thermal_meter, rom->version,
	    le16toh(rom->vid), le16toh(rom->pid));
}
