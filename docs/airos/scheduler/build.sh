#!/bin/bash
# build.sh: schedperf and cputype for x86_64 and arm64 -> out/<name>-<arch>
# Uses the CI cross tools and a configured build dir's libroot (BUILD_X86,
# BUILD_ARM64 override them).
set -e
cd "$(dirname "$0")"
mkdir -p out
H=$(cd ../../../headers && pwd)
INC="-I$H -I$H/posix -I$H/private/system"
for d in $H/os $H/os/*/; do INC="$INC -I$d"; done
for arch in x86_64 arm64; do
	case $arch in
	x86_64) CT=/data1/airos/build/haiku-x86_64/cross-tools-x86_64; T=x86_64-unknown-haiku
		O=${BUILD_X86:-/mnt/HaikuWork/build/master-x86_64}/objects/haiku/x86_64/release;;
	arm64) CT=/data1/airos/build/haiku-arm64/cross-tools-arm64; T=aarch64-unknown-haiku
		O=${BUILD_ARM64:-/mnt/HaikuWork/build/master-arm64}/objects/haiku/arm64/release;;
	esac
	G=$O/system/glue/arch/$arch
	GL=$CT/lib/gcc/$T/13.3.0
	for prog in schedperf cputype; do
		[ $prog = cputype ] && [ $arch = arm64 ] && continue
		$CT/bin/$T-gcc -O2 -Wall -nostdinc -isystem $GL/include $INC \
			-c $prog.c -o out/$prog-$arch.o
		$CT/bin/$T-gcc -nostdlib -o out/$prog-$arch $G/crti.o $GL/crtbegin.o \
			$O/system/glue/start_dyn.o $O/system/glue/init_term_dyn.o \
			out/$prog-$arch.o $O/system/libroot/libroot.so $GL/libgcc.a \
			$GL/crtend.o $G/crtn.o -Wl,--no-undefined
	done
done
ls -la out
