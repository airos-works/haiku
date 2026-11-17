/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_SMP_INITIATOR_H_
#define _LE_SMP_INITIATOR_H_

#include <LESecureConnectionsCrypto.h>
#include <SupportDefs.h>
#include <stddef.h>

class BMessenger;


namespace Bluetooth {

// SMP Pairing Failed reasons (Core Vol 3 Part H 3.5.5).
enum {
	LE_SMP_REASON_PASSKEY_ENTRY_FAILED		= 0x01,
	LE_SMP_REASON_OOB_NOT_AVAILABLE			= 0x02,
	LE_SMP_REASON_AUTHENTICATION_REQUIREMENTS = 0x03,
	LE_SMP_REASON_CONFIRM_VALUE_FAILED		= 0x04,
	LE_SMP_REASON_PAIRING_NOT_SUPPORTED		= 0x05,
	LE_SMP_REASON_ENCRYPTION_KEY_SIZE		= 0x06,
	LE_SMP_REASON_COMMAND_NOT_SUPPORTED		= 0x07,
	LE_SMP_REASON_UNSPECIFIED				= 0x08,
	LE_SMP_REASON_REPEATED_ATTEMPTS			= 0x09,
	LE_SMP_REASON_INVALID_PARAMETERS		= 0x0a,
	LE_SMP_REASON_DHKEY_CHECK_FAILED		= 0x0b,
	LE_SMP_REASON_NUMERIC_COMPARISON_FAILED	= 0x0c
};

// SMP key distribution bits.
enum {
	LE_SMP_KEY_ENCRYPTION	= 0x01,
	LE_SMP_KEY_IDENTITY		= 0x02,
	LE_SMP_KEY_SIGNING		= 0x04
};

struct LESMPOptions {
	uint8	ioCapability;
		// LE_SMP_IO_*: what this side can show and take
	bool	mitm;
		// ask for an authenticated (MITM protected) pairing
	bool	secureConnections;
		// offer LE Secure Connections; used when the peer offers it too
	uint8	responderKeys;
		// keys asked of the responder; the encryption key is derived, not
		// sent, with Secure Connections

	LESMPOptions()
		:
		ioCapability(LE_SMP_IO_NO_INPUT_NO_OUTPUT),
		mitm(false),
		secureConnections(true),
		responderKeys(LE_SMP_KEY_ENCRYPTION | LE_SMP_KEY_IDENTITY)
	{
	}
};

// The user's part in a pairing, called on the pairing thread. Without an
// agent, Just Works goes ahead and the other models fail.
class LESMPUserAgent {
public:
	virtual				~LESMPUserAgent() {}

	// Just Works (value 0) or Numeric Comparison (value is the six digit
	// number both devices show): whether the user accepts. The peer gives
	// up at deadline (system_time() base); answer before it.
	virtual	bool		Confirm(LESMPMethod method, uint32 value,
							bigtime_t deadline) = 0;
	// Passkey Entry with this side showing the number: show it until
	// Finished().
	virtual	void		DisplayPasskey(uint32 passkey) = 0;
	// Passkey Entry with this side typing the number. Return false when the
	// user cannot or will not.
	virtual	bool		EnterPasskey(uint32& passkey, bigtime_t deadline)
							{ return false; }
	// Looked at while waiting for the peer; true ends the pairing.
	virtual	bool		Cancelled() { return false; }
	// The pairing is over, successfully or not; prompts can close.
	virtual	void		Finished(status_t status) {}
};

struct LESMPResult {
	bool		secureConnections;
	bool		authenticated;
		// MITM protected: Numeric Comparison or Passkey Entry
	bool		bonding;
		// both sides asked to bond
	LESMPMethod	method;
	uint8		keySize;
	uint8		responderKeys;
		// what the responder distributes once the link is encrypted
		// (without the encryption key bit for Secure Connections)
	uint8		key[16];
		// encrypt the link with this, as LTK with zero EDIV and Rand: the
		// legacy STK, or the Secure Connections LTK (also the bond's key),
		// masked to keySize
	uint8		reason;
		// SMP reason of a failed pairing, 0 when there was none
	bool		remoteReason;
		// the peer sent it (else this side did)
};

// Pairs as the initiator (central) over a connected SMP fixed-channel socket:
// feature exchange, then LE Secure Connections when both sides offer it
// (P-256 public keys, Just Works, Numeric Comparison or Passkey Entry, the
// DHKey checks), else legacy pairing (Just Works or Passkey Entry). The
// addresses are those the link was created with, in wire order.
//
// Returns B_OK with the key to encrypt with, B_CANCELED when the user
// declined, B_PERMISSION_DENIED when the peer refused (result.reason says
// why), B_BAD_DATA when the peer's values did not check out, B_NOT_SUPPORTED
// for a model this side cannot take part in, B_TIMED_OUT, or an I/O error.
// detail gets a sentence for the user on failure.
status_t LESMPPairAsInitiator(int socket, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, const LESMPOptions& options,
	LESMPUserAgent* agent, LESMPResult& result,
	const BMessenger* progress = NULL, char* detail = NULL,
	size_t detailSize = 0);

const char* LESMPMethodName(LESMPMethod method);

} // namespace Bluetooth

#endif
