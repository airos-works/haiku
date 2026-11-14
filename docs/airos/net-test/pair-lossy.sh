#!/bin/bash
# pair-lossy.sh <name> <image> <sender args> [relay args: rate_mbit delay_ms loss lossdir queue_kb seed]
# Guest A sinks three TCP streams; guest B sends them; relay.py between them
# (B->A is "ba"). Prints receive rates, sender retransmit counters, relay counters.
Q=/data1/scratch/net-build/qemu
cd $Q
N=$1; IMG=$2; SARGS=$3; shift 3
RARGS="${*:-100 10 0.01 ba 256 1}"
P=$((20000 + RANDOM % 20000))
LA=$P QA=$((P+1)) LB=$((P+2)) QB=$((P+3))
mk() { # $1 role, $2 address, $3 test args
	./mkscript.sh $Q/pl-$N-$1.sh bin/nettest $3
	sed -i "s#^/tmp/nt/testbin#IF=\$(ifconfig -a | grep -o '^/dev/net/[^ \\t]*' | head -1); ifconfig \$IF inet $2 netmask 255.255.255.0 up; sleep 3; /tmp/nt/testbin#" $Q/pl-$N-$1.sh
}
mk a 10.10.0.1 "sink 30"
mk b 10.10.0.2 "$SARGS"
echo 'sleep 12; netstat -s | sed -n "/^tcp/,/^udp/p" | while read l; do echo "NT $l" > /dev/dprintf; done' >> $Q/pl-$N-b.sh
TRACE=${TRACE:-} python3 -u relay.py $LA $QA $LB $QB $RARGS > relay-$N.log 2>&1 &
RP=$!
(EXTRA="-netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=$QA,remote.type=inet,remote.host=127.0.0.1,remote.port=$LA -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:01" IMAGE=$IMG TIMEOUT=${TIMEOUT:-300} SMP=2 SCRIPT=pl-$N-a.sh /data1/scratch/kperf/run.sh pl-$N-a > /dev/null 2>&1 &)
sleep 3
EXTRA="-netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=$QB,remote.type=inet,remote.host=127.0.0.1,remote.port=$LB -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:02" IMAGE=$IMG TIMEOUT=${TIMEOUT:-300} SMP=2 SCRIPT=pl-$N-b.sh /data1/scratch/kperf/run.sh pl-$N-b > /dev/null 2>&1
sleep 5
kill $RP; sleep 1
O=/data1/scratch/kperf/out
for r in a b; do strings $O/serial-pl-$N-$r.log | grep -a "NT \|PANIC" | sed 's/.*NT /NT /'; done | grep -v "^NT if"
tail -1 relay-$N.log
