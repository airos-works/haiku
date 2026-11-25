#!/bin/sh
# Run on Haiku with the Terminal binary to test. Set TERMINAL_RENDERER to
# exercise a particular renderer. Only the window opened here is resized.
set -eu

dir=$(mktemp -d)
# The shell in the terminal writes down each size the pty is given.
"${1:-/boot/system/apps/Terminal}" /bin/sh -c 'tty > "$1/tty"
	trap "stty size >> \"$1/sizes\"" WINCH
	while :; do sleep 60 & wait $!; done' sh "$dir" &
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
initialSize=$(stty -F "$tty" size)

# Hold rendering across each resize. The scripting snapshot must still show
# the last complete frame until the terminal program releases the update.
check_resize() {
	printf '\033[?2026h' > "$tty"
	sleep .05
	resize "$1" "$2"
	# Let the grid and pty settle while the program is still holding output.
	sleep .2
	if [ "$(stty -F "$tty" size)" = "$initialSize" ]; then
		echo "The pty did not receive the resized grid" >&2
		exit 1
	fi
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

# A live resize: steps closer together than the end of a gesture is detected
# (75 ms). The grid has to follow the steps, and the pty learn the sizes on
# the way, not only the last one once the steps stop.
: > "$dir/sizes"
width=540
while [ "$width" -lt 840 ]; do
	width=$((width + 10))
	resize "$width" 380
done
sleep .3
sizes=$(sort -u "$dir/sizes" | wc -l)
echo "sizes the pty was given during the steps: $(tr '\n' ',' < "$dir/sizes")"
if [ "$sizes" -lt 3 ]; then
	echo 'The grid did not follow a live resize' >&2
	exit 1
fi
