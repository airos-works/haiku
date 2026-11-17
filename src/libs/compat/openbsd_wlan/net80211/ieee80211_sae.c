/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * SAE (IEEE Std 802.11-2020 12.4) for group 19, with its own small NIST
 * P-256 arithmetic: field elements are eight 32-bit limbs in Montgomery
 * form, points are Jacobian, scalar multiplication is a Montgomery ladder of
 * fixed length. Secret-dependent choices use masks rather than branches;
 * the exponents used (p - 2, (p - 1) / 2, (p + 1) / 4) are public.
 */

#ifdef IEEE80211_SAE_HOST_TEST
#include "sae_host_compat.h"
#else
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/libkern.h>

#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>
#endif

#include <net80211/ieee80211_sae.h>

#define NLIMBS	8

typedef struct {
	u_int32_t	v[NLIMBS];
} bn256;

typedef struct {
	bn256	x, y, z;	/* Jacobian, Montgomery form; z == 0: infinity */
} p256_point;

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const bn256 p256_p = { {
	0xffffffff, 0xffffffff, 0xffffffff, 0x00000000,
	0x00000000, 0x00000000, 0x00000001, 0xffffffff
} };
/* group order n */
static const bn256 p256_n = { {
	0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad,
	0xffffffff, 0xffffffff, 0x00000000, 0xffffffff
} };
static const bn256 p256_b = { {
	0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0,
	0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8
} };
/* p - 2, (p - 1) / 2 and (p + 1) / 4: the exponents for inversion, the
 * Legendre symbol and the square root (p = 3 mod 4) */
static const bn256 p256_p_minus_2 = { {
	0xfffffffd, 0xffffffff, 0xffffffff, 0x00000000,
	0x00000000, 0x00000000, 0x00000001, 0xffffffff
} };
static const bn256 p256_p_minus_1_half = { {
	0xffffffff, 0xffffffff, 0x7fffffff, 0x00000000,
	0x00000000, 0x80000000, 0x80000000, 0x7fffffff
} };
static const bn256 p256_p_plus_1_quarter = { {
	0x00000000, 0x00000000, 0x40000000, 0x00000000,
	0x00000000, 0x40000000, 0xc0000000, 0x3fffffff
} };
/* R^2 mod p, R = 2^256 */
static const bn256 p256_rr = { {
	0x00000003, 0x00000000, 0xffffffff, 0xfffffffb,
	0xfffffffe, 0xffffffff, 0xfffffffd, 0x00000004
} };
/* R mod p: one in Montgomery form */
static const bn256 p256_one = { {
	0x00000001, 0x00000000, 0x00000000, 0xffffffff,
	0xffffffff, 0xffffffff, 0xfffffffe, 0x00000000
} };


/* #pragma mark - multi-precision helpers */


static void
bn_from_bytes(bn256 *r, const u_int8_t *b)
{
	int i;

	for (i = 0; i < NLIMBS; i++) {
		const u_int8_t *q = b + 4 * (NLIMBS - 1 - i);
		r->v[i] = (u_int32_t)q[0] << 24 | (u_int32_t)q[1] << 16
		    | (u_int32_t)q[2] << 8 | q[3];
	}
}

static void
bn_to_bytes(u_int8_t *b, const bn256 *a)
{
	int i;

	for (i = 0; i < NLIMBS; i++) {
		u_int8_t *q = b + 4 * (NLIMBS - 1 - i);
		q[0] = a->v[i] >> 24;
		q[1] = a->v[i] >> 16;
		q[2] = a->v[i] >> 8;
		q[3] = a->v[i];
	}
}

static u_int32_t
bn_add(bn256 *r, const bn256 *a, const bn256 *b)
{
	u_int64_t c = 0;
	int i;

	for (i = 0; i < NLIMBS; i++) {
		c += (u_int64_t)a->v[i] + b->v[i];
		r->v[i] = (u_int32_t)c;
		c >>= 32;
	}
	return (u_int32_t)c;
}

static u_int32_t
bn_sub(bn256 *r, const bn256 *a, const bn256 *b)
{
	u_int64_t d;
	u_int32_t borrow = 0;
	int i;

	for (i = 0; i < NLIMBS; i++) {
		d = (u_int64_t)a->v[i] - b->v[i] - borrow;
		r->v[i] = (u_int32_t)d;
		borrow = (u_int32_t)(d >> 32) & 1;
	}
	return borrow;
}

/* r = mask ? a : b, mask is all ones or zero */
static void
bn_select(bn256 *r, const bn256 *a, const bn256 *b, u_int32_t mask)
{
	int i;

	for (i = 0; i < NLIMBS; i++)
		r->v[i] = (a->v[i] & mask) | (b->v[i] & ~mask);
}

static u_int32_t
bn_is_zero_mask(const bn256 *a)
{
	u_int32_t acc = 0;
	int i;

	for (i = 0; i < NLIMBS; i++)
		acc |= a->v[i];
	/* all ones if acc == 0 */
	return (u_int32_t)(((u_int64_t)acc - 1) >> 32);
}

static u_int32_t
bn_eq_mask(const bn256 *a, const bn256 *b)
{
	bn256 d;
	int i;

	for (i = 0; i < NLIMBS; i++)
		d.v[i] = a->v[i] ^ b->v[i];
	return bn_is_zero_mask(&d);
}

/* all ones if a < b */
static u_int32_t
bn_lt_mask(const bn256 *a, const bn256 *b)
{
	bn256 d;

	return 0 - bn_sub(&d, a, b);
}

/* r = a + b mod m, a and b < m */
static void
bn_mod_add(bn256 *r, const bn256 *a, const bn256 *b, const bn256 *m)
{
	bn256 s, d;
	u_int32_t carry, borrow;

	carry = bn_add(&s, a, b);
	borrow = bn_sub(&d, &s, m);
	/* use the difference if the sum overflowed or is >= m */
	bn_select(r, &d, &s, 0 - (carry | (borrow ^ 1)));
}

/* r = a - b mod m, a and b < m */
static void
bn_mod_sub(bn256 *r, const bn256 *a, const bn256 *b, const bn256 *m)
{
	bn256 d, s;
	u_int32_t borrow;

	borrow = bn_sub(&d, a, b);
	bn_add(&s, &d, m);
	bn_select(r, &s, &d, 0 - borrow);
}

/* a mod m for a < 2m (true for any 256-bit a and m = p or n) */
static void
bn_reduce_once(bn256 *r, const bn256 *a, const bn256 *m)
{
	bn256 d;
	u_int32_t borrow;

	borrow = bn_sub(&d, a, m);
	bn_select(r, a, &d, 0 - borrow);
}


/* #pragma mark - field arithmetic mod p (Montgomery form) */


/* r = a * b / R mod p (CIOS; -p^-1 mod 2^32 is 1) */
static void
fe_mul(bn256 *r, const bn256 *a, const bn256 *b)
{
	u_int32_t t[NLIMBS + 2];
	u_int64_t c, s;
	u_int32_t m, borrow;
	bn256 res, d;
	int i, j;

	memset(t, 0, sizeof(t));
	for (i = 0; i < NLIMBS; i++) {
		c = 0;
		for (j = 0; j < NLIMBS; j++) {
			s = (u_int64_t)t[j] + (u_int64_t)a->v[j] * b->v[i] + c;
			t[j] = (u_int32_t)s;
			c = s >> 32;
		}
		s = (u_int64_t)t[NLIMBS] + c;
		t[NLIMBS] = (u_int32_t)s;
		t[NLIMBS + 1] = (u_int32_t)(s >> 32);

		m = t[0];	/* t[0] * (-p^-1 mod 2^32), which is 1 */
		c = ((u_int64_t)t[0] + (u_int64_t)m * p256_p.v[0]) >> 32;
		for (j = 1; j < NLIMBS; j++) {
			s = (u_int64_t)t[j] + (u_int64_t)m * p256_p.v[j] + c;
			t[j - 1] = (u_int32_t)s;
			c = s >> 32;
		}
		s = (u_int64_t)t[NLIMBS] + c;
		t[NLIMBS - 1] = (u_int32_t)s;
		t[NLIMBS] = t[NLIMBS + 1] + (u_int32_t)(s >> 32);
	}

	/* t < 2p: subtract p once if t >= p */
	for (i = 0; i < NLIMBS; i++)
		res.v[i] = t[i];
	borrow = bn_sub(&d, &res, &p256_p);
	bn_select(r, &d, &res, 0 - (t[NLIMBS] | (borrow ^ 1)));
	explicit_bzero(t, sizeof(t));
}

static void
fe_sqr(bn256 *r, const bn256 *a)
{
	fe_mul(r, a, a);
}

static void
fe_add(bn256 *r, const bn256 *a, const bn256 *b)
{
	bn_mod_add(r, a, b, &p256_p);
}

static void
fe_sub(bn256 *r, const bn256 *a, const bn256 *b)
{
	bn_mod_sub(r, a, b, &p256_p);
}

static void
fe_neg(bn256 *r, const bn256 *a)
{
	static const bn256 zero;

	bn_mod_sub(r, &zero, a, &p256_p);
}

/* plain value < p to Montgomery form */
static void
fe_to_mont(bn256 *r, const bn256 *a)
{
	fe_mul(r, a, &p256_rr);
}

static void
fe_from_mont(bn256 *r, const bn256 *a)
{
	static const bn256 one = { { 1 } };

	fe_mul(r, a, &one);
}

/* r = a^e, e public */
static void
fe_pow(bn256 *r, const bn256 *a, const bn256 *e)
{
	bn256 acc = p256_one, base = *a;
	int i;

	for (i = 255; i >= 0; i--) {
		fe_sqr(&acc, &acc);
		if ((e->v[i / 32] >> (i % 32)) & 1)
			fe_mul(&acc, &acc, &base);
	}
	*r = acc;
	explicit_bzero(&base, sizeof(base));
}

/* inverse by Fermat; maps 0 to 0 */
static void
fe_inv(bn256 *r, const bn256 *a)
{
	fe_pow(r, a, &p256_p_minus_2);
}

/* all ones if a is a square mod p (zero counts as one) */
static u_int32_t
fe_is_square_mask(const bn256 *a)
{
	bn256 t;

	fe_pow(&t, a, &p256_p_minus_1_half);
	return bn_eq_mask(&t, &p256_one) | bn_is_zero_mask(&t);
}

static void
fe_sqrt(bn256 *r, const bn256 *a)
{
	fe_pow(r, a, &p256_p_plus_1_quarter);
}

/* least significant bit of a Montgomery form value, in plain form */
static u_int32_t
fe_lsb(const bn256 *a)
{
	bn256 t;

	fe_from_mont(&t, a);
	return t.v[0] & 1;
}

/* y^2 = x^3 - 3x + b */
static void
fe_curve_rhs(bn256 *r, const bn256 *x)
{
	bn256 t, x3, b;

	fe_sqr(&t, x);
	fe_mul(&x3, &t, x);
	fe_add(&t, x, x);
	fe_add(&t, &t, x);
	fe_sub(&x3, &x3, &t);
	fe_to_mont(&b, &p256_b);
	fe_add(r, &x3, &b);
}


/* #pragma mark - points */


static void
pt_set_infinity(p256_point *r)
{
	memset(r, 0, sizeof(*r));
}

static void
pt_select(p256_point *r, const p256_point *a, const p256_point *b,
    u_int32_t mask)
{
	bn_select(&r->x, &a->x, &b->x, mask);
	bn_select(&r->y, &a->y, &b->y, mask);
	bn_select(&r->z, &a->z, &b->z, mask);
}

static void
pt_double(p256_point *r, const p256_point *a)
{
	bn256 delta, gamma, beta, alpha, t1, t2, x3, y3, z3;

	/* dbl-2001-b, a = -3 */
	fe_sqr(&delta, &a->z);
	fe_sqr(&gamma, &a->y);
	fe_mul(&beta, &a->x, &gamma);
	fe_sub(&t1, &a->x, &delta);
	fe_add(&t2, &a->x, &delta);
	fe_mul(&alpha, &t1, &t2);
	fe_add(&t1, &alpha, &alpha);
	fe_add(&alpha, &t1, &alpha);

	fe_sqr(&x3, &alpha);
	fe_add(&t1, &beta, &beta);	/* 2 beta */
	fe_add(&t1, &t1, &t1);		/* 4 beta */
	fe_add(&t2, &t1, &t1);		/* 8 beta */
	fe_sub(&x3, &x3, &t2);

	fe_add(&z3, &a->y, &a->z);
	fe_sqr(&z3, &z3);
	fe_sub(&z3, &z3, &gamma);
	fe_sub(&z3, &z3, &delta);

	fe_sub(&t1, &t1, &x3);		/* 4 beta - x3 */
	fe_mul(&y3, &alpha, &t1);
	fe_sqr(&t2, &gamma);
	fe_add(&t2, &t2, &t2);
	fe_add(&t2, &t2, &t2);
	fe_add(&t2, &t2, &t2);		/* 8 gamma^2 */
	fe_sub(&y3, &y3, &t2);

	r->x = x3;
	r->y = y3;
	r->z = z3;
}

static void
pt_add(p256_point *r, const p256_point *a, const p256_point *b)
{
	bn256 z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;

	if (bn_is_zero_mask(&a->z)) {
		*r = *b;
		return;
	}
	if (bn_is_zero_mask(&b->z)) {
		*r = *a;
		return;
	}

	/* add-2007-bl */
	fe_sqr(&z1z1, &a->z);
	fe_sqr(&z2z2, &b->z);
	fe_mul(&u1, &a->x, &z2z2);
	fe_mul(&u2, &b->x, &z1z1);
	fe_mul(&t, &b->z, &z2z2);
	fe_mul(&s1, &a->y, &t);
	fe_mul(&t, &a->z, &z1z1);
	fe_mul(&s2, &b->y, &t);
	fe_sub(&h, &u2, &u1);
	fe_sub(&rr, &s2, &s1);

	if (bn_is_zero_mask(&h)) {
		if (bn_is_zero_mask(&rr)) {
			pt_double(r, a);
			return;
		}
		pt_set_infinity(r);
		return;
	}

	fe_add(&i, &h, &h);
	fe_sqr(&i, &i);
	fe_mul(&j, &h, &i);
	fe_add(&rr, &rr, &rr);
	fe_mul(&v, &u1, &i);

	fe_sqr(&x3, &rr);
	fe_sub(&x3, &x3, &j);
	fe_sub(&x3, &x3, &v);
	fe_sub(&x3, &x3, &v);

	fe_sub(&t, &v, &x3);
	fe_mul(&y3, &rr, &t);
	fe_mul(&t, &s1, &j);
	fe_add(&t, &t, &t);
	fe_sub(&y3, &y3, &t);

	fe_add(&z3, &a->z, &b->z);
	fe_sqr(&z3, &z3);
	fe_sub(&z3, &z3, &z1z1);
	fe_sub(&z3, &z3, &z2z2);
	fe_mul(&z3, &z3, &h);

	r->x = x3;
	r->y = y3;
	r->z = z3;
}

static void
pt_neg(p256_point *r, const p256_point *a)
{
	*r = *a;
	fe_neg(&r->y, &a->y);
}

/*
 * r = k * a for k < n. The ladder runs over k + n or k + 2n, whichever has
 * bit 256 set, so it always takes 256 steps from a known start.
 */
static void
pt_mul(p256_point *r, const p256_point *a, const bn256 *k)
{
	p256_point r0, r1, t;
	bn256 k1, k2, kk;
	u_int32_t c1, bit, swap, prev = 0;
	int i;

	if (bn_is_zero_mask(&a->z)) {
		pt_set_infinity(r);
		return;
	}

	c1 = bn_add(&k1, k, &p256_n);
	bn_add(&k2, &k1, &p256_n);
	/* k + n overflowed 2^256: use it; otherwise k + 2n does. Either way
	 * bit 256, left out of kk, is set and starts the ladder. */
	bn_select(&kk, &k1, &k2, 0 - c1);

	r0 = *a;
	pt_double(&r1, a);
	for (i = 255; i >= 0; i--) {
		bit = (kk.v[i / 32] >> (i % 32)) & 1;
		swap = 0 - (bit ^ prev);
		pt_select(&t, &r1, &r0, swap);
		pt_select(&r1, &r0, &r1, swap);
		r0 = t;
		prev = bit;
		pt_add(&r1, &r0, &r1);
		pt_double(&r0, &r0);
	}
	swap = 0 - prev;
	pt_select(&t, &r1, &r0, swap);
	*r = t;

	explicit_bzero(&r0, sizeof(r0));
	explicit_bzero(&r1, sizeof(r1));
	explicit_bzero(&kk, sizeof(kk));
	explicit_bzero(&k1, sizeof(k1));
	explicit_bzero(&k2, sizeof(k2));
}

/* affine x || y, 64 bytes; fails for the point at infinity */
static int
pt_to_bytes(u_int8_t *out, const p256_point *a)
{
	bn256 zi, zi2, x, y;

	if (bn_is_zero_mask(&a->z))
		return -1;
	fe_inv(&zi, &a->z);
	fe_sqr(&zi2, &zi);
	fe_mul(&x, &a->x, &zi2);
	fe_mul(&zi2, &zi2, &zi);
	fe_mul(&y, &a->y, &zi2);
	fe_from_mont(&x, &x);
	fe_from_mont(&y, &y);
	bn_to_bytes(out, &x);
	bn_to_bytes(out + IEEE80211_SAE_PRIME_LEN, &y);
	return 0;
}

/* affine x || y to a point; checks the coordinates and the curve equation */
static int
pt_from_bytes(p256_point *r, const u_int8_t *in)
{
	bn256 x, y, lhs, rhs;

	bn_from_bytes(&x, in);
	bn_from_bytes(&y, in + IEEE80211_SAE_PRIME_LEN);
	if (!bn_lt_mask(&x, &p256_p) || !bn_lt_mask(&y, &p256_p))
		return -1;
	fe_to_mont(&r->x, &x);
	fe_to_mont(&r->y, &y);
	r->z = p256_one;

	fe_sqr(&lhs, &r->y);
	fe_curve_rhs(&rhs, &r->x);
	if (!bn_eq_mask(&lhs, &rhs))
		return -1;
	return 0;
}


/* #pragma mark - hashes */


static void
sae_hmac_sha256(const u_int8_t *key, size_t key_len, const u_int8_t *d1,
    size_t l1, const u_int8_t *d2, size_t l2, const u_int8_t *d3, size_t l3,
    u_int8_t *out)
{
	HMAC_SHA256_CTX ctx;

	HMAC_SHA256_Init(&ctx, key, key_len);
	if (l1 != 0)
		HMAC_SHA256_Update(&ctx, d1, l1);
	if (l2 != 0)
		HMAC_SHA256_Update(&ctx, d2, l2);
	if (l3 != 0)
		HMAC_SHA256_Update(&ctx, d3, l3);
	HMAC_SHA256_Final(out, &ctx);
	explicit_bzero(&ctx, sizeof(ctx));
}

/* KDF-Hash-Length with SHA-256 (12.7.1.6.2), the same as ieee80211_kdf() */
static void
sae_kdf(const u_int8_t *key, size_t key_len, const char *label,
    const u_int8_t *context, size_t context_len, u_int8_t *out, size_t len)
{
	HMAC_SHA256_CTX ctx;
	u_int8_t digest[SHA256_DIGEST_LENGTH];
	u_int8_t le[2], length[2];
	u_int16_t i;

	length[0] = (len * 8) & 0xff;
	length[1] = (len * 8) >> 8;
	for (i = 1; len != 0; i++) {
		le[0] = i & 0xff;
		le[1] = i >> 8;
		HMAC_SHA256_Init(&ctx, key, key_len);
		HMAC_SHA256_Update(&ctx, le, 2);
		HMAC_SHA256_Update(&ctx, (const u_int8_t *)label,
		    strlen(label));
		HMAC_SHA256_Update(&ctx, context, context_len);
		HMAC_SHA256_Update(&ctx, length, 2);
		HMAC_SHA256_Final(digest, &ctx);
		if (len < SHA256_DIGEST_LENGTH) {
			memcpy(out, digest, len);
			break;
		}
		memcpy(out, digest, SHA256_DIGEST_LENGTH);
		out += SHA256_DIGEST_LENGTH;
		len -= SHA256_DIGEST_LENGTH;
	}
	explicit_bzero(digest, sizeof(digest));
	explicit_bzero(&ctx, sizeof(ctx));
}

/* HKDF-Expand (RFC 5869) with SHA-256 */
static void
sae_hkdf_expand(const u_int8_t *prk, const char *info, u_int8_t *out,
    size_t len)
{
	HMAC_SHA256_CTX ctx;
	u_int8_t t[SHA256_DIGEST_LENGTH];
	size_t tlen = 0, n;
	u_int8_t i;

	for (i = 1; len != 0; i++) {
		HMAC_SHA256_Init(&ctx, prk, SHA256_DIGEST_LENGTH);
		if (tlen != 0)
			HMAC_SHA256_Update(&ctx, t, tlen);
		HMAC_SHA256_Update(&ctx, (const u_int8_t *)info, strlen(info));
		HMAC_SHA256_Update(&ctx, &i, 1);
		HMAC_SHA256_Final(t, &ctx);
		tlen = SHA256_DIGEST_LENGTH;
		n = len < tlen ? len : tlen;
		memcpy(out, t, n);
		out += n;
		len -= n;
	}
	explicit_bzero(t, sizeof(t));
	explicit_bzero(&ctx, sizeof(ctx));
}

/* MAX(a, b) || MIN(a, b) */
static void
sae_max_min_addr(u_int8_t *out, const u_int8_t *a, const u_int8_t *b)
{
	if (memcmp(a, b, 6) > 0) {
		memcpy(out, a, 6);
		memcpy(out + 6, b, 6);
	} else {
		memcpy(out, b, 6);
		memcpy(out + 6, a, 6);
	}
}

/* random value in [2, n) */
static void
sae_random_scalar(bn256 *r)
{
	static const bn256 two = { { 2 } };
	u_int8_t buf[IEEE80211_SAE_PRIME_LEN];

	for (;;) {
		arc4random_buf(buf, sizeof(buf));
		bn_from_bytes(r, buf);
		if (bn_lt_mask(r, &p256_n) && !bn_lt_mask(r, &two))
			break;
	}
	explicit_bzero(buf, sizeof(buf));
}


/* #pragma mark - password element */


int
ieee80211_sae_derive_pwe(const u_int8_t *addr1, const u_int8_t *addr2,
    const u_int8_t *password, size_t password_len, u_int8_t *pwe)
{
	u_int8_t key[12], seed[SHA256_DIGEST_LENGTH], value[32], prime[32];
	u_int8_t stub[IEEE80211_SAE_PASSWORD_MAXLEN];
	u_int8_t pw[IEEE80211_SAE_PASSWORD_MAXLEN];
	bn256 x_found, x, rhs, y, ny, xm;
	u_int32_t found = 0, seed_odd = 0, in_range, square, take, i;
	u_int8_t counter;
	int error = -1;

	if (password_len == 0 || password_len > IEEE80211_SAE_PASSWORD_MAXLEN)
		return -1;

	sae_max_min_addr(key, addr1, addr2);
	bn_to_bytes(prime, &p256_p);
	arc4random_buf(stub, password_len);
	memset(&x_found, 0, sizeof(x_found));

	/*
	 * At least 40 rounds whatever happens, so that the time taken does not
	 * tell how early the password produced a point. Once found, the rounds
	 * hash a random stub instead of the password.
	 */
	for (counter = 1; counter <= 40 || !found; counter++) {
		if (counter > 200)
			goto out;

		for (i = 0; i < password_len; i++)
			pw[i] = (password[i] & ~found) | (stub[i] & found);
		sae_hmac_sha256(key, sizeof(key), pw, password_len, &counter, 1,
		    NULL, 0, seed);
		sae_kdf(seed, sizeof(seed), "SAE Hunting and Pecking", prime,
		    sizeof(prime), value, sizeof(value));

		bn_from_bytes(&x, value);
		in_range = bn_lt_mask(&x, &p256_p);
		bn_reduce_once(&x, &x, &p256_p);
		fe_to_mont(&xm, &x);
		fe_curve_rhs(&rhs, &xm);
		square = fe_is_square_mask(&rhs);

		take = in_range & square & ~found;
		bn_select(&x_found, &x, &x_found, take);
		seed_odd = (seed[SHA256_DIGEST_LENGTH - 1] & 1 & take)
		    | (seed_odd & ~take);
		found |= take;
	}

	/* y = sqrt(x^3 + ax + b); PWE = (x, y) if LSB(seed) == LSB(y) */
	fe_to_mont(&xm, &x_found);
	fe_curve_rhs(&rhs, &xm);
	fe_sqrt(&y, &rhs);
	fe_neg(&ny, &y);
	take = 0 - ((fe_lsb(&y) ^ seed_odd) & 1);
	bn_select(&y, &ny, &y, take);
	fe_from_mont(&y, &y);
	bn_to_bytes(pwe, &x_found);
	bn_to_bytes(pwe + IEEE80211_SAE_PRIME_LEN, &y);
	error = 0;

out:
	explicit_bzero(pw, sizeof(pw));
	explicit_bzero(seed, sizeof(seed));
	explicit_bzero(value, sizeof(value));
	explicit_bzero(&x_found, sizeof(x_found));
	explicit_bzero(&x, sizeof(x));
	explicit_bzero(&xm, sizeof(xm));
	explicit_bzero(&y, sizeof(y));
	explicit_bzero(&ny, sizeof(ny));
	explicit_bzero(&rhs, sizeof(rhs));
	return error;
}

/* Simplified SWU (RFC 9380 6.6.2) for P-256 with Z = -10, as in 12.4.4.2.3 */
static void
sae_sswu(p256_point *r, const bn256 *u_plain)
{
	static const bn256 ten = { { 10 } }, three = { { 3 } };
	bn256 u, z, a, b, u2, t1, t2, m, t, x1a, x1b, x1, gx1, x2, gx2, v, x,
	    y, ny, one;
	u_int32_t m_zero, is_qr, flip;

	fe_to_mont(&u, u_plain);
	fe_to_mont(&z, &ten);
	fe_neg(&z, &z);
	fe_to_mont(&a, &three);
	fe_neg(&a, &a);
	fe_to_mont(&b, &p256_b);
	one = p256_one;

	/* m = z^2 u^4 + z u^2 */
	fe_sqr(&u2, &u);
	fe_mul(&t1, &z, &u2);
	fe_sqr(&t2, &t1);
	fe_add(&m, &t1, &t2);
	m_zero = bn_is_zero_mask(&m);
	fe_inv(&t, &m);

	/* x1 = m == 0 ? b / (z a) : (-b / a) (1 + t) */
	fe_mul(&t1, &z, &a);
	fe_inv(&t1, &t1);
	fe_mul(&x1a, &b, &t1);
	fe_inv(&t1, &a);
	fe_mul(&t1, &t1, &b);
	fe_neg(&t1, &t1);
	fe_add(&t2, &one, &t);
	fe_mul(&x1b, &t1, &t2);
	bn_select(&x1, &x1a, &x1b, m_zero);

	fe_curve_rhs(&gx1, &x1);
	fe_mul(&t1, &z, &u2);
	fe_mul(&x2, &t1, &x1);
	fe_curve_rhs(&gx2, &x2);

	is_qr = fe_is_square_mask(&gx1);
	bn_select(&v, &gx1, &gx2, is_qr);
	bn_select(&x, &x1, &x2, is_qr);
	fe_sqrt(&y, &v);

	/* the sign of y follows u */
	flip = 0 - ((fe_lsb(&y) ^ u_plain->v[0]) & 1);
	fe_neg(&ny, &y);
	bn_select(&y, &ny, &y, flip);

	r->x = x;
	r->y = y;
	r->z = p256_one;
}

/* a 48-byte big-endian value mod p, in plain (not Montgomery) form */
static void
sae_reduce_384(bn256 *r, const u_int8_t *in)
{
	u_int8_t hi_bytes[32];
	bn256 hi, lo, him;

	memset(hi_bytes, 0, 16);
	memcpy(hi_bytes + 16, in, 16);
	bn_from_bytes(&hi, hi_bytes);
	bn_from_bytes(&lo, in + 16);
	/* hi * 2^256 mod p is the Montgomery form of hi */
	fe_to_mont(&him, &hi);
	bn_reduce_once(&lo, &lo, &p256_p);
	fe_add(r, &him, &lo);
	explicit_bzero(hi_bytes, sizeof(hi_bytes));
}

int
ieee80211_sae_derive_pt(const u_int8_t *ssid, size_t ssid_len,
    const u_int8_t *password, size_t password_len, const u_int8_t *id,
    size_t id_len, u_int8_t *pt)
{
	u_int8_t seed[SHA256_DIGEST_LENGTH], value[48];
	p256_point p1, p2, sum;
	bn256 u;
	int error;

	/* pwd-seed = HKDF-Extract(ssid, password [|| identifier]) */
	sae_hmac_sha256(ssid, ssid_len, password, password_len, id, id_len,
	    NULL, 0, seed);

	sae_hkdf_expand(seed, "SAE Hash to Element u1 P1", value,
	    sizeof(value));
	sae_reduce_384(&u, value);
	sae_sswu(&p1, &u);

	sae_hkdf_expand(seed, "SAE Hash to Element u2 P2", value,
	    sizeof(value));
	sae_reduce_384(&u, value);
	sae_sswu(&p2, &u);

	pt_add(&sum, &p1, &p2);
	error = pt_to_bytes(pt, &sum);

	explicit_bzero(seed, sizeof(seed));
	explicit_bzero(value, sizeof(value));
	explicit_bzero(&u, sizeof(u));
	explicit_bzero(&p1, sizeof(p1));
	explicit_bzero(&p2, sizeof(p2));
	explicit_bzero(&sum, sizeof(sum));
	return error;
}

int
ieee80211_sae_pwe_from_pt(const u_int8_t *pt, const u_int8_t *addr1,
    const u_int8_t *addr2, u_int8_t *pwe)
{
	static const bn256 one = { { 1 } };
	u_int8_t addrs[12], zero[SHA256_DIGEST_LENGTH], hash[SHA256_DIGEST_LENGTH];
	p256_point ptp, r;
	bn256 val, n1;
	int error;

	if (pt_from_bytes(&ptp, pt) != 0)
		return -1;

	/* val = H(0^n, MAX(addr) || MIN(addr)) mod (n - 1) + 1 */
	sae_max_min_addr(addrs, addr1, addr2);
	memset(zero, 0, sizeof(zero));
	sae_hmac_sha256(zero, sizeof(zero), addrs, sizeof(addrs), NULL, 0,
	    NULL, 0, hash);
	bn_from_bytes(&val, hash);
	bn_sub(&n1, &p256_n, &one);
	bn_reduce_once(&val, &val, &n1);
	bn_add(&val, &val, &one);

	pt_mul(&r, &ptp, &val);
	error = pt_to_bytes(pwe, &r);

	explicit_bzero(&ptp, sizeof(ptp));
	explicit_bzero(&r, sizeof(r));
	explicit_bzero(&val, sizeof(val));
	return error;
}


/* #pragma mark - exchange */


int
ieee80211_sae_commit(struct ieee80211_sae *sae, const u_int8_t *rand_in,
    const u_int8_t *mask_in)
{
	static const bn256 two = { { 2 } };
	bn256 rnd, mask, scalar;
	p256_point pwe, elem;
	int error = -1;

	if (pt_from_bytes(&pwe, sae->pwe) != 0)
		return -1;

	for (;;) {
		if (rand_in != NULL) {
			bn_from_bytes(&rnd, rand_in);
			bn_from_bytes(&mask, mask_in);
		} else {
			sae_random_scalar(&rnd);
			sae_random_scalar(&mask);
		}
		/* commit-scalar = (rand + mask) mod r, must be > 1 */
		bn_mod_add(&scalar, &rnd, &mask, &p256_n);
		if (!bn_lt_mask(&scalar, &two) || rand_in != NULL)
			break;
	}
	if (bn_lt_mask(&scalar, &two))
		goto out;

	/* COMMIT-ELEMENT = inverse(scalar-op(mask, PWE)) */
	pt_mul(&elem, &pwe, &mask);
	pt_neg(&elem, &elem);
	if (pt_to_bytes(sae->own_element, &elem) != 0)
		goto out;

	bn_to_bytes(sae->rand, &rnd);
	bn_to_bytes(sae->own_scalar, &scalar);
	sae->send_confirm = 0;
	sae->have_peer_commit = 0;
	error = 0;

out:
	explicit_bzero(&rnd, sizeof(rnd));
	explicit_bzero(&mask, sizeof(mask));
	explicit_bzero(&pwe, sizeof(pwe));
	explicit_bzero(&elem, sizeof(elem));
	return error;
}

size_t
ieee80211_sae_write_commit(const struct ieee80211_sae *sae, u_int8_t *buf,
    size_t len)
{
	size_t need = 2 + IEEE80211_SAE_PRIME_LEN + IEEE80211_SAE_ELEMENT_LEN;
	u_int8_t *p = buf;

	if (sae->token_len != 0)
		need += sae->token_len + (sae->h2e ? 3 : 0);
	if (len < need)
		return 0;

	*p++ = IEEE80211_SAE_GROUP & 0xff;
	*p++ = IEEE80211_SAE_GROUP >> 8;
	if (sae->token_len != 0 && !sae->h2e) {
		memcpy(p, sae->token, sae->token_len);
		p += sae->token_len;
	}
	memcpy(p, sae->own_scalar, IEEE80211_SAE_PRIME_LEN);
	p += IEEE80211_SAE_PRIME_LEN;
	memcpy(p, sae->own_element, IEEE80211_SAE_ELEMENT_LEN);
	p += IEEE80211_SAE_ELEMENT_LEN;
	if (sae->token_len != 0 && sae->h2e) {
		/* Anti-Clogging Token Container element */
		*p++ = 255;
		*p++ = 1 + sae->token_len;
		*p++ = IEEE80211_ELEMID_EXT_ANTI_CLOGGING;
		memcpy(p, sae->token, sae->token_len);
		p += sae->token_len;
	}
	return p - buf;
}

u_int16_t
ieee80211_sae_parse_commit(struct ieee80211_sae *sae, const u_int8_t *buf,
    size_t len)
{
	static const bn256 one = { { 1 } };
	const u_int8_t *p = buf, *end = buf + len;
	p256_point elem;
	bn256 s;

	if (len < 2)
		return IEEE80211_SAE_STATUS_UNSPECIFIED;
	if ((p[0] | p[1] << 8) != IEEE80211_SAE_GROUP)
		return IEEE80211_SAE_STATUS_UNSUPP_GROUP;
	p += 2;

	/* An access point does not send an anti-clogging token. */
	if (end - p < IEEE80211_SAE_PRIME_LEN + IEEE80211_SAE_ELEMENT_LEN)
		return IEEE80211_SAE_STATUS_UNSPECIFIED;

	/* 1 < scalar < r */
	bn_from_bytes(&s, p);
	if (!bn_lt_mask(&s, &p256_n) || bn_lt_mask(&one, &s) == 0)
		return IEEE80211_SAE_STATUS_UNSPECIFIED;

	/* element coordinates < p and on the curve */
	if (pt_from_bytes(&elem, p + IEEE80211_SAE_PRIME_LEN) != 0)
		return IEEE80211_SAE_STATUS_UNSPECIFIED;

	/* our own values coming back: a reflection, drop it silently */
	if (timingsafe_bcmp(p, sae->own_scalar, IEEE80211_SAE_PRIME_LEN) == 0
	    && timingsafe_bcmp(p + IEEE80211_SAE_PRIME_LEN, sae->own_element,
		IEEE80211_SAE_ELEMENT_LEN) == 0)
		return IEEE80211_SAE_SILENTLY_DISCARD;

	memcpy(sae->peer_scalar, p, IEEE80211_SAE_PRIME_LEN);
	memcpy(sae->peer_element, p + IEEE80211_SAE_PRIME_LEN,
	    IEEE80211_SAE_ELEMENT_LEN);
	p += IEEE80211_SAE_PRIME_LEN + IEEE80211_SAE_ELEMENT_LEN;

	/* Optional elements: a Password Identifier we never sent is an error;
	 * Rejected Groups (H2E) go into the key derivation salt. */
	sae->peer_rejected_len = 0;
	while (end - p >= 3 && p[0] == 255 && p[1] >= 1 && end - p >= 2 + p[1]) {
		if (p[2] == IEEE80211_ELEMID_EXT_PASSWORD_ID)
			return IEEE80211_SAE_STATUS_UNKNOWN_PWID;
		if (p[2] == IEEE80211_ELEMID_EXT_REJECTED_GROUPS && sae->h2e) {
			size_t n = p[1] - 1;
			if (n > sizeof(sae->peer_rejected) || (n & 1) != 0)
				return IEEE80211_SAE_STATUS_UNSPECIFIED;
			memcpy(sae->peer_rejected, p + 3, n);
			sae->peer_rejected_len = n;
		}
		p += 2 + p[1];
	}

	sae->have_peer_commit = 1;
	return IEEE80211_SAE_STATUS_SUCCESS;
}

int
ieee80211_sae_process_commit(struct ieee80211_sae *sae)
{
	u_int8_t k[IEEE80211_SAE_PRIME_LEN], keyseed[SHA256_DIGEST_LENGTH];
	u_int8_t zero[SHA256_DIGEST_LENGTH], context[IEEE80211_SAE_PRIME_LEN];
	u_int8_t keys[IEEE80211_SAE_KCK_LEN + IEEE80211_SAE_PMK_LEN];
	u_int8_t kb[IEEE80211_SAE_ELEMENT_LEN];
	p256_point pwe, peer, kp;
	bn256 own, other, rnd, ctx;
	int error = -1;

	if (!sae->have_peer_commit)
		return -1;
	if (pt_from_bytes(&pwe, sae->pwe) != 0
	    || pt_from_bytes(&peer, sae->peer_element) != 0)
		return -1;

	/* K = rand * (peer-commit-scalar * PWE + PEER-COMMIT-ELEMENT) */
	bn_from_bytes(&other, sae->peer_scalar);
	bn_from_bytes(&rnd, sae->rand);
	pt_mul(&kp, &pwe, &other);
	pt_add(&kp, &kp, &peer);
	pt_mul(&kp, &kp, &rnd);
	if (pt_to_bytes(kb, &kp) != 0)
		goto out;	/* K is the identity */
	memcpy(k, kb, sizeof(k));

	/* keyseed = H(salt, k); salt is the rejected groups (H2E) or 0^32 */
	memset(zero, 0, sizeof(zero));
	if (sae->h2e && sae->peer_rejected_len != 0)
		sae_hmac_sha256(sae->peer_rejected, sae->peer_rejected_len, k,
		    sizeof(k), NULL, 0, NULL, 0, keyseed);
	else
		sae_hmac_sha256(zero, sizeof(zero), k, sizeof(k), NULL, 0, NULL,
		    0, keyseed);

	/* context = (commit-scalar + peer-commit-scalar) mod r */
	bn_from_bytes(&own, sae->own_scalar);
	bn_mod_add(&ctx, &own, &other, &p256_n);
	bn_to_bytes(context, &ctx);

	/* KCK || PMK = KDF-512(keyseed, "SAE KCK and PMK", context) */
	sae_kdf(keyseed, sizeof(keyseed), "SAE KCK and PMK", context,
	    sizeof(context), keys, sizeof(keys));
	memcpy(sae->kck, keys, IEEE80211_SAE_KCK_LEN);
	memcpy(sae->pmk, keys + IEEE80211_SAE_KCK_LEN, IEEE80211_SAE_PMK_LEN);
	/* PMKID = L(context, 0, 128) */
	memcpy(sae->pmkid, context, IEEE80211_SAE_PMKID_LEN);
	error = 0;

out:
	explicit_bzero(k, sizeof(k));
	explicit_bzero(kb, sizeof(kb));
	explicit_bzero(keyseed, sizeof(keyseed));
	explicit_bzero(keys, sizeof(keys));
	explicit_bzero(&kp, sizeof(kp));
	explicit_bzero(&rnd, sizeof(rnd));
	explicit_bzero(&pwe, sizeof(pwe));
	return error;
}

/* CN(KCK, send-confirm, scalar1, element1, scalar2, element2) */
static void
sae_cn(const struct ieee80211_sae *sae, const u_int8_t *sc,
    const u_int8_t *scalar1, const u_int8_t *element1,
    const u_int8_t *scalar2, const u_int8_t *element2, u_int8_t *out)
{
	HMAC_SHA256_CTX ctx;

	HMAC_SHA256_Init(&ctx, sae->kck, IEEE80211_SAE_KCK_LEN);
	HMAC_SHA256_Update(&ctx, sc, 2);
	HMAC_SHA256_Update(&ctx, scalar1, IEEE80211_SAE_PRIME_LEN);
	HMAC_SHA256_Update(&ctx, element1, IEEE80211_SAE_ELEMENT_LEN);
	HMAC_SHA256_Update(&ctx, scalar2, IEEE80211_SAE_PRIME_LEN);
	HMAC_SHA256_Update(&ctx, element2, IEEE80211_SAE_ELEMENT_LEN);
	HMAC_SHA256_Final(out, &ctx);
	explicit_bzero(&ctx, sizeof(ctx));
}

size_t
ieee80211_sae_write_confirm(struct ieee80211_sae *sae, u_int8_t *buf,
    size_t len)
{
	if (len < IEEE80211_SAE_CONFIRM_LEN)
		return 0;
	if (sae->send_confirm < 0xffff)
		sae->send_confirm++;
	buf[0] = sae->send_confirm & 0xff;
	buf[1] = sae->send_confirm >> 8;
	sae_cn(sae, buf, sae->own_scalar, sae->own_element, sae->peer_scalar,
	    sae->peer_element, buf + 2);
	return IEEE80211_SAE_CONFIRM_LEN;
}

int
ieee80211_sae_check_confirm(const struct ieee80211_sae *sae,
    const u_int8_t *buf, size_t len)
{
	u_int8_t verifier[SHA256_DIGEST_LENGTH];
	int result;

	if (len < IEEE80211_SAE_CONFIRM_LEN || !sae->have_peer_commit)
		return -1;
	sae_cn(sae, buf, sae->peer_scalar, sae->peer_element, sae->own_scalar,
	    sae->own_element, verifier);
	result = timingsafe_bcmp(verifier, buf + 2, sizeof(verifier)) == 0
	    ? 0 : -1;
	explicit_bzero(verifier, sizeof(verifier));
	return result;
}

int
ieee80211_sae_set_token(struct ieee80211_sae *sae, const u_int8_t *buf,
    size_t len)
{
	const u_int8_t *p = buf + 2;

	/* Finite Cyclic Group, then the token, or for H2E the token in an
	 * Anti-Clogging Token Container element */
	if (len < 2 || (buf[0] | buf[1] << 8) != IEEE80211_SAE_GROUP)
		return -1;
	len -= 2;
	if (sae->h2e) {
		if (len < 3 || p[0] != 255 || p[2] != IEEE80211_ELEMID_EXT_ANTI_CLOGGING
		    || p[1] < 1 || (size_t)p[1] + 2 > len)
			return -1;
		len = p[1] - 1;
		p += 3;
	}
	if (len == 0 || len > sizeof(sae->token))
		return -1;
	memcpy(sae->token, p, len);
	sae->token_len = len;
	return 0;
}

void
ieee80211_sae_clear(struct ieee80211_sae *sae)
{
	explicit_bzero(sae, sizeof(*sae));
}
