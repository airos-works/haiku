/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_LEGACY_PAIRING_CLIENT_H_
#define _LE_LEGACY_PAIRING_CLIENT_H_

#include <SupportDefs.h>
#include <stddef.h>

class BMessenger;


namespace Bluetooth {

// Initiates LE legacy Just Works over an already connected SMP fixed-channel
// SOCK_SEQPACKET socket. The addresses must be those used to establish the LE
// connection, in Bluetooth wire order. The returned STK is for this connection
// only; encryption and bonding/key distribution are separate steps. Just Works
// has no man-in-the-middle protection.
status_t LELegacyPairJustWorks(int socket, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, uint8 shortTermKey[16],
	uint8* responderKeyDistribution = NULL,
	uint8* negotiatedKeySize = NULL,
	const BMessenger* progress = NULL,
	char* failureDetail = NULL, size_t failureDetailSize = 0);


struct LELegacyBondKey {
	uint8 longTermKey[16];
	uint8 randomNumber[8];
	uint16 encryptedDiversifier;
	uint8 keySize;
	bool hasLongTermKey;
	uint8 identityResolvingKey[16];
	uint8 identityAddress[6];
	uint8 identityAddressType;
	bool hasIdentity;
	bool secureConnections;
		// an LE Secure Connections LTK (EDIV and Rand are zero)
	bool authenticated;
		// paired with MITM protection (Numeric Comparison or Passkey Entry)
};


// Call only after the controller reports that the link is encrypted with the
// STK. expectedDistribution is the responder's accepted key distribution from
// PairJustWorks. No key is retained in output when validation fails.
status_t LELegacyReceiveResponderKeys(int socket, uint8 expectedDistribution,
	uint8 negotiatedKeySize, LELegacyBondKey& output,
	char* failureDetail = NULL, size_t failureDetailSize = 0);

} // namespace Bluetooth

#endif
