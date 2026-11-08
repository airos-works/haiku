#!/bin/sh
# suite.sh [label]   (runs ON the air/OS machine) the schedperf suite in
# low latency and power saving mode; prints "R ..." result lines, prefixed
# with the label and mode, and restores the mode it found.
cd /boot/home
L=${1:-run}
N=$(sysinfo -cpu 2>/dev/null | grep -c '^CPU #')
[ "$N" -gt 0 ] || N=$(./schedperf cpus | grep -o ' c[0-9]*=' | wc -l)
H=$((N / 2))
orig=$(./schedperf mode | awk '{print $3}')
export SCHEDPERF_WORK=mixed
for mode in lowlat powersave; do
	./schedperf mode $mode >/dev/null
	sleep 1
	p() { sed "s/^R /R $L $mode /"; }
	./schedperf ramp -1 2 | grep '^R' | p
	for n in 1 2 4 $H $N; do ./schedperf place $n 3 | grep '^R' | p; done
	./schedperf burst 16.7 3000 120 | grep '^R' | p
	./schedperf burst 16.7 3000 120 $H | grep '^R' | p
	./schedperf burst 16.7 3000 120 $N | grep '^R' | p
	./schedperf wake 1000 | grep '^R' | p
	./schedperf wake 1000 $N | grep '^R' | p
done
./schedperf mode $orig >/dev/null
echo "R $L done mode=$orig"
