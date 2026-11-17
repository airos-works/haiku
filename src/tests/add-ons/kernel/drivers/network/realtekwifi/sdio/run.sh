#!/bin/bash
# Host test of realtekwifi's SDIO backend: framing helpers, the RTL8723BS
# power sequences against a fake SDIO function, the give-up rule. No Haiku
# needed.
#
# usage: run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../../.." && pwd)
rtwn="$top/src/add-ons/kernel/drivers/network/wlan/realtekwifi"
compat="$top/src/libs/compat/freebsd_network/compat"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The kernel headers the sources name: empty, rtwn_host.h has what they need.
for h in sys/param.h sys/lock.h sys/mutex.h sys/mbuf.h sys/kernel.h \
		sys/socket.h sys/systm.h sys/malloc.h sys/taskqueue.h sys/bus.h \
		sys/endian.h sys/linker.h net/if.h net/ethernet.h net/if_media.h \
		net80211/ieee80211_var.h net80211/ieee80211_radiotap.h; do
	mkdir -p "$work/include/$(dirname "$h")"
	: > "$work/include/$h"
done
mkdir -p "$work/include/dev/sdio"
cp "$compat/dev/sdio/sdio_subr.h" "$work/include/dev/sdio/"

cc -std=gnu11 -g -O0 -Wall -Wno-unused -Wno-unused-parameter \
	-fsanitize=address,undefined \
	-include "$here/rtwn_host.h" -I"$work/include" -I"$rtwn" \
	-o "$work/test" \
	"$here/sdio_backend_test.c" \
	"$rtwn/dev/rtwn/sdio/rtwn_sdio_reg.c" \
	"$rtwn/dev/rtwn/rtl8723b/sdio/r23bs_init.c" \
	"$rtwn/dev/rtwn/rtl8723b/r23b_pwr.c"
"$work/test"
