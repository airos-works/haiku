/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERM_CRYPTO_H
#define TERM_CRYPTO_H


#include <string>

#include <SupportDefs.h>


/*!	kitty's way for programs to send a password to the terminal: each
	Terminal has an X25519 key pair, its public key goes to shells as
	KITTY_PUBLIC_KEY, and a program encrypts for it with a key pair of its
	own (ECDH, SHA-256 of the shared secret, AES-256-GCM).
*/
namespace TermCrypto {

//!	"1:" and the public key in base85; empty without OpenSSL. Make it
//!	before forking, so that the key is this team's.
const char*	PublicKey();

//!	Decrypts a JSON object of "iv", "tag", "pubkey" and "encrypted" made for
//!	PublicKey().
bool		Decrypt(const char* json, std::string& _plaintext);

//!	Whether a time, in nanoseconds since 1970, is at most five minutes
//!	away from now.
bool		IsRecent(int64 timestamp);

//!	Whether \a text is "<nanoseconds since 1970>:" and \a expected, from at
//!	most five minutes ago (or ahead).
bool		CheckTimestamped(const std::string& text,
				const std::string& expected);

//!	Compares in constant time.
bool		Equal(const std::string& a, const std::string& b);

}	// namespace TermCrypto


#endif	// TERM_CRYPTO_H
