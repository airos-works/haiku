/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _IF_AICVAR_H_
#define _IF_AICVAR_H_


#define AIC_WORK_COUNT			32
#define AIC_WORK_DATA			336	/* the largest: a connect request */
#define AIC_EVENT_MAX			1024
#define AIC_REORDER_WINDOW		64

/* work for the driver's thread, from net80211's hooks and the receive
   threads */
enum aic_work_type {
	AIC_WORK_SCAN,
	AIC_WORK_SCAN_CANCEL,
	AIC_WORK_CONNECT,
	AIC_WORK_DISCONNECT,
	AIC_WORK_SET_KEY,
	AIC_WORK_DELETE_KEY,
};

struct aic_work {
	enum aic_work_type	type;
	size_t				length;
	uint8_t				data[AIC_WORK_DATA];
};

/* a message from the firmware, for the driver's thread */
struct aic_event {
	struct aic_event*	next;
	uint16_t			id;
	size_t				length;
	uint8_t				data[];
};

/* a key as net80211 handed it over */
struct aic_key {
	int					pairwise;
	int					cipher;
	int					id;
	int					length;
	uint8_t				key[32];
};

/* one TID's A-MPDU reorder window */
struct aic_reorder {
	int					active;
	uint16_t			head;		/* the next sequence number expected */
	int					held;
	int					behind;		/* frames in a row behind the window */
	bigtime_t			since;		/* when the oldest held frame came */
	struct mbuf_list	slots[AIC_REORDER_WINDOW];
	uint8_t				filled[AIC_REORDER_WINDOW];
};

struct aic_softc {
	struct ieee80211com	sc_ic;
	device_t			sc_dev;
	int					(*sc_newstate)(struct ieee80211com*,
							enum ieee80211_state, int);

	int					sc_attached;
	int					sc_5ghz;
	int					sc_vht;			/* advertise VHT (driver setting) */
	int					sc_started;		/* MM_START sent */
	int					sc_vif;			/* -1: no interface */
	int					sc_scanning;
	bigtime_t			sc_scan_started;
	int					sc_scan_results;
	int					sc_scan_results_5ghz;
	bigtime_t			sc_scan_logged;

	/* the association */
	int					sc_connected;
	int					sc_connecting;
	int					sc_ap;			/* the AP's station index */
	int					sc_qos;
	int					sc_key_tasks;
	int					sc_hw_key[6];	/* by key ID; pairwise at 4 */
	struct mbuf*		sc_early_eapol;

	/* the driver's thread */
	thread_id			sc_thread;
	sem_id				sc_wake;
	volatile int		sc_run;
	struct mutex		sc_lock;		/* work ring and event list */
	struct aic_work		sc_work[AIC_WORK_COUNT];
	int					sc_work_head;
	int					sc_work_tail;
	int					sc_work_count;
	struct aic_event*	sc_events;
	struct aic_event**	sc_events_tail;
	int					sc_event_count;
	volatile int		sc_tx_restart;
	volatile int		sc_gone;
	volatile int		sc_back;
	int					sc_timeouts;
	int					sc_recovering;

	/* a frame being sent (aic_start() runs under Giant) */
	uint8_t*			sc_tx_buffer;

	/* frames received on each pipe, delivered at the end of a transfer */
	struct mbuf_list	sc_rxq[2];
	struct mutex		sc_rx_lock;		/* the reorder buffers */
	struct aic_reorder	sc_reorder[8];
	volatile int		sc_reorder_held;

	/* EAPOL frames sent with a confirmation index, and the last index
	   confirmed: the pairwise key waits for message 4 to have left */
	volatile uint32_t	sc_eapol_sent;
	volatile uint32_t	sc_eapol_confirmed;
	sem_id				sc_eapol_wait;

	/* what the firmware said about itself */
	uint32_t			sc_phy_2;
	int8_t				sc_txpower[AIC_TXPWR_ENTRIES];
	int					sc_txpower_enable;

	/* when the link's rate and signal were last asked for */
	bigtime_t			sc_link_time;
	uint32_t			sc_link_rate;

	bigtime_t			sc_stats_time;	/* the debug watch's last line */
};


#endif	/* _IF_AICVAR_H_ */
