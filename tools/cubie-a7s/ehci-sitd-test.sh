#!/usr/bin/env bash
# Host test of the EHCI driver's split isochronous arithmetic (siTD masks,
# T-count, encodings, TT budget) and of usb_audio's packet sizes; see
# ehci-sitd-test.cpp.
#
#   ehci-sitd-test.sh
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/ehci-sitd-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -Werror \
	-I"$TOP/src/add-ons/kernel/busses/usb" \
	-I"$TOP/src/add-ons/kernel/drivers/audio/usb" \
	"$HERE/ehci-sitd-test.cpp" -o "$OUT/ehci-sitd-test"
"$OUT/ehci-sitd-test"
