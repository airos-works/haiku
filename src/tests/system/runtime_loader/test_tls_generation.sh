#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?usage: test_tls_generation.sh build-directory}
mkdir -p "$build_dir"
cd "$build_dir"

gcc -O2 -Wall -Wextra -fPIC -shared "$source_dir/tls_generation_module.c" -o liba.so
gcc -O2 -Wall -Wextra -fPIC -shared "$source_dir/tls_generation_module.c" -o libb.so
gcc -O2 -Wall -Wextra "$source_dir/tls_generation_test.c" -o tls_generation_test
./tls_generation_test ./liba.so ./libb.so
