#!/bin/sh
# Run on Haiku with the Terminal binary to test. Set TERMINAL_RENDERER to
# exercise a particular renderer. Only the window opened here is resized.
set -eu

dir=$(mktemp -d)
"${1:-/boot/system/apps/Terminal}" /bin/sh -c 'tty > "$1/tty"; exec sleep 60' sh "$dir" &
team=$!
trap 'kill "$team" 2>/dev/null || true; rm -rf "$dir"' EXIT
attempt=0
until [ -s "$dir/tty" ]; do
	attempt=$((attempt + 1))
	[ "$attempt" -lt 100 ] || exit 1
	sleep .1
done
tty=$(cat "$dir/tty")
test -c "$tty"

resize() {
	hey "$team" set Frame of Window 0 to "BRect(30,60,$1,$2)" >/dev/null
}

snapshot() {
	hey "$team" do snapshot of View termview of Window 0 \
		with path="$dir/$1.png" >/dev/null
	test -s "$dir/$1.png"
}

resize 640 440
printf '\033[?25l\033[2J\033[Hfirst row\r\nsecond row\r\nthird row' > "$tty"
sleep .2
snapshot before

# Hold rendering across each resize. The scripting snapshot must still show
# the last complete frame until the terminal program releases the update.
check_resize() {
	printf '\033[?2026h' > "$tty"
	sleep .05
	resize "$1" "$2"
	sleep .05
	rm -f "$dir/held.png"
	snapshot held
	cmp "$dir/before.png" "$dir/held.png"
}
check_resize 740 490
check_resize 540 380

printf '\033[?2026l' > "$tty"
sleep .2
snapshot after
if cmp -s "$dir/before.png" "$dir/after.png"; then
	echo 'Terminal did not present the resized frame' >&2
	exit 1
fi
