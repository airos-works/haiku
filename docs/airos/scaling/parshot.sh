#!/bin/sh
# On the device: parshot.sh <app> <window index> <x> <y> <prefix>
# Puts the window at logical (x, y), then at (x + 1, y + 1), and captures the
# screen in buffer pixels each time (devshot) as <prefix>-a.png and
# <prefix>-b.png; prints the window's logical width and height for
# linepar.py. A move by one logical pixel changes which logical pixels are
# one and which two buffer pixels wide at 150 percent.
app=$1; win=$2; x=$3; y=$4; out=$5
frame=$(hey "$app" get Frame of Window $win | grep -o 'BRect([^)]*)' \
	| tr -d 'BRect()')
l=$(echo $frame | cut -d, -f1 | cut -d. -f1)
t=$(echo $frame | cut -d, -f2 | cut -d. -f1)
r=$(echo $frame | cut -d, -f3 | cut -d. -f1)
b=$(echo $frame | cut -d, -f4 | cut -d. -f1)
w=$((r - l)); h=$((b - t))
for shot in a b; do
	hey "$app" set Frame of Window $win to "BRect($x,$y,$((x + w)),$((y + h)))" \
		> /dev/null
	hey "$app" set Active of Window $win to true > /dev/null
	sleep 1.5
	devshot $out-$shot.png > /dev/null
	x=$((x + 1)); y=$((y + 1))
done
echo $((w + 1)) $((h + 1))
