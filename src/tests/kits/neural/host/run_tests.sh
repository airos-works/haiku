#!/bin/sh
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
#
# Builds the Neural Kit's CPU interpreter on the build host and checks its
# kernels against LiteRT (TensorFlow Lite) with its reference kernels:
#
#	run_tests.sh [<work directory>]
#
# In the work directory (default /tmp/neural-host) go the build (host/), the
# generated test models (tests/) and, if they are to be compared too, real
# models (models/*.tflite, not made here). It may hold a Python environment
# with numpy and ai-edge-litert (venv/); otherwise python3 must have them.
#
# The environment may set CXXFLAGS (say "-O1 -g -fsanitize=address,undefined"
# to look for memory errors; the build then goes to host-custom/), PYTHON
# and SEEDS (how many different random inputs each test model gets,
# default 3).
#
# Exits with 0 when everything agrees.

set -e

here=$(cd "$(dirname "$0")" && pwd)
work=${1:-/tmp/neural-host}
mkdir -p "$work"
work=$(cd "$work" && pwd)

if [ -z "$PYTHON" ]; then
	if [ -x "$work/venv/bin/python" ]; then
		PYTHON=$work/venv/bin/python
	else
		PYTHON=python3
	fi
fi

out=$work/host
if [ -n "$CXXFLAGS" ]; then
	out=$work/host-custom
	export CXXFLAGS
fi

echo "== build ($out)"
log=$work/build.log
if ! make -C "$here" OUT="$out" > "$log" 2>&1; then
	cat "$log"
	echo "FAILED: build"
	exit 1
fi
if grep -q "warning:" "$log"; then
	grep -A6 "warning:" "$log"
	echo "FAILED: the build has warnings"
	exit 1
fi

failed=0

echo "== test models"
rm -rf "$work/tests"
"$PYTHON" -I "$here/make_models.py" "$work/tests"
"$PYTHON" -I "$here/compare.py" --host "$out/neural_host" --all --quiet \
	--seeds "${SEEDS:-3}" "$work"/tests/*.tflite || failed=1

echo "== damaged models"
"$PYTHON" -I "$here/damage_models.py" --host "$out/neural_host" "$work/tests" \
	"$work/damaged" || failed=1

if ls "$work"/models/*.tflite > /dev/null 2>&1; then
	echo "== real models"
	"$PYTHON" -I "$here/compare.py" --host "$out/neural_host" --all --quiet \
		"$work"/models/*.tflite || failed=1
fi

if [ $failed -ne 0 ]; then
	echo "FAILED"
	exit 1
fi
echo "all tests passed"
