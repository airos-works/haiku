#!/bin/bash
# Host test of i2c_hid's GpioInterrupt (AMD and Intel GPIO controllers) with
# fake ACPI, fake registers and fake interrupts; no Haiku needed.
#
# usage: run.sh [git revision]
#   With a revision, the AMD part also runs against GpioInterrupt.cpp of that
#   revision, and the two must program the controller identically.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../.." && pwd)
acpica=$top/src/add-ons/kernel/bus_managers/acpi/acpica/include
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

build() {	# build <source dir> <output> [-DTEST_NEW]
	# a copy, so that "Driver.h" is the stub, not the driver's
	src=$work/src-$(basename "$2")
	mkdir -p "$src"
	cp "$1/GpioInterrupt.cpp" "$1/GpioInterrupt.h" "$src/"
	[ -f "$1/IntelGpioTables.h" ] && cp "$1/IntelGpioTables.h" "$src/"
	c++ -std=c++17 -g -O0 -Wall -Wno-unused-variable -D__linux__ $3 \
		-I"$src" -I"$here/stub" -I"$acpica" -I"$acpica/platform" \
		-include "$here/stub/SupportDefs.h" -o "$2" \
		"$here/gpio_interrupt_test.cpp" "$src/GpioInterrupt.cpp"
}

build "$here/.." "$work/test" -DTEST_NEW
"$work/test" | tee "$work/new.out"

if [ -n "$1" ]; then
	mkdir "$work/old"
	(cd "$top" && git show "$1:src/add-ons/kernel/drivers/input/i2c_hid/GpioInterrupt.cpp" \
			> "$work/old/GpioInterrupt.cpp" \
		&& git show "$1:src/add-ons/kernel/drivers/input/i2c_hid/GpioInterrupt.h" \
			> "$work/old/GpioInterrupt.h")
	build "$work/old" "$work/old-test"
	"$work/old-test" > "$work/old.out"
	if diff <(grep '^AMD' "$work/old.out") <(grep '^AMD' "$work/new.out") 2>/dev/null; then
		echo "AMD: the same as $1"
	else
		echo "AMD: differs from $1"
		exit 1
	fi
fi
