#!/bin/sh
# SPDX-License-Identifier: MIT
# exit() while another thread opens libraries (summit#119). A child that
# crashes is held by debug_server; for an unattended run, give the binary
# "report" or "kill" in ~/config/settings/system/debug_server/settings
# (executable_actions).
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?usage: test_dlopen_exit.sh build-directory [rounds]}
mkdir -p "$build_dir"
cd "$build_dir"

gcc -O2 -Wall -Wextra "$source_dir/dlopen_exit_test.c" -o dlopen_exit_test
./dlopen_exit_test "${2:-300}"
