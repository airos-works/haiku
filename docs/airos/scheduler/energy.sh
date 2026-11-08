#!/bin/sh
# energy.sh <label>   (runs ON the laptop) socket power for idle, one busy
# thread and a light periodic load, from the amdgpu sensor (whole watts,
# averaged over 500 ms samples); results as "E <label> <test> ..." lines
cd /boot/home
L=$1
avgp() { awk '{for(i=1;i<=NF;i++) if($i ~ /^avg_power/){split($i,a,"=");sub("W","",$(i+1)); }} {for(i=1;i<=NF;i++) if($i=="avg_power"){}}' ; }
measure() { # name, command
	./gpusens 36 500 > /tmp/p.$$ 2>&1 &
	gp=$!
	sleep 1
	out=$(sh -c "$2" | grep '^R' | head -1)
	wait $gp
	p=$(sed -n 's/.*avg_power \([0-9]*\)W.*/\1/p' /tmp/p.$$ | awk '{s+=$1;n++} END {if(n) printf "%.2f", s/n}')
	echo "E $L $1 power=${p}W $out"
}
measure idle "sleep 16; echo R idle"
measure single "SCHEDPERF_WORK=mixed ./schedperf place 1 16"
measure burst "SCHEDPERF_WORK=mixed ./schedperf burst 16.7 2000 950"
measure six "SCHEDPERF_WORK=mixed ./schedperf place 6 16"
