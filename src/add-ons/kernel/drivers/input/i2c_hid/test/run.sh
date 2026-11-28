#!/bin/bash
# Host test of i2c_hid's GpioInterrupt (AMD and Intel GPIO controllers) with
# fake ACPI, fake registers and fake interrupts; no Haiku needed.
#
#
# And of what the driver does with a precision touchpad when the system
# resumes (resume_test.cpp): HIDDevice and the shared touchpad handler with a
# fake touchpad on a fake I2C bus.
#
# usage: run.sh [git revision]
#   With a revision, the AMD part also runs against GpioInterrupt.cpp of that
#   revision, and the two must program the controller identically; and the
#   resume test is run on the driver of that revision as well, to show what
#   it catches (its result there does not count).
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
			> "$work/old/GpioInterrupt.h"
		# (a revision that knows Intel's controllers has their tables)
		git show "$1:src/add-ons/kernel/drivers/input/i2c_hid/IntelGpioTables.h" \
			> "$work/old/IntelGpioTables.h" 2>/dev/null \
			|| rm -f "$work/old/IntelGpioTables.h")
	build "$work/old" "$work/old-test"
	"$work/old-test" > "$work/old.out"
	if diff <(grep '^AMD' "$work/old.out") <(grep '^AMD' "$work/new.out") 2>/dev/null; then
		echo "AMD: the same as $1"
	else
		echo "AMD: differs from $1"
		exit 1
	fi
fi


# --- resume

hid=src/add-ons/kernel/drivers/input
kHIDSources="HIDCollection HIDParser HIDReport HIDReportItem HIDWriter
	TouchpadProtocolHandler"

build_resume() {	# build_resume <revision or ""> <output> [-DTEST_NEW]
	src=$work/resume-$(basename "$2")
	mkdir -p "$src/util" "$src/usb"
	for file in i2c_hid/HIDDevice.cpp i2c_hid/HIDDevice.h \
			i2c_hid/I2CHIDProtocol.h i2c_hid/GpioInterrupt.h \
			hid_shared/ProtocolHandler.h hid_shared/HIDDataTypes.h \
			$(for name in $kHIDSources; do
				echo hid_shared/$name.cpp hid_shared/$name.h; done); do
		if [ -n "$1" ]; then
			(cd "$top" && git show "$1:$hid/$file") > "$src/$(basename $file)"
		else
			cp "$top/$hid/$file" "$src/"
		fi
	done
	# the few system headers that are used as they are
	cp "$top/headers/private/kernel/util/Vector.h" "$src/util/"
	cp "$top"/headers/os/drivers/usb/USB_hid*.h "$src/usb/"
	cp "$top/headers/private/input/keyboard_mouse_driver.h" \
		"$top/headers/private/drivers/lendian_bitfield.h" "$src/"

	c++ -std=c++17 -g -O0 -Wall -Wno-unused-variable -Wno-unused-function \
		-Wno-address-of-packed-member -fsanitize=address,undefined \
		-D__linux__ -Dopen=test_open $3 \
		-I"$here/hidstub" -I"$src" -I"$here/stub" \
		-include "$here/hidstub/SupportDefs.h" -o "$2" \
		"$here/resume_test.cpp" "$src/HIDDevice.cpp" \
		$(for name in $kHIDSources; do echo "$src/$name.cpp"; done)
}

# (the parser never frees its report items: not this test's subject)
export ASAN_OPTIONS=detect_leaks=0

build_resume "" "$work/resume" -DTEST_NEW
"$work/resume"

if [ -n "$1" ]; then
	echo "=== the resume test on the driver of $1:"
	# (a driver that knows resets has the hook the test's handler stubs)
	flag=
	(cd "$top" && git show "$1:$hid/hid_shared/ProtocolHandler.h") \
		| grep -q DeviceWasReset && flag=-DTEST_NEW
	build_resume "$1" "$work/resume-old" $flag
	"$work/resume-old" || true
fi
