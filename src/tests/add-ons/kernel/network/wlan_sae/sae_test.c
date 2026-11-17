/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Host test of net80211's SAE (ieee80211_sae.c): P-256 arithmetic, the
 * IEEE Std 802.11-2020 Annex J.10 vectors (hunting-and-pecking commit, KCK,
 * PMK, PMKID; hash-to-element PT and PWE), the RFC 9380 simplified SWU map
 * vectors, and full exchanges between two instances. Run ./run.sh.
 */

#define IEEE80211_SAE_HOST_TEST
#include "../../../../../libs/compat/openbsd_wlan/net80211/ieee80211_sae.c"

#include <stdio.h>

static int sFailures;

static void
check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		sFailures++;
}

static size_t
unhex(u_int8_t *out, const char *hex)
{
	size_t n = 0;

	while (hex[0] != '\0' && hex[1] != '\0') {
		unsigned int v;
		if (hex[0] == ' ') {
			hex++;
			continue;
		}
		sscanf(hex, "%2x", &v);
		out[n++] = v;
		hex += 2;
	}
	return n;
}

static int
eq_hex(const u_int8_t *data, size_t len, const char *hex)
{
	u_int8_t buf[512];
	size_t n = unhex(buf, hex);

	return n == len && memcmp(buf, data, len) == 0;
}

static void
test_arithmetic(void)
{
	bn256 r, two, t, plain;
	p256_point g, q, s;
	u_int8_t gbytes[64], out[64];
	int i;

	/* R^2 mod p by doubling R mod p 256 times */
	r = p256_one;
	for (i = 0; i < 256; i++)
		fe_add(&r, &r, &r);
	check(bn_eq_mask(&r, &p256_rr) != 0, "R^2 mod p constant");

	/* x * x^-1 = 1 */
	memset(&two, 0, sizeof(two));
	two.v[0] = 12345;
	fe_to_mont(&t, &two);
	fe_inv(&r, &t);
	fe_mul(&r, &r, &t);
	check(bn_eq_mask(&r, &p256_one) != 0, "field inverse");

	/* sqrt(x^2) = +-x */
	fe_sqr(&r, &t);
	fe_sqrt(&r, &r);
	fe_neg(&plain, &r);
	check(bn_eq_mask(&r, &t) || bn_eq_mask(&plain, &t), "field sqrt");

	unhex(gbytes, "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
	    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
	check(pt_from_bytes(&g, gbytes) == 0, "generator is on the curve");

	/* (n - 1) G = -G, n G = infinity, 2G + G = 3G */
	{
		static const bn256 one = { { 1 } }, three = { { 3 } };
		bn256 nm1;
		bn_sub(&nm1, &p256_n, &one);
		pt_mul(&q, &g, &nm1);
		pt_neg(&s, &g);
		pt_to_bytes(out, &q);
		pt_to_bytes(gbytes, &s);
		check(memcmp(out, gbytes, 64) == 0, "(n - 1) G = -G");

		pt_add(&q, &q, &g);
		check(bn_is_zero_mask(&q.z) != 0, "(n - 1) G + G = infinity");

		pt_double(&q, &g);
		pt_add(&q, &q, &g);
		pt_mul(&s, &g, &three);
		pt_to_bytes(out, &q);
		pt_to_bytes(gbytes, &s);
		check(memcmp(out, gbytes, 64) == 0, "2G + G = 3G");
	}

	/* k G for a known k (the RFC 6979 P-256 test key) */
	{
		u_int8_t kb[32];
		bn256 k;
		unhex(kb, "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
		bn_from_bytes(&k, kb);
		unhex(gbytes, "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
		    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
		pt_from_bytes(&g, gbytes);
		pt_mul(&q, &g, &k);
		pt_to_bytes(out, &q);
		check(eq_hex(out, 64,
		    "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
		    "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"),
		    "scalar multiplication (RFC 6979 A.2.5 public key)");
	}
}

static void
test_sswu(void)
{
	/* RFC 9380 J.1.1, P256_XMD:SHA-256_SSWU_RO_, msg "" */
	u_int8_t ub[32], q0[64], q1[64], out[64];
	p256_point a, b, sum;
	bn256 u;

	unhex(q0, "ab640a12220d3ff283510ff3f4b1953d09fad35795140b1c5d64f313967934d5"
	    "dccb558863804a881d4fff3455716c836cef230e5209594ddd33d85c565b19b1");
	unhex(q1, "51cce63c50d972a6e51c61334f0f4875c9ac1cd2d3238412f84e31da7d980ef5"
	    "b45d1a36d00ad90e5ec7840a60a4de411917fbe7c82c3949a6e699e5a1b66aac");

	unhex(ub, "ad5342c66a6dd0ff080df1da0ea1c04b96e0330dd89406465eeba11582515009");
	bn_from_bytes(&u, ub);
	sae_sswu(&a, &u);
	pt_to_bytes(out, &a);
	check(memcmp(out, q0, 64) == 0, "SSWU map, RFC 9380 J.1.1 u[0] -> Q0");

	unhex(ub, "8c0f1d43204bd6f6ea70ae8013070a1518b43873bcd850aafa0a9e220e2eea5a");
	bn_from_bytes(&u, ub);
	sae_sswu(&b, &u);
	pt_to_bytes(out, &b);
	check(memcmp(out, q1, 64) == 0, "SSWU map, RFC 9380 J.1.1 u[1] -> Q1");

	pt_add(&sum, &a, &b);
	pt_to_bytes(out, &sum);
	check(eq_hex(out, 64,
	    "2c15230b26dbc6fc9a37051158c95b79656e17a1a920b11394ca91c44247d3e4"
	    "8a7a74985cc5c776cdfe4b1f19884970453912e9d31528c060be9ab5c43e8415"),
	    "Q0 + Q1 = P, RFC 9380 J.1.1");
}

/* IEEE Std 802.11-2020 Annex J.10 */
static const u_int8_t kAddr1[6] = { 0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87 };
static const u_int8_t kAddr2[6] = { 0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c };
static const char *kPassword = "mekmitasdigoat";

static void
test_annex_j_hnp(void)
{
	struct ieee80211_sae sae;
	u_int8_t rnd[32], mask[32], buf[512], peer[256];
	size_t len, peer_len;

	memset(&sae, 0, sizeof(sae));
	memcpy(sae.own_addr, kAddr1, 6);
	memcpy(sae.peer_addr, kAddr2, 6);
	check(ieee80211_sae_derive_pwe(kAddr1, kAddr2,
	    (const u_int8_t *)kPassword, strlen(kPassword), sae.pwe) == 0,
	    "J.10 hunting-and-pecking PWE derived");

	unhex(rnd, "992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94");
	unhex(mask, "9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322");
	check(ieee80211_sae_commit(&sae, rnd, mask) == 0, "J.10 commit computed");
	len = ieee80211_sae_write_commit(&sae, buf, sizeof(buf));
	check(eq_hex(buf, len,
	    "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c"
	    "65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8d7b9"
	    "5083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1"),
	    "J.10 local commit message");

	peer_len = unhex(peer,
	    "1300591b96f3397fb9451008" "48e7b550543b6720d88337ee93fc49fd6df7e08b"
	    "5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909be0"
	    "3e83ae208f60f8ef5537858074db0668703239986299" "9b511e0a1552a5fea317c2");
	check(ieee80211_sae_parse_commit(&sae, peer, peer_len)
	    == IEEE80211_SAE_STATUS_SUCCESS, "J.10 peer commit parsed");
	check(ieee80211_sae_process_commit(&sae) == 0, "J.10 keys derived");
	check(eq_hex(sae.kck, 32,
	    "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a"),
	    "J.10 KCK");
	check(eq_hex(sae.pmk, 32,
	    "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59"),
	    "J.10 PMK");
	check(eq_hex(sae.pmkid, 16, "8747a600eea3f9f22475df58ca1e5498"),
	    "J.10 PMKID");
}

static void
test_annex_j_h2e(void)
{
	static const u_int8_t a1[6] = { 0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e };
	static const u_int8_t a2[6] = { 0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46 };
	const char *ssid = "byteme", *id = "psk4internet";
	u_int8_t pt[64], pwe[64];

	check(ieee80211_sae_derive_pt((const u_int8_t *)ssid, strlen(ssid),
	    (const u_int8_t *)kPassword, strlen(kPassword),
	    (const u_int8_t *)id, strlen(id), pt) == 0, "J.10 H2E PT derived");
	check(ieee80211_sae_pwe_from_pt(pt, a1, a2, pwe) == 0,
	    "J.10 H2E PWE derived");
	check(eq_hex(pwe, 64,
	    "c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e"
	    "73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0"),
	    "J.10 H2E PWE from PT");
	check(ieee80211_sae_pwe_from_pt(pt, a2, a1, pt + 0) == 0
	    && memcmp(pt, pwe, 64) == 0, "H2E PWE is symmetric in the addresses");
}

/* Two instances run the exchange against each other. */
static void
test_exchange(int h2e, int with_token)
{
	static const u_int8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const u_int8_t ap[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	const char *ssid = "Gaspers-test", *pw = "correct horse battery staple";
	struct ieee80211_sae a, b;
	u_int8_t pt[64], ca[512], cb[512], fa[64], fb[64], tok[64];
	size_t la, lb, lfa, lfb;
	char name[128];

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memcpy(a.own_addr, sta, 6);
	memcpy(a.peer_addr, ap, 6);
	memcpy(b.own_addr, ap, 6);
	memcpy(b.peer_addr, sta, 6);
	a.h2e = b.h2e = h2e;

	if (h2e) {
		ieee80211_sae_derive_pt((const u_int8_t *)ssid, strlen(ssid),
		    (const u_int8_t *)pw, strlen(pw), NULL, 0, pt);
		ieee80211_sae_pwe_from_pt(pt, sta, ap, a.pwe);
		ieee80211_sae_pwe_from_pt(pt, ap, sta, b.pwe);
	} else {
		ieee80211_sae_derive_pwe(sta, ap, (const u_int8_t *)pw,
		    strlen(pw), a.pwe);
		ieee80211_sae_derive_pwe(ap, sta, (const u_int8_t *)pw,
		    strlen(pw), b.pwe);
	}
	snprintf(name, sizeof(name), "%s: both sides derive the same PWE",
	    h2e ? "H2E" : "H&P");
	check(memcmp(a.pwe, b.pwe, 64) == 0, name);

	ieee80211_sae_commit(&a, NULL, NULL);
	ieee80211_sae_commit(&b, NULL, NULL);
	if (with_token) {
		/* the AP answers the first commit with a token request */
		size_t n = 0;
		tok[n++] = 19;
		tok[n++] = 0;
		if (h2e) {
			tok[n++] = 255;
			tok[n++] = 1 + 32;
			tok[n++] = IEEE80211_ELEMID_EXT_ANTI_CLOGGING;
		}
		memset(tok + n, 0x5a, 32);
		n += 32;
		snprintf(name, sizeof(name), "%s: anti-clogging token taken",
		    h2e ? "H2E" : "H&P");
		check(ieee80211_sae_set_token(&a, tok, n) == 0, name);
	}
	la = ieee80211_sae_write_commit(&a, ca, sizeof(ca));
	lb = ieee80211_sae_write_commit(&b, cb, sizeof(cb));
	if (with_token) {
		/* the AP side strips the token before parsing */
		u_int8_t stripped[512];
		size_t n;
		snprintf(name, sizeof(name), "%s: commit carries the token",
		    h2e ? "H2E" : "H&P");
		check(la == 2 + 96 + 32 + (h2e ? 3 : 0), name);
		if (h2e) {
			memcpy(stripped, ca, 2 + 96);
			n = 2 + 96;
		} else {
			memcpy(stripped, ca, 2);
			memcpy(stripped + 2, ca + 2 + 32, 96);
			n = 2 + 96;
		}
		memcpy(ca, stripped, n);
		la = n;
	}

	check(ieee80211_sae_parse_commit(&a, cb, lb) == 0
	    && ieee80211_sae_parse_commit(&b, ca, la) == 0,
	    h2e ? "H2E: commits parsed" : "H&P: commits parsed");
	check(ieee80211_sae_process_commit(&a) == 0
	    && ieee80211_sae_process_commit(&b) == 0,
	    h2e ? "H2E: keys derived" : "H&P: keys derived");
	check(memcmp(a.pmk, b.pmk, 32) == 0 && memcmp(a.kck, b.kck, 32) == 0
	    && memcmp(a.pmkid, b.pmkid, 16) == 0,
	    h2e ? "H2E: same KCK, PMK, PMKID" : "H&P: same KCK, PMK, PMKID");

	lfa = ieee80211_sae_write_confirm(&a, fa, sizeof(fa));
	lfb = ieee80211_sae_write_confirm(&b, fb, sizeof(fb));
	check(ieee80211_sae_check_confirm(&a, fb, lfb) == 0
	    && ieee80211_sae_check_confirm(&b, fa, lfa) == 0,
	    h2e ? "H2E: confirms verify" : "H&P: confirms verify");
	fb[10] ^= 1;
	check(ieee80211_sae_check_confirm(&a, fb, lfb) != 0,
	    h2e ? "H2E: a damaged confirm fails" : "H&P: a damaged confirm fails");

	/* our own commit coming back is a reflection */
	check(ieee80211_sae_parse_commit(&b, cb, lb)
	    == IEEE80211_SAE_SILENTLY_DISCARD, "reflected commit is dropped");
	/* another group */
	cb[0] = 20;
	check(ieee80211_sae_parse_commit(&a, cb, lb)
	    == IEEE80211_SAE_STATUS_UNSUPP_GROUP, "group 20 is refused");
	cb[0] = 19;
	/* an element off the curve */
	cb[2 + 32 + 63] ^= 1;
	check(ieee80211_sae_parse_commit(&a, cb, lb)
	    == IEEE80211_SAE_STATUS_UNSPECIFIED, "element off the curve is refused");
}

static void
test_wrong_password(void)
{
	static const u_int8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const u_int8_t ap[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	struct ieee80211_sae a, b;
	u_int8_t ca[256], cb[256], fa[64], fb[64];
	size_t la, lb;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	ieee80211_sae_derive_pwe(sta, ap, (const u_int8_t *)"password1", 9,
	    a.pwe);
	ieee80211_sae_derive_pwe(ap, sta, (const u_int8_t *)"password2", 9,
	    b.pwe);
	ieee80211_sae_commit(&a, NULL, NULL);
	ieee80211_sae_commit(&b, NULL, NULL);
	la = ieee80211_sae_write_commit(&a, ca, sizeof(ca));
	lb = ieee80211_sae_write_commit(&b, cb, sizeof(cb));
	ieee80211_sae_parse_commit(&a, cb, lb);
	ieee80211_sae_parse_commit(&b, ca, la);
	ieee80211_sae_process_commit(&a);
	ieee80211_sae_process_commit(&b);
	ieee80211_sae_write_confirm(&a, fa, sizeof(fa));
	ieee80211_sae_write_confirm(&b, fb, sizeof(fb));
	check(ieee80211_sae_check_confirm(&a, fb, sizeof(fb)) != 0
	    && memcmp(a.pmk, b.pmk, 32) != 0,
	    "different passwords: confirm fails, PMKs differ");
}

int
main(void)
{
	int i;

	test_arithmetic();
	test_sswu();
	test_annex_j_hnp();
	test_annex_j_h2e();
	for (i = 0; i < 4; i++)
		test_exchange(i & 1, i >> 1);
	test_wrong_password();

	printf("%s: %d failure(s)\n", sFailures ? "FAILED" : "ALL PASSED",
	    sFailures);
	return sFailures != 0;
}
