#!/bin/bash
# pair-linux.sh <name> <haiku sink image> "<lsend args>" [relay args]: as
# pair-lossy.sh, with a Linux sender (linuxvm kernel + busybox) as guest B
Q=/data1/scratch/net-build/qemu; L=${LINUXGUEST:-/data1/scratch/linuxguest}
cd $Q
N=$1; IMG=$2; SARGS=$3; shift 3
RARGS="${*:-100 10 0.01 ba 256 1}"
P=$((20000 + RANDOM % 20000))
LA=$P QA=$((P+1)) LB=$((P+2)) QB=$((P+3))
./mkscript.sh $Q/pl-$N-a.sh bin/nettest ${SINK:-sink 30}
sed -i "s#^/tmp/nt/testbin#IF=\$(ifconfig -a | grep -o '^/dev/net/[^ \\t]*' | head -1); ifconfig \$IF inet 10.10.0.1 netmask 255.255.255.0 up; sleep 3; /tmp/nt/testbin#" $Q/pl-$N-a.sh
$Q/mkinitrd-linux.sh $L/initrd-$N.gz $SARGS
TRACE=${TRACE:-} python3 -u relay.py $LA $QA $LB $QB $RARGS > relay-$N.log 2>&1 &
RP=$!
(EXTRA="-netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=$QA,remote.type=inet,remote.host=127.0.0.1,remote.port=$LA -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:01" IMAGE=$IMG TIMEOUT=${TIMEOUT:-300} SMP=2 SCRIPT=pl-$N-a.sh /data1/scratch/kperf/run.sh pl-$N-a > /dev/null 2>&1 &)
sleep 3
timeout ${TIMEOUT:-300} qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 -nographic -no-reboot \
	-kernel $L/x/data/linuxvm/vmlinuz -initrd $L/initrd-$N.gz -append "console=ttyS0 quiet panic=-1" \
	-netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=$QB,remote.type=inet,remote.host=127.0.0.1,remote.port=$LB \
	-device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:02 > $Q/serial-linux-$N.log 2>&1
sleep 8
kill $RP; sleep 1
grep -a "NT " $Q/serial-linux-$N.log | tr -d '\r'
strings /data1/scratch/kperf/out/serial-pl-$N-a.log | grep -a "NT \|PANIC" | sed 's/.*NT /NT /' | grep -v "^NT if"
tail -1 relay-$N.log
