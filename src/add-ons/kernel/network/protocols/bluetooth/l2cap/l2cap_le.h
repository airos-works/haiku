/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef L2CAP_LE_H
#define L2CAP_LE_H

#include <btCoreData.h>
#include <net_buffer.h>


// 0 quiet, 2 link/channel traces, 3 packet dumps (SMP secrets redacted).
// Set with "level N" in ~/config/settings/bluetooth/le_logging; re-read
// whenever an LE fixed channel is opened.
extern int32 gL2capLELogLevel;

#define L2CAP_LE_TRACE(format, args...) \
	do { if (gL2capLELogLevel >= 2) dprintf("l2cap-le: " format, ##args); } \
		while (false)

void l2cap_le_reload_settings();
void l2cap_le_dump(const char* label, HciConnection* connection, uint16 cid,
	net_buffer* buffer);

// Consumes the buffer.
status_t l2cap_le_handle_signaling(HciConnection* connection,
	net_buffer* buffer);
// The ATT bearer of an LE link: its ATT_MTU and the Exchange MTU state
// (Core Vol 3 Part F 3.4.2), kept until the link closes.
struct l2cap_le_att_update {
	uint16	mtu;
		// the link's ATT_MTU when the PDU set it, 0 otherwise
	bool	exchangeDone;
		// this side's Exchange MTU Request got its answer
};

uint16 l2cap_le_att_mtu(HciConnection* connection);
// Sends this side's Exchange MTU Request, offering receiveMTU. B_OK when it
// went out: the answer comes through l2cap_le_answer_att(). B_NOT_ALLOWED
// when this side already asked on the link (a client asks only once).
status_t l2cap_le_att_exchange_mtu(HciConnection* connection,
	uint16 receiveMTU);
// A socket sends its own Exchange MTU Request: note what it offered so the
// answer sets the link's ATT_MTU; the answer still goes to the socket.
void l2cap_le_att_note_request(HciConnection* connection, uint16 receiveMTU);
void l2cap_le_att_forget(HciConnection* connection);

// Returns true when the PDU was a request answered here, or the answer to
// this side's MTU exchange; the caller then frees the buffer. Indications
// are confirmed and still delivered. receiveMTU is what the ATT socket on the
// link takes (0 without one); update says what the PDU changed.
bool l2cap_le_answer_att(HciConnection* connection, net_buffer* buffer,
	uint16 receiveMTU, l2cap_le_att_update& update);

#endif
