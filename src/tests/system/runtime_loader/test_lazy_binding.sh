#!/bin/sh
# SPDX-License-Identifier: MIT
# Lazy binding of PLT jump slots (LD_BIND_LAZY, LD_BIND_NOW, DF_BIND_NOW).
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?usage: test_lazy_binding.sh build-directory}
mkdir -p "$build_dir"
cd "$build_dir"

flags="-O2 -Wall -Wextra"
gcc $flags -fPIC -shared "$source_dir/lazy_binding_module.c" \
	-o liblazy_binding_module.so
gcc $flags "$source_dir/lazy_binding_test.c" -o lazy_binding_test \
	-L. -llazy_binding_module
mkdir -p libmissing libmissing_now
gcc $flags -fPIC -shared -DLAZY_MISSING "$source_dir/lazy_binding_module.c" \
	-o libmissing/liblazy_binding_module.so
gcc $flags -fPIC -shared -DLAZY_MISSING -Wl,-z,now \
	"$source_dir/lazy_binding_module.c" \
	-o libmissing_now/liblazy_binding_module.so
gcc $flags -DLAZY_MISSING "$source_dir/lazy_binding_test.c" \
	-o lazy_binding_missing_test -Llibmissing -llazy_binding_module \
	-Wl,--allow-shlib-undefined

export LIBRARY_PATH=.:${LIBRARY_PATH:-/boot/system/lib}

LD_BIND_LAZY=1 ./lazy_binding_test
./lazy_binding_test

missing="LIBRARY_PATH=libmissing:$LIBRARY_PATH"
failures=0
expect()
{
	# expect <status> <description> <command...>
	want=$1; what=$2; shift 2
	set +e
	env "$@" > output.txt 2>&1
	status=$?
	set -e
	if [ "$status" = "$want" ]; then
		echo "ok: $what (status $status)"
	else
		echo "FAILED: $what: status $status, expected $want"
		cat output.txt
		failures=$((failures + 1))
	fi
}

expect 0 "lazily bound program with an unresolvable slot starts" \
	"$missing" LD_BIND_LAZY=1 ./lazy_binding_missing_test
expect 127 "and ends when the slot is first called" \
	"$missing" LD_BIND_LAZY=1 ./lazy_binding_missing_test call
grep -q "lazy_missing_function" output.txt \
	|| { echo "FAILED: no message naming the symbol"; failures=$((failures + 1)); }
if env "$missing" ./lazy_binding_missing_test > /dev/null 2>&1; then
	echo "FAILED: eagerly bound, it started"; failures=$((failures + 1))
else
	echo "ok: eagerly bound (default), it does not start"
fi
if env "$missing" LD_BIND_LAZY=1 LD_BIND_NOW=1 ./lazy_binding_missing_test \
		> /dev/null 2>&1; then
	echo "FAILED: LD_BIND_NOW was ignored"; failures=$((failures + 1))
else
	echo "ok: LD_BIND_NOW=1 binds at load"
fi
if env "LIBRARY_PATH=libmissing_now:$LIBRARY_PATH" LD_BIND_LAZY=1 \
		./lazy_binding_missing_test > /dev/null 2>&1; then
	echo "FAILED: DF_BIND_NOW (-z now) was ignored"
	failures=$((failures + 1))
else
	echo "ok: a library linked with -z now binds at load"
fi

[ $failures = 0 ] && echo "PASS" || { echo "FAIL: $failures"; exit 1; }
