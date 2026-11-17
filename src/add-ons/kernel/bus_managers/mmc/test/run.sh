#!/bin/bash
# Host test of the MMC bus' SDIO protocol (sdio_card.cpp) against a simulated
# SDIO card; no Haiku needed.
#
# usage: run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

c++ -std=c++17 -g -O0 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-I"$here/stub" -I"$here/.." -I"$top/headers/private/drivers" \
	-include "$here/stub/SupportDefs.h" -o "$work/test" \
	"$here/sdio_card_test.cpp" "$here/../sdio_card.cpp"
"$work/test"
