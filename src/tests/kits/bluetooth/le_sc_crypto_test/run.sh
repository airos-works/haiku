#!/bin/bash
# Host test of the LE pairing cryptography (LE Secure Connections f4, f5, f6,
# g2, P-256 ECDH and AES-CMAC; legacy c1, s1 and ah) against the Bluetooth
# Core specification's sample data, and of the SMP initiator against a
# simulated responder (smp_initiator_test.cpp). No Haiku needed.
#
# usage: run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../.." && pwd)
kit=$top/src/kits/bluetooth
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
cxx=${CXX:-c++}
flags="-g -O1 -Wall -Wno-multichar -fsanitize=address,undefined -fno-omit-frame-pointer"

$cc $flags -c -I"$top/src/libs/microecc" -o "$work/LEP256.o" "$kit/LEP256.c"
$cc $flags -c -o "$work/aes.o" \
	"$top/src/libs/compat/openbsd_wlan/crypto/aes.c"
crypto="$work/LEP256.o $work/aes.o $kit/LESecureConnectionsCrypto.cpp \
	$kit/LELegacyPairingCrypto.cpp"

$cxx -std=c++17 $flags -I"$top/headers/private/bluetooth" \
	-include stdint.h -Duint8=uint8_t \
	-o "$work/le_sc_crypto_test" "$here/le_sc_crypto_test.cpp" $crypto
"$work/le_sc_crypto_test"

if [ -f "$here/smp_initiator_test.cpp" ]; then
	$cxx -std=c++17 $flags -I"$here/stub" -I"$top/headers/private/bluetooth" \
		-I"$kit" -include "$here/stub/SupportDefs.h" \
		-o "$work/smp_initiator_test" "$here/smp_initiator_test.cpp" \
		"$kit/LESMPInitiator.cpp" "$kit/LESMPChannel.cpp" $crypto -lpthread
	"$work/smp_initiator_test"
fi
