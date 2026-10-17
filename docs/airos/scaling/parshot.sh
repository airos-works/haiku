#!/bin/sh
# On the device: parshot.sh <app> <window index> <x> <y> <prefix> [<scale>]
# Puts the window at logical (x, y), then at (x + 1, y + 1), and captures the
# screen in buffer pixels each time (devshot) as <prefix>-a.png and
# <prefix>-b.png; prints the window's logical width and height for
# linepar.py. A move by one logical pixel changes which logical pixels are
# one and which two buffer pixels wide at 150 percent. With the scale (1.75)
# only the part of the screen around the window is captured, much faster,
# and the crop's left and top buffer pixel are printed after the size.
app=$1; win=$2; x=$3; y=$4; out=$5; scale=$6
frame=$(hey "$app" get Frame of Window $win | grep -o 'BRect([^)]*)' \
	| tr -d 'BRect()')
l=$(echo $frame | cut -d, -f1 | cut -d. -f1)
t=$(echo $frame | cut -d, -f2 | cut -d. -f1)
r=$(echo $frame | cut -d, -f3 | cut -d. -f1)
b=$(echo $frame | cut -d, -f4 | cut -d. -f1)
w=$((r - l)); h=$((b - t))
crop=""
if [ -n "$scale" ]; then
	crop=$(echo "$x $y $w $h $scale" | awk '{s = $5;
		x0 = int($1 * s) - 8; y0 = int($2 * s) - 8;
		if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
		print x0, y0, int(($3 + 3) * s) + 16, int(($4 + 3) * s) + 16}')
fi
for shot in a b; do
	hey "$app" set Frame of Window $win to "BRect($x,$y,$((x + w)),$((y + h)))" \
		> /dev/null
	hey "$app" set Active of Window $win to true > /dev/null
	sleep 1.5
	devshot $out-$shot.png $crop > /dev/null
	x=$((x + 1)); y=$((y + 1))
done
if [ -n "$crop" ]; then
	echo $((w + 1)) $((h + 1)) $(echo $crop | cut -d' ' -f1-2)
else
	echo $((w + 1)) $((h + 1))
fi
