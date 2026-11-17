/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Simultaneous Authentication of Equals (SAE, IEEE Std 802.11-2020 12.4)
 * for elliptic curve group 19 (NIST P-256), as used by WPA3-Personal.
 *
 * This part knows nothing about frames or nodes: it derives the password
 * element (hunting-and-pecking or hash-to-element), builds and parses the
 * Commit and Confirm message bodies and derives the KCK, PMK and PMKID.
 * ieee80211_proto.c runs the exchange with it. It builds in the kernel and,
 * for the test vectors, on the host.
 */
#ifndef _NET80211_IEEE80211_SAE_H_
#define _NET80211_IEEE80211_SAE_H_

#define IEEE80211_SAE_GROUP		19	/* NIST P-256 */
#define IEEE80211_SAE_PRIME_LEN		32
#define IEEE80211_SAE_ELEMENT_LEN	(2 * IEEE80211_SAE_PRIME_LEN)
#define IEEE80211_SAE_KCK_LEN		32
#define IEEE80211_SAE_PMK_LEN		32
#define IEEE80211_SAE_PMKID_LEN		16
#define IEEE80211_SAE_CONFIRM_LEN	(2 + IEEE80211_SAE_KCK_LEN)
#define IEEE80211_SAE_TOKEN_MAXLEN	256
#define IEEE80211_SAE_PASSWORD_MAXLEN	128
#define IEEE80211_SAE_REJECTED_MAXLEN	32
#define IEEE80211_SAE_COMMIT_MAXLEN	(2 + 3 * IEEE80211_SAE_PRIME_LEN \
	+ 3 + IEEE80211_SAE_TOKEN_MAXLEN)

/* Authentication algorithm number and status codes (Table 9-49, 9-50) */
#define IEEE80211_AUTH_ALG_SAE			3
#define IEEE80211_SAE_STATUS_SUCCESS		0
#define IEEE80211_SAE_STATUS_UNSPECIFIED	1
#define IEEE80211_SAE_STATUS_ANTI_CLOGGING	76
#define IEEE80211_SAE_STATUS_UNSUPP_GROUP	77
#define IEEE80211_SAE_STATUS_UNKNOWN_PWID	123
#define IEEE80211_SAE_STATUS_HASH_TO_ELEMENT	126
#define IEEE80211_SAE_STATUS_PK			127

/* Element ID extensions used in SAE frames */
#define IEEE80211_ELEMID_EXT_PASSWORD_ID	33
#define IEEE80211_ELEMID_EXT_REJECTED_GROUPS	92
#define IEEE80211_ELEMID_EXT_ANTI_CLOGGING	93

/* RSN Extension element (9.4.2.241) */
#define IEEE80211_ELEMID_RSNXE			244
#define IEEE80211_RSNXE_SAE_H2E			0x20

/* ieee80211_sae_parse_commit(): drop the frame without an answer */
#define IEEE80211_SAE_SILENTLY_DISCARD		0xffff

struct ieee80211_sae {
	u_int8_t	own_addr[6];
	u_int8_t	peer_addr[6];
	int		h2e;
	u_int8_t	pwe[IEEE80211_SAE_ELEMENT_LEN];
	u_int8_t	rand[IEEE80211_SAE_PRIME_LEN];
	u_int8_t	own_scalar[IEEE80211_SAE_PRIME_LEN];
	u_int8_t	own_element[IEEE80211_SAE_ELEMENT_LEN];
	u_int8_t	peer_scalar[IEEE80211_SAE_PRIME_LEN];
	u_int8_t	peer_element[IEEE80211_SAE_ELEMENT_LEN];
	u_int8_t	kck[IEEE80211_SAE_KCK_LEN];
	u_int8_t	pmk[IEEE80211_SAE_PMK_LEN];
	u_int8_t	pmkid[IEEE80211_SAE_PMKID_LEN];
	u_int16_t	send_confirm;
	u_int16_t	token_len;
	u_int8_t	token[IEEE80211_SAE_TOKEN_MAXLEN];
	u_int16_t	peer_rejected_len;
	u_int8_t	peer_rejected[IEEE80211_SAE_REJECTED_MAXLEN];
	int		have_peer_commit;
};

/* Password element by hunting-and-pecking (12.4.4.2.2). */
int	ieee80211_sae_derive_pwe(const u_int8_t *, const u_int8_t *,
	    const u_int8_t *, size_t, u_int8_t *);
/* Password token PT for hash-to-element (12.4.4.2.3); id may be NULL. */
int	ieee80211_sae_derive_pt(const u_int8_t *, size_t, const u_int8_t *,
	    size_t, const u_int8_t *, size_t, u_int8_t *);
/* Password element from PT and the two MAC addresses. */
int	ieee80211_sae_pwe_from_pt(const u_int8_t *, const u_int8_t *,
	    const u_int8_t *, u_int8_t *);

/*
 * Start an exchange: own_addr, peer_addr, h2e and pwe must be set. rand and
 * mask are NULL except for test vectors.
 */
int	ieee80211_sae_commit(struct ieee80211_sae *, const u_int8_t *,
	    const u_int8_t *);
size_t	ieee80211_sae_write_commit(const struct ieee80211_sae *, u_int8_t *,
	    size_t);
/* Returns a status code, or IEEE80211_SAE_SILENTLY_DISCARD. */
u_int16_t ieee80211_sae_parse_commit(struct ieee80211_sae *, const u_int8_t *,
	    size_t);
/* Derives k, the KCK, PMK and PMKID once the peer's commit is parsed. */
int	ieee80211_sae_process_commit(struct ieee80211_sae *);
size_t	ieee80211_sae_write_confirm(struct ieee80211_sae *, u_int8_t *, size_t);
int	ieee80211_sae_check_confirm(const struct ieee80211_sae *,
	    const u_int8_t *, size_t);
/* Stores the token of an Anti-Clogging Token request (status 76). */
int	ieee80211_sae_set_token(struct ieee80211_sae *, const u_int8_t *,
	    size_t);
void	ieee80211_sae_clear(struct ieee80211_sae *);

#if defined(_KERNEL) && !defined(IEEE80211_SAE_HOST_TEST)
/*
 * Station side, in ieee80211_sae_sta.c. The password comes from user land
 * (Haiku's IEEE80211_IOC_HAIKU_SAE_PASSWORD) for one SSID; joining that SSID
 * uses SAE (AKM 00-0F-AC:8, with management frame protection required)
 * whenever the access point offers it, and PSK otherwise.
 */
enum ieee80211_sae_state {
	IEEE80211_SAE_NOTHING,
	IEEE80211_SAE_COMMITTED,
	IEEE80211_SAE_CONFIRMED,
	IEEE80211_SAE_ACCEPTED
};

struct ieee80211_sae_sta {
	u_int8_t	ssid[32];
	u_int		ssid_len;
	u_int8_t	password[IEEE80211_SAE_PASSWORD_MAXLEN];
	u_int		password_len;
	u_int8_t	pt[IEEE80211_SAE_ELEMENT_LEN];	/* H2E password token */
	int		pt_valid;

	struct ieee80211_sae x;		/* the exchange in progress */
	enum ieee80211_sae_state state;
	int		resend_with_token;
	int		tokens;		/* token requests this exchange */
	int		commits_seen;	/* peer commits this exchange */

	/* the PMK of the last completed exchange, for the 4-way handshake */
	u_int8_t	pmk_bssid[6];
	int		pmk_valid;
	u_int8_t	pmk[IEEE80211_SAE_PMK_LEN];
	u_int8_t	pmkid[IEEE80211_SAE_PMKID_LEN];
};

struct ieee80211com;
struct ieee80211_node;
struct ieee80211_frame;
struct mbuf;

int	ieee80211_sae_set_password(struct ieee80211com *, const u_int8_t *,
	    size_t, const u_int8_t *, size_t);
void	ieee80211_sae_detach(struct ieee80211com *);
int	ieee80211_sae_usable(struct ieee80211com *, const struct ieee80211_node *);
int	ieee80211_sae_selected(struct ieee80211com *, const struct ieee80211_node *);
int	ieee80211_sae_use_h2e(struct ieee80211com *, const struct ieee80211_node *);
struct mbuf *ieee80211_sae_get_auth(struct ieee80211com *,
	    struct ieee80211_node *, u_int16_t);
void	ieee80211_sae_recv_auth(struct ieee80211com *, struct ieee80211_node *,
	    const struct ieee80211_frame *, const u_int8_t *, const u_int8_t *,
	    u_int16_t, u_int16_t);
const u_int8_t *ieee80211_sae_pmk(struct ieee80211com *,
	    const struct ieee80211_node *);
#endif

#endif /* _NET80211_IEEE80211_SAE_H_ */
