#!/bin/bash
# build.sh <prog>: cross-compile a guest test against net-build's libroot/libnetwork
set -e
cd /data1/scratch/net-build/qemu/src
prog=${1:-nettest}
H=/mnt/HaikuWork/src/haiku-net/headers
INC="-I$H -I$H/posix -I$H/private/system"
for d in $H/os $H/os/*/; do INC="$INC -I$d"; done
CT=/mnt/HaikuWork/x399/build/x86_64/cross-tools-x86_64; T=x86_64-unknown-haiku
O=/data1/scratch/net-build/objects/haiku/x86_64/release
G=$O/system/glue/arch/x86_64
GL=$(ls -d $CT/lib/gcc/$T/*)
$CT/bin/$T-gcc -O2 -Wall -Wno-unused-function -nostdinc -isystem $GL/include $INC -c $prog.c -o $prog.o
$CT/bin/$T-gcc -nostdlib -o ../bin/$prog $G/crti.o $GL/crtbegin.o \
	$O/system/glue/start_dyn.o $O/system/glue/init_term_dyn.o \
	$prog.o $O/system/libroot/libroot.so $O/system/libnetwork/libnetwork.so $GL/libgcc.a \
	$GL/crtend.o $G/crtn.o -Wl,--no-undefined
echo built ../bin/$prog
