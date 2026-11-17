/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * SAE authentication for a station (IEEE Std 802.11-2020 12.4.8), in the
 * kernel next to net80211's own four-way handshake: net80211 sends the
 * Commit and Confirm messages as Authentication frames with algorithm 3,
 * derives the PMK, and the four-way handshake then runs with AKM 8 as it does
 * with PSK. ieee80211_sae.c does the cryptography.
 *
 * Only what a station needs: group 19, hunting-and-pecking or hash-to-element
 * (whichever the access point's RSN Extension element asks for), anti-
 * clogging tokens, no password identifiers, no SAE-PK.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/endian.h>
#include <sys/errno.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_sae.h>

struct mbuf *ieee80211_getmgmt(int, int, u_int);
void ieee80211_try_another_bss(struct ieee80211com *);

#define SAE_MAX_TOKENS	3
#define SAE_MAX_COMMITS	3


int
ieee80211_sae_set_password(struct ieee80211com *ic, const u_int8_t *ssid,
    size_t ssid_len, const u_int8_t *password, size_t password_len)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;

	if (ssid_len > sizeof(st->ssid)
	    || password_len > IEEE80211_SAE_PASSWORD_MAXLEN)
		return EINVAL;

	if (password_len == 0) {
		if (st != NULL) {
			explicit_bzero(st, sizeof(*st));
			free(st, M_DEVBUF, sizeof(*st));
			ic->ic_sae = NULL;
		}
		return 0;
	}

	if (st == NULL) {
		st = malloc(sizeof(*st), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (st == NULL)
			return ENOMEM;
		ic->ic_sae = st;
	}

	if (st->ssid_len == ssid_len && memcmp(st->ssid, ssid, ssid_len) == 0
	    && st->password_len == password_len
	    && timingsafe_bcmp(st->password, password, password_len) == 0)
		return 0;

	explicit_bzero(st, sizeof(*st));
	memcpy(st->ssid, ssid, ssid_len);
	st->ssid_len = ssid_len;
	memcpy(st->password, password, password_len);
	st->password_len = password_len;
	/* The hash-to-element token depends only on the SSID and password. */
	st->pt_valid = ieee80211_sae_derive_pt(ssid, ssid_len, password,
	    password_len, NULL, 0, st->pt) == 0;
	return 0;
}

void
ieee80211_sae_detach(struct ieee80211com *ic)
{
	if (ic->ic_sae != NULL) {
		explicit_bzero(ic->ic_sae, sizeof(*ic->ic_sae));
		free(ic->ic_sae, M_DEVBUF, sizeof(*ic->ic_sae));
		ic->ic_sae = NULL;
	}
}

/*
 * Can we authenticate with this access point using SAE? It must offer the
 * AKM and management frame protection, we must support MFP, and the
 * password must be for its SSID.
 */
int
ieee80211_sae_usable(struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	const struct ieee80211_sae_sta *st = ic->ic_sae;

	if (st == NULL || ic->ic_opmode != IEEE80211_M_STA)
		return 0;
	if (!(ic->ic_caps & IEEE80211_C_MFP))
		return 0;
	if (!(ni->ni_rsnakms & IEEE80211_AKM_SAE)
	    || !(ni->ni_rsncaps & IEEE80211_RSNCAP_MFPC)
	    || ni->ni_rsngroupmgmtcipher != IEEE80211_CIPHER_BIP)
		return 0;
	if (ni->ni_esslen != st->ssid_len
	    || memcmp(ni->ni_essid, st->ssid, st->ssid_len) != 0)
		return 0;
	return 1;
}

/* Did ieee80211_choose_rsnparams() pick SAE for this node? */
int
ieee80211_sae_selected(struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	return ic->ic_sae != NULL && ic->ic_opmode == IEEE80211_M_STA
	    && (ic->ic_flags & IEEE80211_F_RSNON)
	    && ni->ni_rsnakms == IEEE80211_AKM_SAE;
}

/* Hash-to-element when the access point asks for it in its RSNXE. */
int
ieee80211_sae_use_h2e(struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	return ic->ic_sae != NULL && ic->ic_sae->pt_valid
	    && ni->ni_rsnxe_len >= 1
	    && (ni->ni_rsnxe[0] & IEEE80211_RSNXE_SAE_H2E) != 0;
}

static int
sae_start(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;
	struct ieee80211_sae *x = &st->x;
	int error;

	ieee80211_sae_clear(x);
	st->state = IEEE80211_SAE_NOTHING;
	st->tokens = 0;
	st->commits_seen = 0;
	st->resend_with_token = 0;

	IEEE80211_ADDR_COPY(x->own_addr, ic->ic_myaddr);
	IEEE80211_ADDR_COPY(x->peer_addr, ni->ni_macaddr);
	x->h2e = ieee80211_sae_use_h2e(ic, ni);
	if (x->h2e)
		error = ieee80211_sae_pwe_from_pt(st->pt, x->own_addr,
		    x->peer_addr, x->pwe);
	else
		error = ieee80211_sae_derive_pwe(x->own_addr, x->peer_addr,
		    st->password, st->password_len, x->pwe);
	if (error == 0)
		error = ieee80211_sae_commit(x, NULL, NULL);
	if (error != 0) {
		printf("%s: SAE: cannot derive the password element\n",
		    ic->ic_if.if_xname);
		return -1;
	}
	st->state = IEEE80211_SAE_COMMITTED;
	printf("%s: SAE: authenticating with %s (%s)\n", ic->ic_if.if_xname,
	    ether_sprintf(ni->ni_macaddr),
	    x->h2e ? "hash-to-element" : "hunting-and-pecking");
	return 0;
}

/*
 * Builds the body of an SAE Authentication frame: transaction 1 sends our
 * Commit (starting a new exchange unless answering a token request),
 * transaction 2 our Confirm.
 */
struct mbuf *
ieee80211_sae_get_auth(struct ieee80211com *ic, struct ieee80211_node *ni,
    u_int16_t seq)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;
	struct mbuf *m;
	u_int8_t *frm;
	u_int16_t status;
	size_t len;

	if (seq == 1) {
		if (!st->resend_with_token || st->state != IEEE80211_SAE_COMMITTED) {
			if (sae_start(ic, ni) != 0)
				return NULL;
		}
		st->resend_with_token = 0;
		status = st->x.h2e ? IEEE80211_SAE_STATUS_HASH_TO_ELEMENT
		    : IEEE80211_SAE_STATUS_SUCCESS;
	} else {
		if (st->state != IEEE80211_SAE_CONFIRMED)
			return NULL;
		status = IEEE80211_SAE_STATUS_SUCCESS;
	}

	m = ieee80211_getmgmt(M_DONTWAIT, MT_DATA,
	    6 + IEEE80211_SAE_COMMIT_MAXLEN);
	if (m == NULL)
		return NULL;
	frm = mtod(m, u_int8_t *);
	LE_WRITE_2(frm, IEEE80211_AUTH_ALG_SAE); frm += 2;
	LE_WRITE_2(frm, seq); frm += 2;
	LE_WRITE_2(frm, status); frm += 2;
	if (seq == 1)
		len = ieee80211_sae_write_commit(&st->x, frm,
		    IEEE80211_SAE_COMMIT_MAXLEN);
	else
		len = ieee80211_sae_write_confirm(&st->x, frm,
		    IEEE80211_SAE_COMMIT_MAXLEN);
	if (len == 0) {
		m_freem(m);
		return NULL;
	}
	m->m_pkthdr.len = m->m_len = 6 + len;
	return m;
}

static void
sae_fail(struct ieee80211com *ic, struct ieee80211_node *ni, const char *why,
    u_int16_t status)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;

	printf("%s: SAE: authentication with %s failed: %s (status %u)\n",
	    ic->ic_if.if_xname, ether_sprintf(ni->ni_macaddr), why, status);
	st->state = IEEE80211_SAE_NOTHING;
	ieee80211_sae_clear(&st->x);
	ic->ic_stats.is_rx_auth_fail++;
	if (ni != ic->ic_bss)
		ni->ni_fails++;
	else
		ieee80211_try_another_bss(ic);
}

static void
sae_accepted(struct ieee80211com *ic, struct ieee80211_node *ni,
    const struct ieee80211_frame *wh)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;
	struct ieee80211_node *bss = ic->ic_bss;

	st->state = IEEE80211_SAE_ACCEPTED;
	memcpy(st->pmk, st->x.pmk, sizeof(st->pmk));
	memcpy(st->pmkid, st->x.pmkid, sizeof(st->pmkid));
	IEEE80211_ADDR_COPY(st->pmk_bssid, ni->ni_macaddr);
	st->pmk_valid = 1;
	ieee80211_sae_clear(&st->x);

	printf("%s: SAE: authenticated with %s\n", ic->ic_if.if_xname,
	    ether_sprintf(ni->ni_macaddr));

	/* as ieee80211_auth_open() does before associating */
	bss->ni_flags &= ~(IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_RXMGMTPROT
	    | IEEE80211_NODE_TXMGMTPROT);
	bss->ni_port_valid = 0;
	bss->ni_replaycnt_ok = 0;
	(*ic->ic_delete_key)(ic, bss, &bss->ni_pairwise_key);

	ieee80211_new_state(ic, IEEE80211_S_ASSOC,
	    wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK);
}

void
ieee80211_sae_recv_auth(struct ieee80211com *ic, struct ieee80211_node *ni,
    const struct ieee80211_frame *wh, const u_int8_t *frm,
    const u_int8_t *efrm, u_int16_t seq, u_int16_t status)
{
	struct ieee80211_sae_sta *st = ic->ic_sae;
	u_int16_t result;
	size_t len = efrm - frm;

	if (st == NULL || ic->ic_opmode != IEEE80211_M_STA
	    || ic->ic_state != IEEE80211_S_AUTH
	    || !IEEE80211_ADDR_EQ(wh->i_addr2, ic->ic_bss->ni_macaddr)
	    || st->state == IEEE80211_SAE_NOTHING
	    || st->state == IEEE80211_SAE_ACCEPTED) {
		ic->ic_stats.is_rx_bad_auth++;
		return;
	}

	if (seq == 1) {
		if (status == IEEE80211_SAE_STATUS_ANTI_CLOGGING) {
			if (st->state != IEEE80211_SAE_COMMITTED
			    || ++st->tokens > SAE_MAX_TOKENS
			    || ieee80211_sae_set_token(&st->x, frm, len) != 0) {
				sae_fail(ic, ni, "bad anti-clogging token request",
				    status);
				return;
			}
			st->resend_with_token = 1;
			IEEE80211_SEND_MGMT(ic, ic->ic_bss,
			    IEEE80211_FC0_SUBTYPE_AUTH, 1);
			return;
		}
		if (status != (st->x.h2e ? IEEE80211_SAE_STATUS_HASH_TO_ELEMENT
		    : IEEE80211_SAE_STATUS_SUCCESS)) {
			sae_fail(ic, ni, status == IEEE80211_SAE_STATUS_SUCCESS
			    || status == IEEE80211_SAE_STATUS_HASH_TO_ELEMENT
			    ? "password element method mismatch" : "commit refused",
			    status);
			return;
		}
		if (++st->commits_seen > SAE_MAX_COMMITS) {
			sae_fail(ic, ni, "too many commits", status);
			return;
		}

		if (st->state == IEEE80211_SAE_CONFIRMED) {
			/* The access point sent its commit again: it did not
			 * get our confirm. Send the confirm again for the same
			 * commit, drop a different one. */
			if (len >= 2 + IEEE80211_SAE_PRIME_LEN
			    && memcmp(frm + 2, st->x.peer_scalar,
				IEEE80211_SAE_PRIME_LEN) == 0)
				IEEE80211_SEND_MGMT(ic, ic->ic_bss,
				    IEEE80211_FC0_SUBTYPE_AUTH, 2);
			return;
		}

		result = ieee80211_sae_parse_commit(&st->x, frm, len);
		if (result == IEEE80211_SAE_SILENTLY_DISCARD)
			return;
		if (result != IEEE80211_SAE_STATUS_SUCCESS) {
			sae_fail(ic, ni, "bad commit from the access point", result);
			return;
		}
		if (ieee80211_sae_process_commit(&st->x) != 0) {
			sae_fail(ic, ni, "key derivation failed", 0);
			return;
		}
		st->state = IEEE80211_SAE_CONFIRMED;
		IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_AUTH, 2);
		return;
	}

	if (seq == 2) {
		if (st->state != IEEE80211_SAE_CONFIRMED)
			return;
		if (status != IEEE80211_SAE_STATUS_SUCCESS) {
			sae_fail(ic, ni, "confirm refused", status);
			return;
		}
		if (ieee80211_sae_check_confirm(&st->x, frm, len) != 0) {
			/* a wrong password ends here: drop it, as 12.4.8.6.6
			 * says, and let the authentication time out */
			printf("%s: SAE: confirm from %s does not verify "
			    "(wrong password?)\n", ic->ic_if.if_xname,
			    ether_sprintf(ni->ni_macaddr));
			return;
		}
		sae_accepted(ic, ni, wh);
		return;
	}

	ic->ic_stats.is_rx_bad_auth++;
}

/* The PMK for the four-way handshake with this access point. */
const u_int8_t *
ieee80211_sae_pmk(struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	const struct ieee80211_sae_sta *st = ic->ic_sae;

	if (st == NULL || !st->pmk_valid
	    || !IEEE80211_ADDR_EQ(st->pmk_bssid, ni->ni_macaddr))
		return NULL;
	return st->pmk;
}
