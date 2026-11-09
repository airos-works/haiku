#!/bin/sh
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
#
# Builds the core of the Neural Kit's speech recognizer on the build host
# and checks it against a reference (NumPy and LiteRT):
#
#	speech_tests.sh [<work directory>]
#
# The work directory is run_tests.sh's (default /tmp/neural-host). It must
# hold a model, speech/models/whisper-tiny.en (made by
# tools/airos/neural/fetch-models.sh <work directory>/speech speech, which
# puts it under neural/models there, or by whisper_convert.py), and
# recordings to try it on, speech/audio/*.wav (16 bit, 16 kHz; say jfk.wav
# from github.com/ggerganov/whisper.cpp/samples). See speech_tests.py for
# what is compared.
#
# The environment may set CXXFLAGS (the build then goes to host-custom/),
# PYTHON and MODEL (another model directory).
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

if [ -z "$MODEL" ]; then
	MODEL=$work/speech/models/whisper-tiny.en
	if [ ! -d "$MODEL" ]; then
		MODEL=$work/speech/neural/models/whisper-tiny.en
	fi
fi
if [ ! -f "$MODEL/speech.conf" ] \
	|| ! ls "$work"/speech/audio/*.wav > /dev/null 2>&1; then
	echo "no model in $MODEL or no recordings in $work/speech/audio:"
	echo "see the top of $0"
	exit 1
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

mkdir -p "$work/speech/tests"
"$PYTHON" -I "$here/speech_tests.py" --host "$out" --model "$MODEL" \
	--work "$work/speech/tests" "$work"/speech/audio/*.wav
