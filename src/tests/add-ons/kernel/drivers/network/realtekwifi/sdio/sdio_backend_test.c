/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*
 * Host test of realtekwifi's SDIO backend (airos-works/haiku #133).
 *
 * 1. The address and framing helpers (dev/rtwn/sdio/rtwn_sdio_frame.h)
 *    against hand-made values and buffers.
 * 2. The RTL8723BS power sequences (rtl8723b/sdio/r23bs_init.c around
 *    rtl8723b/r23b_pwr.c, through sdio/rtwn_sdio_reg.c) against a fake SDIO
 *    function that models the few power-state registers the sequences wait
 *    on and records every CMD52/CMD53. The recorded writes must contain, in
 *    order, the steps of Realtek's RTL8723B power tables for the SDIO
 *    interface (card disabled -> card emulation -> active, and back), as
 *    listed in the kExpected* tables below; nothing but CMD52 may reach the
 *    chip while its MAC is off; timeouts must end in bounded time.
 * 3. The give-up rule: after RTWN_SDIO_MAX_ERRORS failed transfers nothing
 *    more is sent.
 *
 * run.sh builds and runs it; no Haiku needed.
 */

#include <dev/sdio/sdio_subr.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>
#include <dev/rtwn/sdio/rtwn_sdio_frame.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>
#include <dev/rtwn/rtl8192e/r92e_var.h>
#include <dev/rtwn/rtl8723b/r23b_reg.h>
#include <dev/rtwn/rtl8723b/r23b_var.h>
#include <dev/rtwn/rtl8723b/sdio/r23bs.h>


static int sFailures;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__,	\
		    #cond);						\
		sFailures++;						\
	}								\
} while (0)


/* #pragma mark - the clock and the log */


static uint64_t sNow;	/* us */
static int sQuiet;

void
test_delay(int usec)
{
	sNow += usec;
}

void
test_log(const char *fmt, ...)
{
	va_list args;

	if (sQuiet)
		return;
	va_start(args, fmt);
	printf("    log: ");
	vprintf(fmt, args);
	va_end(args);
}

void *
device_get_ivars(device_t dev)
{
	return (NULL);
}


/* #pragma mark - the fake SDIO function */


enum { OP_R52, OP_W52, OP_R53, OP_W53 };

struct op {
	int		kind;
	int		dom;
	uint32_t	reg;
	uint32_t	val;	/* CMD53: the first four bytes */
	size_t		len;
	int		mac_on;	/* the model's MAC state when it happened */
};

#define MAX_OPS	200000
static struct op sOps[MAX_OPS];
static int sOpCount;

static uint8_t sMac[0x10000];
static uint8_t sLocal[0x1000];
static int sMacOn;		/* the model's MAC power */
static int sHsusStuck;		/* SDIO interface never leaves suspend */
static int sOnMacStuck;		/* ONMAC request never completes */
static int sFailAll;		/* every transfer fails */
static int sBusCalls;

static void
model_reset(void)
{
	memset(sMac, 0, sizeof(sMac));
	memset(sLocal, 0, sizeof(sLocal));
	sOpCount = 0;
	sMacOn = 0;
	sHsusStuck = sOnMacStuck = sFailAll = 0;
	sBusCalls = 0;
	sNow = 0;

	/* card disabled: HCI suspended, power ready (SUS_HOST) */
	sLocal[RTWN_SDIO_HSUS_CTRL] = RTWN_SDIO_HSUS_SUS_REQ;
	sMac[R92C_APS_FSMCO + 2] = R92C_APS_FSMCO_SUS_HOST >> 16;
	sMac[R92C_SYS_CFG] = 0x00;
}

static void
decode(uint32_t ioaddr, int *dom, uint32_t *reg)
{
	if (ioaddr & 0x10000) {
		*dom = RTWN_SDIO_DOM_MAC;
		*reg = ioaddr & 0xffff;
	} else {
		*dom = (ioaddr >> RTWN_SDIO_DOM_SHIFT) & 7;
		*reg = ioaddr & 0x1fff;
	}
}

static void
record(int kind, uint32_t ioaddr, uint32_t val, size_t len)
{
	struct op *op;

	if (sOpCount >= MAX_OPS)
		return;
	op = &sOps[sOpCount++];
	op->kind = kind;
	decode(ioaddr, &op->dom, &op->reg);
	op->val = val;
	op->len = len;
	op->mac_on = sMacOn;
}

static uint8_t
model_read(int dom, uint32_t reg)
{
	if (dom == RTWN_SDIO_DOM_LOCAL)
		return (sLocal[reg & 0xfff]);
	if (dom == RTWN_SDIO_DOM_MAC)
		return (sMac[reg & 0xffff]);
	return (0);
}

static void
model_write(int dom, uint32_t reg, uint8_t val)
{
	if (dom == RTWN_SDIO_DOM_LOCAL) {
		reg &= 0xfff;
		sLocal[reg] = val;
		if (reg == RTWN_SDIO_HSUS_CTRL && !sHsusStuck) {
			if (val & RTWN_SDIO_HSUS_SUS_REQ)
				sLocal[reg] &= ~RTWN_SDIO_HSUS_RESUME_RDY;
			else
				sLocal[reg] |= RTWN_SDIO_HSUS_RESUME_RDY;
		}
		return;
	}
	if (dom != RTWN_SDIO_DOM_MAC)
		return;

	reg &= 0xffff;
	if (reg == R92C_APS_FSMCO + 1) {
		/* APFM_ONMAC (bit 0) and APFM_OFF (bit 1): requests */
		if (val & 0x01)
			sMacOn = 1;
		if (val & 0x02)
			sMacOn = 0;
		sMac[reg] = val & (sOnMacStuck ? ~0x02 : ~0x03);
		return;
	}
	sMac[reg] = val;
}

uint8_t
sdio_read_1(struct sdio_func *f, uint32_t ioaddr, int *err)
{
	int dom;
	uint32_t reg;
	uint8_t val;

	sBusCalls++;
	if (sFailAll) {
		*err = EIO;
		return (0xff);
	}
	decode(ioaddr, &dom, &reg);
	val = model_read(dom, reg);
	record(OP_R52, ioaddr, val, 1);
	*err = 0;
	return (val);
}

void
sdio_write_1(struct sdio_func *f, uint32_t ioaddr, uint8_t val, int *err)
{
	int dom;
	uint32_t reg;

	sBusCalls++;
	if (sFailAll) {
		*err = EIO;
		return;
	}
	record(OP_W52, ioaddr, val, 1);
	decode(ioaddr, &dom, &reg);
	model_write(dom, reg, val);
	*err = 0;
}

int
sdio_read_extended(struct sdio_func *f, uint32_t ioaddr, bool inc, void *buf,
    size_t len)
{
	int dom;
	uint32_t reg, val = 0;
	size_t i;

	sBusCalls++;
	if (sFailAll)
		return (EIO);
	decode(ioaddr, &dom, &reg);
	for (i = 0; i < len; i++)
		((uint8_t *)buf)[i] = model_read(dom, reg + (inc ? i : 0));
	memcpy(&val, buf, len < 4 ? len : 4);
	record(OP_R53, ioaddr, le32toh(val), len);
	return (0);
}

int
sdio_write_extended(struct sdio_func *f, uint32_t ioaddr, bool inc,
    const void *buf, size_t len)
{
	int dom;
	uint32_t reg, val = 0;
	size_t i;

	sBusCalls++;
	if (sFailAll)
		return (EIO);
	memcpy(&val, buf, len < 4 ? len : 4);
	record(OP_W53, ioaddr, le32toh(val), len);
	decode(ioaddr, &dom, &reg);
	for (i = 0; i < len; i++)
		model_write(dom, reg + (inc ? i : 0), ((const uint8_t *)buf)[i]);
	return (0);
}


/* #pragma mark - what the chip code needs besides */


void
r23b_init_ampdu(struct rtwn_softc *sc)
{
}

int
r23b_efuse_preread(struct rtwn_softc *sc)
{
	return (0);
}

static void
fw_reset_stub(struct rtwn_softc *sc, int reason)
{
}


static struct rtwn_sdio_softc sSoftc;
static struct r23b_softc sChip;
static struct sdio_func sFunc;

static struct rtwn_softc *
setup_softc(void)
{
	struct rtwn_softc *sc = &sSoftc.sd_sc;

	memset(&sSoftc, 0, sizeof(sSoftc));
	memset(&sChip, 0, sizeof(sChip));
	memset(&sFunc, 0, sizeof(sFunc));
	sFunc.fn = 1;
	sSoftc.sd_func = &sFunc;
	sSoftc.sd_blksize = 512;

	sc->sc_write_1 = rtwn_sdio_write_1;
	sc->sc_write_2 = rtwn_sdio_write_2;
	sc->sc_write_4 = rtwn_sdio_write_4;
	sc->sc_read_1 = rtwn_sdio_read_1;
	sc->sc_read_2 = rtwn_sdio_read_2;
	sc->sc_read_4 = rtwn_sdio_read_4;
	sc->sc_delay = rtwn_sdio_delay;
	sc->sc_fw_reset = fw_reset_stub;
	sc->sc_hwcrypto = RTWN_CRYPTO_PAIR;

	/* as r23bs_parse_rom() leaves it for a module with 0xc3 = 0xff */
	sChip.ant_main = 1;
	sChip.bt_antnum = 1;
	sChip.rs_ant_local_write = r23bs_ant_local_write;
	sc->sc_priv = &sChip;
	return (sc);
}


/* #pragma mark - expected sequences */


/*
 * A step: a CMD52 write to (domain, register) whose value has \a want in
 * the bits of \a mask, or (POLL) a read of the register. They have to show
 * up in this order among the recorded operations; others may come between.
 */
#define LOC	RTWN_SDIO_DOM_LOCAL
#define MAC	RTWN_SDIO_DOM_MAC
#define POLL	0x100

struct step {
	int		dom;
	uint32_t	reg;
	uint32_t	mask;
	uint32_t	want;
	const char	*what;
};

/*
 * Realtek's RTL8723B power tables, the SDIO interface's view: card disabled
 * -> card emulation, card emulation -> active.
 */
static const struct step kExpectedPowerOn[] = {
	{ MAC, 0x01c, 0xff, 0x00, "unlock power control (RSV_CTRL)" },
	{ MAC, 0x005, 0x88, 0x00, "clear suspend and power-down enable" },
	{ LOC, 0x086, 0x01, 0x00, "clear the SDIO suspend request" },
	{ LOC | POLL, 0x086, 0, 0, "wait for the SDIO interface to resume" },
	{ MAC, 0x005, 0x18, 0x00, "WL suspend off" },
	{ MAC, 0x023, 0x10, 0x00, "12H LDO normal mode" },
	{ MAC, 0x020, 0x01, 0x01, "LDOA12 on" },
	{ MAC, 0x067, 0x10, 0x00, "BT_GPS_SEL pins off" },
	{ MAC, 0x000, 0x20, 0x00, "analog IPs not isolated" },
	{ MAC, 0x005, 0x1c, 0x00, "SW LPS and WL suspend off" },
	{ MAC | POLL, 0x006, 0, 0, "wait for power ready" },
	{ MAC, 0x006, 0x01, 0x01, "release WLON reset" },
	{ MAC, 0x005, 0x80, 0x00, "HW power down off" },
	{ MAC, 0x005, 0x18, 0x00, "WL suspend off" },
	{ MAC, 0x005, 0x01, 0x01, "ONMAC request" },
	{ MAC | POLL, 0x005, 0, 0, "wait for ONMAC" },
	{ MAC, 0x010, 0x40, 0x40, "WL controls the crystal" },
	{ MAC, 0x049, 0x02, 0x02, "GPIO interrupt falling edge" },
	{ MAC, 0x063, 0x02, 0x02, "GPIO9 interrupt mode" },
	{ MAC, 0x062, 0x02, 0x00, "GPIO9 input" },
	{ MAC, 0x058, 0x01, 0x01, "HSISR GPIO interrupt" },
	{ MAC, 0x05a, 0x02, 0x02, "HSISR GPIO9 interrupt" },
	{ MAC, 0x069, 0x40, 0x40, "GPIO9 pull up" },
	{ LOC, 0x060, 0xff, 0x00, "antenna setting: main port, one antenna" },
};

/* Active -> card emulation -> card disabled, the SDIO steps at the end. */
static const struct step kExpectedPowerOff[] = {
	{ MAC, 0x01f, 0xff, 0x00, "RF off" },
	{ MAC, 0x049, 0x02, 0x00, "GPIO interrupt rising edge" },
	{ MAC, 0x006, 0x01, 0x01, "release WLON reset" },
	{ MAC, 0x005, 0x02, 0x02, "MAC off request" },
	{ MAC | POLL, 0x005, 0, 0, "wait for MAC off" },
	{ MAC, 0x000, 0x20, 0x20, "analog IPs isolated" },
	{ MAC, 0x020, 0x01, 0x00, "LDOA12 off" },
	{ MAC, 0x005, 0x18, 0x08, "WL suspend on" },
	{ MAC, 0x007, 0xff, 0x20, "SOP option" },
	{ MAC, 0x023, 0x10, 0x10, "12H LDO sleep mode" },
	{ LOC, 0x086, 0x01, 0x01, "set the SDIO suspend request" },
	{ LOC | POLL, 0x086, 0, 0, "wait for the SDIO interface to suspend" },
};

static void
check_sequence(const char *name, const struct step *steps, int count,
    int from)
{
	int i, s = 0;

	for (i = from; i < sOpCount && s < count; i++) {
		const struct op *op = &sOps[i];
		const struct step *st = &steps[s];
		int dom = st->dom & ~POLL;

		if (op->dom != dom || op->reg != st->reg)
			continue;
		if (st->dom & POLL) {
			if (op->kind == OP_R52 || op->kind == OP_R53)
				s++;
			continue;
		}
		if (op->kind == OP_W52 && (op->val & st->mask) == st->want)
			s++;
	}
	if (s < count) {
		fprintf(stderr, "%s: step %d (%s, %s %#x) not found in order\n",
		    name, s, steps[s].what,
		    (steps[s].dom & ~POLL) == LOC ? "local" : "MAC",
		    steps[s].reg);
		sFailures++;
	} else
		printf("  %s: all %d steps in order\n", name, count);
}

/* No CMD53 while the model's MAC is off. */
static void
check_cmd53_only_with_mac(const char *name, int from)
{
	int i, bad = 0;

	for (i = from; i < sOpCount; i++) {
		if ((sOps[i].kind == OP_R53 || sOps[i].kind == OP_W53) &&
		    !sOps[i].mac_on) {
			if (bad++ < 4) {
				fprintf(stderr, "%s: CMD53 at %s %#x with the MAC "
				    "off\n", name, sOps[i].dom == LOC ? "local" :
				    "MAC", sOps[i].reg);
			}
		}
	}
	CHECK(bad == 0);
}


/* #pragma mark - tests */


static void
test_framing(void)
{
	uint8_t free_pages[4];
	uint8_t desc[40], ref[40];
	uint16_t sum = 0;
	int i;

	printf("framing helpers\n");

	CHECK(rtwn_sdio_local_addr(0x086) == 0x00086);
	CHECK(rtwn_sdio_local_addr(0x1018) == 0x00018);
	CHECK(rtwn_sdio_mac_addr(0x0004) == 0x10004);
	CHECK(rtwn_sdio_mac_addr(0x0948) == 0x10948);
	CHECK(rtwn_sdio_mac_addr(0xfe08) == 0x1fe08);
	CHECK(rtwn_sdio_rx_addr(0) == 0xe000);
	CHECK(rtwn_sdio_rx_addr(5) == 0xe001);

	/* 40-byte descriptor and a 100-byte frame: 35 words */
	CHECK(rtwn_sdio_tx_addr(RTWN_SDIO_QUEUE_HIGH, 140) == (0x8000 | 35));
	CHECK(rtwn_sdio_tx_addr(RTWN_SDIO_QUEUE_NORMAL, 141) == (0xa000 | 36));
	CHECK(rtwn_sdio_tx_addr(RTWN_SDIO_QUEUE_LOW, 1540) == (0xc000 | 385));

	CHECK(rtwn_sdio_xfer_len(1, 512) == 4);
	CHECK(rtwn_sdio_xfer_len(140, 512) == 140);
	CHECK(rtwn_sdio_xfer_len(141, 512) == 144);
	CHECK(rtwn_sdio_xfer_len(512, 512) == 512);
	CHECK(rtwn_sdio_xfer_len(513, 512) == 1024);
	CHECK(rtwn_sdio_xfer_len(1540, 512) == 1536 + 512);
	CHECK(rtwn_sdio_xfer_len(1540, 256) == 1792);

	CHECK(rtwn_sdio_tx_pages(1) == 1);
	CHECK(rtwn_sdio_tx_pages(128) == 1);
	CHECK(rtwn_sdio_tx_pages(129) == 2);
	CHECK(rtwn_sdio_tx_pages(1540) == 13);

	rtwn_sdio_parse_free_pages(0xe7020210, free_pages);
	CHECK(free_pages[RTWN_SDIO_QUEUE_HIGH] == 0x10);
	CHECK(free_pages[RTWN_SDIO_QUEUE_NORMAL] == 0x02);
	CHECK(free_pages[RTWN_SDIO_QUEUE_LOW] == 0x02);
	CHECK(free_pages[RTWN_SDIO_QUEUE_PUBLIC] == 0xe7);
	CHECK(rtwn_sdio_pages_fit(free_pages, RTWN_SDIO_QUEUE_LOW, 13));
	rtwn_sdio_parse_free_pages(0x04000201, free_pages);
	CHECK(rtwn_sdio_pages_fit(free_pages, RTWN_SDIO_QUEUE_HIGH, 4));
	CHECK(!rtwn_sdio_pages_fit(free_pages, RTWN_SDIO_QUEUE_HIGH, 5));
	CHECK(!rtwn_sdio_pages_fit(free_pages, RTWN_SDIO_QUEUE_LOW, 4));
	rtwn_sdio_take_pages(free_pages, RTWN_SDIO_QUEUE_HIGH, 3);
	CHECK(free_pages[RTWN_SDIO_QUEUE_HIGH] == 0);
	CHECK(free_pages[RTWN_SDIO_QUEUE_PUBLIC] == 2);
	rtwn_sdio_take_pages(free_pages, RTWN_SDIO_QUEUE_HIGH, 9);
	CHECK(free_pages[RTWN_SDIO_QUEUE_PUBLIC] == 0);

	/* checksum: XOR of the first 16 little endian words, field zeroed */
	for (i = 0; i < 40; i++)
		desc[i] = (uint8_t)(i * 37 + 11);
	memcpy(ref, desc, sizeof(ref));
	ref[28] = ref[29] = 0;
	for (i = 0; i < 16; i++)
		sum ^= ref[2 * i] | (ref[2 * i + 1] << 8);
	rtwn_sdio_tx_checksum(desc);
	CHECK(desc[28] == (sum & 0xff) && desc[29] == (sum >> 8));
	CHECK(memcmp(desc + 30, ref + 30, 10) == 0);
	rtwn_sdio_tx_checksum(desc);	/* idempotent */
	CHECK(desc[28] == (sum & 0xff) && desc[29] == (sum >> 8));
	rtwn_sdio_tx_set_agg_num(desc, 1);
	CHECK(desc[31] == 1);
}

static void
put_rx_desc(uint8_t *p, uint32_t pktlen, uint32_t infosz8, uint32_t shift)
{
	uint32_t rxdw0 = (pktlen & 0x3fff) | ((infosz8 & 0xf) << 16) |
	    ((shift & 3) << 24);

	memset(p, 0, RTWN_SDIO_RX_DESC_SIZE);
	p[0] = rxdw0;
	p[1] = rxdw0 >> 8;
	p[2] = rxdw0 >> 16;
	p[3] = rxdw0 >> 24;
}

static void
test_rx_aggregate(void)
{
	uint8_t buf[2048];
	struct rtwn_sdio_rx_pkt pkt;
	uint32_t off, len;

	printf("RX aggregate parsing\n");
	memset(buf, 0xee, sizeof(buf));

	/* 1: 24 + 32 (PHY status) + 100 = 156, next at 160 */
	put_rx_desc(buf, 100, 4, 0);
	/* 2: 24 + 0 + 2 (shift) + 61 = 87, next at 160 + 88 = 248 */
	put_rx_desc(buf + 160, 61, 0, 2);
	/* 3: 24 + 32 + 1500 = 1556, ends at 1804 */
	put_rx_desc(buf + 248, 1500, 4, 0);
	len = 248 + 1556;

	CHECK(rtwn_sdio_rx_next_pkt(buf, len, 0, &pkt) == RTWN_SDIO_RX_PKT_OK);
	CHECK(pkt.off == 0 && pkt.pktlen == 100 && pkt.infosz == 32 &&
	    pkt.shift == 0 && pkt.totlen == 156 && pkt.next == 160);
	off = pkt.next;
	CHECK(rtwn_sdio_rx_next_pkt(buf, len, off, &pkt) == RTWN_SDIO_RX_PKT_OK);
	CHECK(pkt.off == 160 && pkt.pktlen == 61 && pkt.shift == 2 &&
	    pkt.totlen == 87 && pkt.next == 248);
	off = pkt.next;
	CHECK(rtwn_sdio_rx_next_pkt(buf, len, off, &pkt) == RTWN_SDIO_RX_PKT_OK);
	CHECK(pkt.off == 248 && pkt.totlen == 1556);
	off = pkt.next;
	CHECK(off >= len);
	CHECK(rtwn_sdio_rx_next_pkt(buf, len, off, &pkt) ==
	    RTWN_SDIO_RX_PKT_END);

	/* the third claims more than was read */
	CHECK(rtwn_sdio_rx_next_pkt(buf, len - 1, 248, &pkt) ==
	    RTWN_SDIO_RX_PKT_TRUNCATED);
	/* less than a descriptor left */
	CHECK(rtwn_sdio_rx_next_pkt(buf, 170, 160, &pkt) ==
	    RTWN_SDIO_RX_PKT_END);
	/* a zero length ends it */
	put_rx_desc(buf + 160, 0, 0, 0);
	CHECK(rtwn_sdio_rx_next_pkt(buf, len, 160, &pkt) ==
	    RTWN_SDIO_RX_PKT_END);
}

static void
test_power_on_off(void)
{
	struct rtwn_softc *sc;
	int i, error, from, found;

	printf("RTL8723BS power on\n");
	model_reset();
	sc = setup_softc();
	error = r23bs_power_on(sc);
	CHECK(error == 0);
	CHECK(sSoftc.sd_mac_on == 1);
	CHECK(sMacOn == 1);
	check_sequence("power on", kExpectedPowerOn, nitems(kExpectedPowerOn),
	    0);
	check_cmd53_only_with_mac("power on", 0);

	/* the BB antenna register goes in one piece: S1, 0x280 */
	found = 0;
	for (i = 0; i < sOpCount; i++) {
		if (sOps[i].dom == MAC && sOps[i].reg == R23B_ANT_SEL &&
		    sOps[i].kind == OP_W53 && sOps[i].len == 4 &&
		    sOps[i].val == R23B_ANT_SEL_S1)
			found = 1;
		CHECK(!(sOps[i].dom == MAC && sOps[i].reg >= 0x948 &&
		    sOps[i].reg <= 0x94b && sOps[i].kind == OP_W52));
		/* nothing to the USB local register */
		CHECK(!(sOps[i].dom == MAC && sOps[i].reg == 0xfe08));
	}
	CHECK(found);
	printf("  %d transfers, %llu us\n", sOpCount,
	    (unsigned long long)sNow);

	printf("RTL8723BS power off\n");
	from = sOpCount;
	r23bs_power_off(sc);
	CHECK(sSoftc.sd_mac_on == 0);
	CHECK(sMacOn == 0);
	CHECK((sLocal[RTWN_SDIO_HSUS_CTRL] & RTWN_SDIO_HSUS_RESUME_RDY) == 0);
	check_sequence("power off", kExpectedPowerOff,
	    nitems(kExpectedPowerOff), from);
	for (i = from; i < sOpCount; i++)
		CHECK(sOps[i].kind == OP_R52 || sOps[i].kind == OP_W52);

	printf("RTL8723BS power on, antenna at the auxiliary port\n");
	model_reset();
	sc = setup_softc();
	sChip.ant_main = 0;
	sChip.bt_antnum = 0;	/* two antennas */
	CHECK(r23bs_power_on(sc) == 0);
	found = 0;
	for (i = 0; i < sOpCount; i++) {
		if (sOps[i].dom == MAC && sOps[i].reg == R23B_ANT_SEL &&
		    sOps[i].kind == OP_W53 && sOps[i].val == R23B_ANT_SEL_S0)
			found |= 1;
		if (sOps[i].dom == LOC && sOps[i].reg == RTWN_SDIO_BTCOEX_ANT &&
		    sOps[i].kind == OP_W52 && sOps[i].val == 0x05)
			found |= 2;
	}
	CHECK(found == 3);
}

static void
test_timeouts(void)
{
	struct rtwn_softc *sc;
	int i, error;

	printf("timeouts\n");
	sQuiet = 1;

	/* the SDIO interface never resumes */
	model_reset();
	sHsusStuck = 1;
	sc = setup_softc();
	error = r23bs_power_on(sc);
	CHECK(error == ETIMEDOUT);
	CHECK(sNow >= 50000 && sNow <= 60000);
	CHECK(sOpCount < 2000);
	for (i = 0; i < sOpCount; i++)
		CHECK(!(sOps[i].dom == MAC && sOps[i].reg == 0x020));
	CHECK(sSoftc.sd_mac_on == 0);
	printf("  SDIO resume: ETIMEDOUT after %llu us, %d transfers\n",
	    (unsigned long long)sNow, sOpCount);

	/* the MAC never comes on */
	model_reset();
	sOnMacStuck = 1;
	sc = setup_softc();
	error = r23bs_power_on(sc);
	CHECK(error == ETIMEDOUT);
	CHECK(sNow <= 100000);
	CHECK(sSoftc.sd_mac_on == 0);
	printf("  ONMAC: ETIMEDOUT after %llu us, %d transfers\n",
	    (unsigned long long)sNow, sOpCount);

	sQuiet = 0;
}

static void
test_give_up(void)
{
	struct rtwn_softc *sc;
	int i;

	printf("a card that stops answering\n");
	sQuiet = 1;
	model_reset();
	sc = setup_softc();
	sFailAll = 1;
	for (i = 0; i < 100; i++)
		CHECK(rtwn_read_1(sc, 0x100) == 0xff);
	CHECK(sSoftc.sd_gone == 1);
	CHECK(sBusCalls == RTWN_SDIO_MAX_ERRORS);
	CHECK(rtwn_write_4(sc, 0x100, 1) == ENXIO);
	CHECK(r23bs_power_on(sc) == ENXIO);
	CHECK(sBusCalls == RTWN_SDIO_MAX_ERRORS);
	printf("  gave up after %d failed transfers\n", sBusCalls);
	sQuiet = 0;
}

int
main(void)
{
	test_framing();
	test_rx_aggregate();
	test_power_on_off();
	test_timeouts();
	test_give_up();

	if (sFailures != 0) {
		printf("%d check(s) FAILED\n", sFailures);
		return (1);
	}
	printf("all passed\n");
	return (0);
}
