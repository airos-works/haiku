#!/bin/bash
# The range arithmetic of an SD card trim (sd_erase_range.h of mmc_disk) on
# the build host; no Haiku and no card needed.
#
# usage: run_host.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

c++ -std=c++17 -g -O0 -Wall -Wextra -fsanitize=address,undefined \
	-I"$top/src/add-ons/kernel/drivers/disk/mmc" -o "$work/test" \
	"$here/sd_erase_range_test.cpp"
"$work/test"
