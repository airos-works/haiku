#!/bin/sh
# Builds and runs the SAE host test against net80211/ieee80211_sae.c and the
# OpenBSD crypto code it uses. Needs a host C compiler only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
wlan=$here/../../../../../libs/compat/openbsd_wlan
out=${TMPDIR:-/tmp}/sae_test.$$
cc=${CC:-cc}

$cc -O2 -Wall -Wno-unused-function -Wno-attributes -Wno-array-parameter \
	-I"$here" -I"$here/shim" -I"$wlan" \
	-o "$out" "$here/sae_test.c" \
	"$wlan/crypto/sha2.c" "$wlan/crypto/hmac.c" "$wlan/crypto/md5.c" \
	"$wlan/crypto/sha1.c"
status=0
"$out" || status=$?
rm -f "$out"
exit $status
