#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?usage: test_tls_templates.sh build-directory [rounds]}
rounds=${2:-100}
mkdir -p "$build_dir/modules"
cd "$build_dir"

gcc -std=c11 -O2 -Wall -Wextra -fPIC -shared -DTLS_SIZE=1048576 \
	"$source_dir/tls_template_module.c" -o base.so
gcc -std=c11 -O2 -Wall -Wextra -fPIC -shared \
	"$source_dir/tls_template_module.c" -o small.so
i=0
while [ "$i" -lt 256 ]; do
	name=$(printf 'modules/module%03d.so' "$i")
	cp small.so "$name"
	i=$((i + 1))
done
gcc -std=gnu11 -O2 -Wall -Wextra "$source_dir/tls_template_test.c" -o tls_template_test
i=0
while [ "$i" -lt "$rounds" ]; do
	./tls_template_test
	i=$((i + 1))
done
