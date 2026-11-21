#!/bin/bash
# Host test of bosch_accel's HingeTracker (what the two accelerometers of a
# 2-in-1 make of its hinge); no Haiku needed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

c++ -std=c++17 -g -O1 -Wall -Wextra -fsanitize=undefined \
	-I"$here/stub" -I"$top/headers/private/drivers" -I"$here/.." \
	-o "$work/hinge_test" "$here/hinge_test.cpp" "$here/../HingeTracker.cpp" -lm
"$work/hinge_test"
