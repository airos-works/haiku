#!/bin/sh
# pstate-ab.sh: Zen 1 P-state scaling off vs on, same boot (runs ON the X399)
cd /boot/home
export SCHEDPERF_WORK=mixed
for state in off on; do
	./schedperf pstates $state >/dev/null
	p() { sed "s/^R /R $state /"; }
	sleep 2
	./schedperf power 10 | p
	./schedperf burst 16.7 1000 600 | grep '^R' | p
	./schedperf burst 16.7 4000 600 | grep '^R' | p
	./schedperf burst 50 10000 200 | grep '^R' | p
	./schedperf ramp -1 2 | grep '^R' | p
	./schedperf power 5 1 | grep 'R power' | p
	./schedperf power 5 4 | grep 'R power' | p
	./schedperf power 5 16 | grep 'R power' | p
	./schedperf power 5 32 | grep 'R power' | p
	for n in 1 16 32; do ./schedperf place $n 3 | grep '^R' | cut -d' ' -f1-6 | p; done
	./schedperf wake 1000 | grep '^R' | p
done
./schedperf pstates on >/dev/null
