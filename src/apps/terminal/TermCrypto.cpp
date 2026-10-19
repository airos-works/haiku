/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "TermCrypto.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include <Json.h>
#include <Message.h>

#include <SHA256.h>

#ifdef OPENSSL_ENABLED
#	include <openssl/evp.h>
#endif


static const char kBase85[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
static const int64 kMaxClockDifference = 5 * 60 * 1000000000LL;


//!	Python's b85encode without padding, as kitty has it.
static std::string
encode_base85(const uint8* data, size_t length)
{
	std::string encoded;
	for (size_t i = 0; i < length; i += 4) {
		size_t count = std::min((size_t)4, length - i);
		uint32 value = 0;
		for (size_t j = 0; j < 4; j++)
			value = value << 8 | (j < count ? data[i + j] : 0);
		char chunk[5];
		for (int j = 4; j >= 0; j--) {
			chunk[j] = kBase85[value % 85];
			value /= 85;
		}
		encoded.append(chunk, count + 1);
	}
	return encoded;
}


static bool
decode_base85(const std::string& text, std::string& _decoded)
{
	_decoded.clear();
	for (size_t i = 0; i < text.size(); i += 5) {
		size_t count = std::min((size_t)5, text.size() - i);
		if (count == 1)
			return false;
		uint32 value = 0;
		for (size_t j = 0; j < 5; j++) {
			int digit = 84;
			if (j < count) {
				const char* found = strchr(kBase85, text[i + j]);
				if (found == NULL || text[i + j] == '\0')
					return false;
				digit = found - kBase85;
			}
			value = value * 85 + digit;
		}
		for (size_t j = 0; j + 1 < count; j++)
			_decoded += (char)(value >> (24 - 8 * j));
	}
	return true;
}


#ifdef OPENSSL_ENABLED

static pthread_once_t sKeyOnce = PTHREAD_ONCE_INIT;
static EVP_PKEY* sKey = NULL;
static std::string sPublicKey;


static void
make_key()
{
	EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
	if (context == NULL)
		return;
	if (EVP_PKEY_keygen_init(context) > 0
		&& EVP_PKEY_keygen(context, &sKey) > 0) {
		uint8 key[32];
		size_t length = sizeof(key);
		if (EVP_PKEY_get_raw_public_key(sKey, key, &length) > 0)
			sPublicKey = "1:" + encode_base85(key, length);
	}
	EVP_PKEY_CTX_free(context);
}

#endif	// OPENSSL_ENABLED


const char*
TermCrypto::PublicKey()
{
#ifdef OPENSSL_ENABLED
	pthread_once(&sKeyOnce, make_key);
	return sPublicKey.c_str();
#else
	return "";
#endif
}


bool
TermCrypto::Decrypt(const char* json, std::string& _plaintext)
{
#ifdef OPENSSL_ENABLED
	PublicKey();
	if (sKey == NULL)
		return false;

	BMessage fields;
	if (BJson::Parse(json, fields) != B_OK)
		return false;
	std::string iv;
	std::string tag;
	std::string peerKey;
	std::string encrypted;
	if (!decode_base85(fields.GetString("iv", ""), iv)
		|| !decode_base85(fields.GetString("tag", ""), tag)
		|| !decode_base85(fields.GetString("pubkey", ""), peerKey)
		|| !decode_base85(fields.GetString("encrypted", ""), encrypted)
		|| iv.empty() || tag.size() != 16 || peerKey.size() != 32
		|| fields.GetString("enc_proto", "1") != BString("1")) {
		return false;
	}

	// the shared secret, hashed, is the AES key
	EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL,
		(const uint8*)peerKey.data(), peerKey.size());
	if (peer == NULL)
		return false;
	uint8 secret[32];
	size_t secretLength = sizeof(secret);
	EVP_PKEY_CTX* context = EVP_PKEY_CTX_new(sKey, NULL);
	bool derived = context != NULL && EVP_PKEY_derive_init(context) > 0
		&& EVP_PKEY_derive_set_peer(context, peer) > 0
		&& EVP_PKEY_derive(context, secret, &secretLength) > 0
		&& secretLength == sizeof(secret);
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(peer);
	if (!derived)
		return false;
	SHA256 hash;
	hash.Init();
	hash.Update(secret, secretLength);
	uint8 key[32];
	memcpy(key, hash.Digest(), sizeof(key));
	memset(secret, 0, sizeof(secret));

	EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
	if (cipher == NULL)
		return false;
	std::string plaintext(encrypted.size() + 16, '\0');
	int length = 0;
	int finalLength = 0;
	bool decrypted
		= EVP_DecryptInit_ex(cipher, EVP_aes_256_gcm(), NULL, NULL, NULL) > 0
		&& EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_IVLEN, iv.size(),
			NULL) > 0
		&& EVP_DecryptInit_ex(cipher, NULL, NULL, key,
			(const uint8*)iv.data()) > 0
		&& EVP_DecryptUpdate(cipher, (uint8*)&plaintext[0], &length,
			(const uint8*)encrypted.data(), encrypted.size()) > 0
		&& EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_TAG, tag.size(),
			&tag[0]) > 0
		&& EVP_DecryptFinal_ex(cipher, (uint8*)&plaintext[length],
			&finalLength) > 0;
	EVP_CIPHER_CTX_free(cipher);
	memset(key, 0, sizeof(key));
	if (!decrypted)
		return false;
	plaintext.resize(length + finalLength);
	_plaintext.swap(plaintext);
	return true;
#else
	return false;
#endif
}


bool
TermCrypto::IsRecent(int64 timestamp)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	int64 difference = (int64)now.tv_sec * 1000000000LL + now.tv_nsec
		- timestamp;
	return difference <= kMaxClockDifference
		&& difference >= -kMaxClockDifference;
}


bool
TermCrypto::CheckTimestamped(const std::string& text,
	const std::string& expected)
{
	size_t colon = text.find(':');
	if (colon == std::string::npos || colon == 0)
		return false;
	return IsRecent(strtoll(text.substr(0, colon).c_str(), NULL, 10))
		&& Equal(text.substr(colon + 1), expected);
}


bool
TermCrypto::Equal(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
		return false;
	uint8 different = 0;
	for (size_t i = 0; i < a.size(); i++)
		different |= (uint8)(a[i] ^ b[i]);
	return different == 0;
}
