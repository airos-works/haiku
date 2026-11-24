#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?usage: test_tls_many_threads.sh build-directory}
mkdir -p "$build_dir"
cd "$build_dir"

gcc -O2 -Wall -Wextra "$source_dir/tls_many_threads_test.c" -o tls_many_threads_test
./tls_many_threads_test
